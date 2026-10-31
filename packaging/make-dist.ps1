# Packages the built app and plugin with their source code for sharing (run by package.bat after the build):
#   <OutDir>\Jam Sesh <version>\              Jam Sesh.exe, Jam Sesh.vst3, README.txt, LICENSE.txt,
#                                             Licenses\ and JamSesh-<version>-Source.zip
#   <OutDir>\JamSesh-<version>-Windows.zip    the same folder, zipped
#   <OutDir>\JamSesh-<version>-Setup.exe      the installer (packaging\installer.iss), when Inno Setup 6 is
#                                             installed (or CLOD_ISCC points at its ISCC.exe)
# Jam Sesh is GPLv3 (LICENSE.txt), so every shared copy carries the source code: the
# source zip holds what's needed to build them, and nothing private (never docs\, which holds signed
# paperwork). The version comes from project(ClodScope VERSION x.y.z) in CMakeLists.txt, the app's name
# from CLOD_APP_NAME there.
#   -BuildDir  the CMake build folder holding ClodScope_artefacts (default: build)
#   -OutDir    where the folder and zip go (default: dist)
param(
    [string]$BuildDir = "build",
    [string]$OutDir = "dist"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not [IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $root $BuildDir }
if (-not [IO.Path]::IsPathRooted($OutDir)) { $OutDir = Join-Path $root $OutDir }

$cmake = Get-Content -Raw (Join-Path $root "CMakeLists.txt")
if ($cmake -notmatch 'project\(ClodScope VERSION ([0-9.]+)\)') { throw "No project version found in CMakeLists.txt" }
$version = $Matches[1]
if ($cmake -notmatch 'set\(CLOD_APP_NAME "([^"]+)"\)') { throw "No app name (CLOD_APP_NAME) found in CMakeLists.txt" }
$appName = $Matches[1]
$fileName = $appName.Replace(" ", "")

$bundle = Join-Path $BuildDir "ClodScope_artefacts\Release\VST3\$appName.vst3"
$app = Join-Path $BuildDir "ClodScope_artefacts\Release\Standalone\$appName.exe"
if (-not (Test-Path $bundle)) { throw "Build the plugin first: $bundle is missing" }
if (-not (Test-Path $app)) { throw "Build the standalone app first: $app is missing" }

$stage = Join-Path $OutDir "$appName $version"
$zip = Join-Path $OutDir "$fileName-$version-Windows.zip"
$sourceName = "$fileName-$version-Source"
$sourceZip = Join-Path $stage "$sourceName.zip"

Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

# Writes a zip from (file, entry name) pairs. Entries are added one by one: ZipFile.CreateFromDirectory
# writes backslash paths here, which the zip format forbids. A zip inside is stored, not compressed again.
function Write-Zip([string]$path, $entries) {
    if (Test-Path $path) { Remove-Item -Force $path }
    $archive = [System.IO.Compression.ZipFile]::Open($path, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($e in $entries) {
            $level = if ($e[0].EndsWith(".zip")) { [System.IO.Compression.CompressionLevel]::NoCompression }
                     else { [System.IO.Compression.CompressionLevel]::Optimal }
            [void] [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $e[0], $e[1], $level)
        }
    }
    finally {
        $archive.Dispose()
    }
}

# Copies a text file with Windows line endings (for Notepad) and the version filled in
function Write-Text([string]$from, [string]$to) {
    $text = [IO.File]::ReadAllText($from).Replace("{VERSION}", $version) -replace "`r?`n", "`r`n"
    [IO.File]::WriteAllText($to, $text)
}

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item -Recurse $bundle (Join-Path $stage "$appName.vst3")
Copy-Item $app (Join-Path $stage "$appName.exe")
Write-Text (Join-Path $PSScriptRoot "README.txt") (Join-Path $stage "README.txt")
Write-Text (Join-Path $root "LICENSE.txt") (Join-Path $stage "LICENSE.txt")

# the licences of the parts it's built from
$licenses = Join-Path $stage "Licenses"
New-Item -ItemType Directory -Force $licenses | Out-Null
$notices = [ordered]@{
    "JUCE.txt"            = "JUCE\LICENSE.md"
    "JUCE - AGPLv3.txt"   = "JUCE\LICENSE-AGPLv3.txt"
    "ASIO SDK.txt"        = "third_party\asiosdk\LICENSE.txt"
    "VST3 SDK.txt"        = "JUCE\modules\juce_audio_processors\format_types\VST3_SDK\LICENSE.txt"
    "Opus.txt"            = "third_party\opus\COPYING"
    "New Rocker font.txt" = "Source\Resources\Fonts\NewRocker-OFL.txt"
}
foreach ($n in $notices.GetEnumerator()) { Write-Text (Join-Path $root $n.Value) (Join-Path $licenses $n.Key) }

# The source code: JUCE without its examples and extras (extras\Build builds JUCE's juceaide tool), Opus
# without its neural-network model data (dnn\*_data.c: only DRED, OSCE and deep PLC use it, and they're
# off), the ASIO SDK's common folder, and the test tools without their build folders
$sourceItems = @(
    "CMakeLists.txt", "LICENSE.txt", "README.md", "build.bat", "package.bat", "install-vst3.ps1",
    "Source", "packaging", ".github\workflows",
    "tools\agent-build.ps1", "tools\compile-one.ps1", "tools\snap", "tools\jam-test", "tools\latency-probe", "tools\icon",
    "tools\plugin-test\CMakeLists.txt", "tools\plugin-test\build.bat", "tools\plugin-test\main.cpp",
    "tools\plugin-test\capture.cpp",
    "third_party\opus",
    "third_party\asiosdk\common", "third_party\asiosdk\LICENSE.txt", "third_party\asiosdk\README.md",
    "third_party\asiosdk\changes.txt",
    "JUCE\CMakeLists.txt", "JUCE\LICENSE.md", "JUCE\LICENSE-AGPLv3.txt", "JUCE\README.md", "JUCE\modules",
    "JUCE\extras\Build")
$building = Join-Path $stage "BUILDING.txt"
Write-Text (Join-Path $PSScriptRoot "BUILDING.txt") $building
$sourceFiles = [System.Collections.Generic.List[object]]::new()
$sourceFiles.Add(@($building, "$sourceName/BUILDING.txt"))
foreach ($item in $sourceItems) {
    $full = Join-Path $root $item
    if (-not (Test-Path $full)) { throw "Source code: $item is missing" }
    $files = if (Test-Path $full -PathType Container) { Get-ChildItem -Recurse -File $full } else { Get-Item $full }
    foreach ($f in $files) {
        $rel = $f.FullName.Substring($root.Length + 1)
        if ($rel -match '^third_party\\opus\\dnn\\[^\\]+_data\.c$') { continue }
        $name = "$sourceName/" + $rel.Replace("\", "/")
        $sourceFiles.Add(@($f.FullName, $name))
    }
}
Write-Zip $sourceZip $sourceFiles
Remove-Item $building

# the zip holds the "<app> <version>" folder itself, so it unpacks into one folder
$stageFiles = [System.Collections.Generic.List[object]]::new()
foreach ($f in Get-ChildItem -Recurse -File $stage) {
    $name = (Split-Path -Leaf $stage) + "/" + $f.FullName.Substring($stage.Length + 1).Replace("\", "/")
    $stageFiles.Add(@($f.FullName, $name))
}
Write-Zip $zip $stageFiles

# the installer, from the same staged folder
$iscc = @($env:CLOD_ISCC, "G:\clod codes\toolchains\InnoSetup6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
          "$env:ProgramFiles\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") |
        Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
$setup = Join-Path $OutDir "$fileName-$version-Setup.exe"
if ($iscc) {
    & $iscc /Q "/DAppVersion=$version" "/DStage=$stage" "/DOutDir=$OutDir" (Join-Path $PSScriptRoot "installer.iss")
    if ($LASTEXITCODE -ne 0) { throw "The installer didn't build (ISCC exit $LASTEXITCODE)" }
}

"Packaged $appName $version"
"  folder: $stage"
"  zip:    $zip ({0:N1} MB, of which source code {1:N1} MB, {2} files)" -f ((Get-Item $zip).Length / 1MB), ((Get-Item $sourceZip).Length / 1MB), $sourceFiles.Count
if ($iscc) { "  setup:  $setup ({0:N1} MB)" -f ((Get-Item $setup).Length / 1MB) }
else { "  setup:  skipped (Inno Setup 6 isn't installed)" }
