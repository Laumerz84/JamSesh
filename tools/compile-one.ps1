# Compiles ONE source file of the plugin with the exact flags of the main build, into a private
# object folder, without touching the main build. Safe to run from several agents at once.
#   powershell -ExecutionPolicy Bypass -File "G:\clod codes\fl-scope-plugin\tools\compile-one.ps1" Source\Orbital\OrbitalModel.cpp
# Prints compiler errors/warnings; exit code 0 = compiled cleanly.
param([Parameter(Mandatory = $true)][string]$Source)

$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root "build"
$full = (Resolve-Path (Join-Path $root $Source) -ErrorAction SilentlyContinue)
if (-not $full) { $full = Resolve-Path $Source }
$full = $full.Path

$db = Get-Content (Join-Path $buildDir "compile_commands.json") -Raw | ConvertFrom-Json
$entry = $db | Where-Object { ($_.file -replace '/', '\') -ieq $full } | Select-Object -First 1
if (-not $entry) { Write-Error "Not in compile_commands.json: $full (ask the integrator to re-run CMake)"; exit 2 }

$name = [IO.Path]::GetFileNameWithoutExtension($full)
$objDir = Join-Path $root "tools\obj"
New-Item -ItemType Directory -Force $objDir | Out-Null
$obj = Join-Path $objDir "$name.obj"
$pdb = Join-Path $objDir "$name.pdb"

# Split "cl.exe <args>" and redirect the outputs
$cmd = $entry.command
$m = [regex]::Match($cmd, '^\s*("[^"]+"|\S+)\s+(.*)$')
$compiler = $m.Groups[1].Value
$cmdArgs = $m.Groups[2].Value
$cmdArgs = [regex]::Replace($cmdArgs, '/Fo\S+', "/Fo`"$obj`"")
$cmdArgs = [regex]::Replace($cmdArgs, '/Fd\S+', "/Fd`"$pdb`"")
$cmdArgs = $cmdArgs -replace '/showIncludes', ''
$rsp = Join-Path $objDir "$name.rsp"
Set-Content -Path $rsp -Value $cmdArgs -Encoding ascii

$vcvars = "G:\clod codes\toolchains\VS2022-BuildTools\VC\Auxiliary\Build\vcvars64.bat"
$bat = Join-Path $objDir "$name.bat"
Set-Content -Path $bat -Encoding ascii -Value @"
@echo off
call "$vcvars" >nul 2>nul
cd /d "$($entry.directory)"
$compiler @"$rsp"
"@
cmd /c "`"$bat`""
$code = $LASTEXITCODE
if ($code -eq 0) { Write-Output "OK: $name compiled" } else { Write-Output "FAILED: $name (exit $code)" }
exit $code
