# Copies the built plugin into the standard VST3 folder (FL Studio only scans .vst3 there).
# If FL has the old binary loaded it can't be overwritten, but it can be renamed,
# so the old file is moved aside and the new one takes effect on FL's next start.
param([string]$Source)

$destRoot = "C:\Program Files\Common Files\VST3"
$dest = Join-Path $destRoot "Clod Scope.vst3"
$binDir = Join-Path $dest "Contents\x86_64-win"
$bin = Join-Path $binDir "Clod Scope.vst3"

New-Item -ItemType Directory -Force $binDir | Out-Null
Get-ChildItem $binDir -Filter "*.old*" -ErrorAction SilentlyContinue | ForEach-Object { Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue }
if (Test-Path $bin) {
    try { Remove-Item $bin -Force -ErrorAction Stop }
    catch { Rename-Item $bin ("Clod Scope.vst3.old" + (Get-Date -Format "HHmmss")) -Force }
}
Copy-Item -Recurse -Force (Join-Path $Source "*") $dest
