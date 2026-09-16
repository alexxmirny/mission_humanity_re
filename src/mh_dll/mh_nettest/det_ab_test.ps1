# det_ab_test.ps1 -- fully-scripted same-machine determinism test for mh.exe (the replay-harness notes).
#
# Runs the replay harness twice against the SAME fixture save and proves the strategic sim is
# replay-deterministic: pass A dumps the full sim-state blob (incl. RNG) at seed_step, pass B injects
# it for a byte-identical start; then the per-step STATE hashes (log field 4, excludes the cosmetic fx
# RNG channel) are compared. Identical => deterministic.
#
# Requires (in $Dir): the launch build -- mh.launch.exe (launch_load import-only patch) + mh.dll (the
# composing build: launch auto-drives --load, harness hooks the sim) + mh_net.ini carrying a
# `[harness] enable=1` block (ONE file since fork F2G) + the save. The
# per-exe Windows compat shim must be applied to mh.launch.exe (see the deploy-compat-shim note).
#
# This is the D17 payoff: --load auto-loads the fixture, so NO human "click Load" is needed (the old
# procedure in the replay-harness notes required it). Each pass exits itself (exit_on_stop=1).
#
# Usage:  powershell -File det_ab_test.ps1 [-Dir <game-install>] [-Save 4-saibel-4] [-STOP 2000]
#   -Dir defaults to $env:MH_DET_DIR, else this box's determinism install.
# Cross-vendor (Test 2): run pass A on machine 1, copy mh_harness_seed.bin to machine 2, run pass B
# there with seed_mode=1, and diff the two _det_*.log state columns (see the replay-harness notes).

param(
    [string]$Dir  = $(if ($env:MH_DET_DIR) { $env:MH_DET_DIR } else { 'F:\games\mh_amd' }),
    [string]$Save = '4-saibel-4',
    [int]   $STOP = 2000
)

$ini  = Join-Path $Dir 'mh_net.ini'   # fork F2G: [harness] lives here now, armed by enable=1
$log  = Join-Path $Dir 'mh_harness.log'
$seed = Join-Path $Dir 'mh_harness_seed.bin'
$exe  = Join-Path $Dir 'mh.launch.exe'
$gargs = @('--load', $Save, '--skip-intro')

function RunPass([int]$mode, [string]$tag) {
    (Get-Content $ini) -replace '^seed_mode=.*', "seed_mode=$mode" -replace '^stop_step=.*', "stop_step=$STOP" |
        Set-Content $ini -Encoding ASCII
    if (Test-Path $log) { Remove-Item $log -Force }
    Get-Process mh.launch,mh,mh.harness -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $exe -ArgumentList $gargs -WorkingDirectory $Dir -PassThru
    $ok = $p.WaitForExit(300000); $sw.Stop()
    if (-not $ok) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Write-Output "$tag TIMEOUT" }
    else { Write-Output ("{0}: exited in {1:N1}s (code {2})" -f $tag, $sw.Elapsed.TotalSeconds, $p.ExitCode) }
    Start-Sleep -Seconds 1
    Copy-Item $log (Join-Path $Dir "_det_$tag.log") -Force -ErrorAction SilentlyContinue
}

function StateMap([string]$file) {
    $m = @{}
    foreach ($l in (Get-Content $file)) {
        if ($l.StartsWith(';')) { continue }
        if ($l.Trim().Length -eq 0) { continue }
        $x = $l -split '\s+'
        if ($x.Count -ge 4) { $m[$x[0]] = $x[3] }
    }
    return $m
}

if (Test-Path $seed) { Remove-Item $seed -Force }
RunPass 0 'A'   # dump the seed blob (incl RNG) at seed_step
RunPass 1 'B'   # inject A's seed -> byte-identical start

$ma = StateMap (Join-Path $Dir '_det_A.log')
$mb = StateMap (Join-Path $Dir '_det_B.log')
$mis = 0; $chk = 0
foreach ($k in $ma.Keys) {
    if ($mb.ContainsKey($k)) {
        $chk++
        if ($ma[$k] -ne $mb[$k]) { $mis++; if ($mis -le 5) { Write-Output "MISMATCH step $k A=$($ma[$k]) B=$($mb[$k])" } }
    }
}
Write-Output "A=$($ma.Count) lines, B=$($mb.Count) lines, compared=$chk, mismatches=$mis"
if ($chk -gt 0 -and $mis -eq 0) { Write-Output '=== SAME-MACHINE DETERMINISTIC: state hash identical A vs B ===' }
else { Write-Output '=== DIVERGENCE or no data -- investigate ===' }
