# The round's end-to-end check in one command: builds ClodJamTest and ClodScopeSnap in build-test (below-normal
# priority, via agent-build.ps1), runs every Jam Sesh case (hidden child processes, 127.0.0.1 only) and every
# snapshot mode (off-screen, no window), and writes <OutDir>\summary.txt.
#   powershell -ExecutionPolicy Bypass -File "G:\clod codes\fl-scope-plugin\tools\jam-test\run-all.ps1" [-OutDir <dir>] [-Quick] [-Cases "connect2 latency"] [-NoBuild]
# -Quick shortens the long cases (drift 40 s instead of 3 min); the full run takes about 12 minutes.
param(
    [string]$OutDir = "",
    [switch]$Quick,
    [string]$Cases = "",
    [switch]$NoBuild
)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if ($OutDir -eq "") { $OutDir = Join-Path $root ("build-test\e2e-" + (Get-Date -Format "yyyyMMdd-HHmm")) }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$summary = Join-Path $OutDir "summary.txt"
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("Clod Scope end-to-end check, $(Get-Date -Format 'yyyy-MM-dd HH:mm'), $OutDir")

# never all interfaces (Windows Firewall would ask); snapshots never save the owner's theme
$env:CLOD_JAM_BIND = "127.0.0.1"
if (-not $env:CLOD_THEME) { $env:CLOD_THEME = "0" }
$env:CLOD_JAM_NETSIM = ""

$build = Join-Path $root "tools\agent-build.ps1"
$jamExe = Join-Path $root "build-test\ClodJamTest_artefacts\Release\ClodJamTest.exe"
$snapExe = Join-Path $root "build-test\ClodScopeSnap_artefacts\Release\ClodScopeSnap.exe"

if (-not $NoBuild) {
    foreach ($t in @("ClodJamTest", "ClodScopeSnap")) {
        $out = & powershell -ExecutionPolicy Bypass -File $build -Name test -Target $t -Jobs 4 2>&1
        $code = $LASTEXITCODE
        $lines.Add("build ${t}: " + $(if ($code -eq 0) { "ok" } else { "FAILED (exit $code)" }))
        if ($code -ne 0) {
            $out | Select-Object -Last 30 | ForEach-Object { $lines.Add("    $_") }
        }
    }
}

function Run-Hidden([string]$exe, [string[]]$arguments, [string]$log) {
    # console tools started hidden; stdout to a file
    $quoted = $arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
    $p = Start-Process -FilePath $exe -ArgumentList $quoted -WindowStyle Hidden -PassThru -Wait `
        -RedirectStandardOutput $log -RedirectStandardError ($log + ".err")
    return $p.ExitCode
}

# ---- Jam Sesh cases ----
if (Test-Path $jamExe) {
    $jamDir = Join-Path $OutDir "jam"
    $a = @($jamDir)
    if ($Quick) { $a += "--quick" }
    if ($Cases -ne "") { $a += ($Cases -split '\s+') }
    $code = Run-Hidden $jamExe $a (Join-Path $OutDir "jam-stdout.txt")
    $lines.Add("")
    $lines.Add("== Jam Sesh (ClodJamTest, exit $code; per-player results and logs in $jamDir) ==")
    $js = Join-Path $jamDir "summary.txt"
    if (Test-Path $js) { Get-Content $js | ForEach-Object { $lines.Add($_) } } else { $lines.Add("no jam summary written") }
} else {
    $lines.Add("ClodJamTest not built: $jamExe")
}

# ---- snapshots ----
if (Test-Path $snapExe) {
    $snapDir = Join-Path $OutDir "snap"
    $code = Run-Hidden $snapExe @($snapDir, "all", "--jamtest", $jamExe) (Join-Path $OutDir "snap-stdout.txt")
    $lines.Add("")
    $lines.Add("== Snapshots (ClodScopeSnap all, exit $code; PNGs in $snapDir) ==")
    Get-Content (Join-Path $OutDir "snap-stdout.txt") | Where-Object { $_ -notmatch '^\s+wrote ' -or $_ -match ';' } |
        ForEach-Object { $lines.Add($_) }
    $pngs = @(Get-ChildItem $snapDir -Filter *.png -ErrorAction SilentlyContinue).Count
    $lines.Add("$pngs PNGs written")
} else {
    $lines.Add("ClodScopeSnap not built: $snapExe")
}

$lines | Set-Content -Path $summary -Encoding utf8
$lines | ForEach-Object { $_ }
"summary: $summary"
