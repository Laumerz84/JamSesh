# Builds one target in a private build folder (build-<Name>) at below-normal priority, so several agents
# can build at once without touching the lead's build\ folder or slowing the owner's games.
#   powershell -ExecutionPolicy Bypass -File "G:\clod codes\fl-scope-plugin\tools\agent-build.ps1" -Name net -Target ClodJamTest
# Targets: ClodScope_VST3, ClodScope_Standalone, ClodScopeSnap, ClodJamTest. The first build of a folder
# compiles JUCE and takes several minutes; later builds only what changed. Prints errors and warnings,
# exit code 0 = built. Output files: build-<Name>\<target>_artefacts\Release\...
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][string]$Target,
    [int]$Jobs = 3
)

$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root "build-$Name"
$vcvars = "G:\clod codes\toolchains\VS2022-BuildTools\VC\Auxiliary\Build\vcvars64.bat"
New-Item -ItemType Directory -Force $dir | Out-Null
$log = Join-Path $dir "agent-build.log"

$configure = ""
if (-not (Test-Path (Join-Path $dir "build.ninja"))) {
    $configure = "cmake -S `"$root`" -B `"$dir`" -G Ninja -DCMAKE_BUILD_TYPE=Release && "
}
$cmd = "call `"$vcvars`" >nul 2>nul && $configure cmake --build `"$dir`" --target $Target -j $Jobs"

$p = Start-Process -FilePath cmd.exe -ArgumentList "/c", "$cmd > `"$log`" 2>&1" -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 200
try { $p.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::BelowNormal } catch {}
$p.WaitForExit()

Select-String -Path $log -Pattern "error|warning C|FAILED|CMake Error" | Select-Object -First 60 | ForEach-Object { $_.Line }
if ($p.ExitCode -eq 0) { "BUILT: $Target in build-$Name" } else { "BUILD FAILED: $Target (exit $($p.ExitCode)); full log: $log" }
exit $p.ExitCode
