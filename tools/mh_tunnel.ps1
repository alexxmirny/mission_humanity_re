# mh_tunnel.ps1 -- publish this PC's game port on a VPS over an SSH reverse tunnel, with a
# TIMESTAMPED log. Canonical copy; copy it and mh_tunnel.bat next to the game and run the .bat.
# (This header used to name a tools/deploy_tunnel.py that does not exist in the tree -- fork F5B.)  # CITATION-OK
#
# WHY THIS IS NOT THE OLD `ssh -E log` ONE-LINER (rewritten 2026-08-06)
# --------------------------------------------------------------------
# `ssh -v -E <file>` writes its debug log with NO per-line timestamps. That is fine until you have to
# correlate it with anything: on 2026-08-02 an internet session produced four logs (host, two peers,
# tunnel) in which every peer connected, exchanged real traffic, and then both ends silently fired
# their 10 s link watchdog. The tunnel log showed the channels opening and closing -- but with no
# times, there was no way to line a `forwarded-tcpip` channel up against a `conn 0 dropped`, and the
# post-mortem had to fall back on file mtimes, which only date the LAST write. That is why the
# stamps exist; mh_net.log gained matching ones in the same change.
#
# The timestamping cannot be a plain `ssh ... | filter` in a .bat, because cmd's %ERRORLEVEL% after a
# pipe is the FILTER's, not ssh's -- so the retry loop would read the wrong exit code and the "did
# the forward fail?" branch would be nonsense. (Same trap as piping a validated run through `tail`;
# see the pipe-hides-the-exit-code trap.) Doing the loop in PowerShell keeps $LASTEXITCODE = ssh's.
#
# THE RELAY HOST IS NOT BAKED IN (fork F5B). It used to default to one operator's own VPS, which made
# this file unpublishable and -- worse -- made a wrong host SILENT: a missing argument connected you to
# somebody else's box instead of telling you the value was missing. So the host now comes from the
# argument or from $env:MH_TUNNEL_VPS, and with neither the script REFUSES rather than guessing. That
# is the same fail-closed shape tools/machine_config.py gives the Python side (env MH_<NAME> ->
# tools/machine.local.json -> committed default); a .ps1 deployed next to the game cannot import that
# module, so it reads the same MH_-prefixed environment variable directly.
param(
    [string]$Vps  = $env:MH_TUNNEL_VPS,
    [int]   $Port = 6501,
    [string]$LogDir = (Join-Path $PSScriptRoot "logs")
)

$ErrorActionPreference = "Continue"
if ([string]::IsNullOrWhiteSpace($Vps)) {
    Write-Error ("mh_tunnel: no relay host. Pass one (mh_tunnel.bat user@vps.example.com [port]) or " +
                 "set MH_TUNNEL_VPS. The internet-play notes says what the VPS needs (GatewayPorts).")
    exit 2
}
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$log   = Join-Path $LogDir "tunnel_$stamp.log"
Write-Host "Tunnel log: $log"
try { Start-Service ssh-agent -ErrorAction Stop } catch {}   # keys usually live in the agent

# One writer, so console and file never disagree about what happened or when.
function Write-Line([string]$text) {
    $line = "[{0}] {1}" -f (Get-Date -Format "HH:mm:ss.fff"), $text
    Write-Host $line
    Add-Content -Path $log -Value $line -Encoding utf8
}

$sshArgs = @(
    "-N", "-v",
    "-R", "0.0.0.0:${Port}:127.0.0.1:${Port}",   # 127.0.0.1, NOT localhost: on Windows that resolves
    $Vps,                                        # to ::1 first and the game listens on IPv4
    "-o", "ExitOnForwardFailure=yes",            # LOAD-BEARING: without it a failed forward leaves a
                                                 # live ssh that forwards nothing (dead-ends 2026-07-26)
    "-o", "ServerAliveInterval=30",
    "-o", "ServerAliveCountMax=3",
    "-o", "TCPKeepAlive=yes",
    "-o", "StrictHostKeyChecking=no"
)

while ($true) {
    Write-Line "=== opening tunnel: internet $Port -> this PC $Port (via $Vps) ==="
    # 2>&1 folds ssh's -v output (which goes to stderr) into the pipeline so it can be stamped.
    # $_.ToString() because a merged stderr record is an ErrorRecord, not a string.
    & ssh @sshArgs 2>&1 | ForEach-Object { Write-Line $_.ToString() }
    $rc = $LASTEXITCODE                          # ssh's own code -- the pipe does not clobber it here
    Write-Line "=== tunnel closed (exit $rc) ==="

    if ($rc -eq 255) {
        # With ExitOnForwardFailure=yes this is the "port still held on the VPS" case, and it is the
        # one failure whose fix is not "wait".
        Write-Line "  ssh exited 255. If the log says 'remote port forwarding failed', the VPS port is"
        Write-Line "  still held by an older session. On the VPS:  ss -tlnp | grep $Port   then kill that pid."
    }
    Write-Line "  retrying in 5 s (Ctrl+C to stop)"
    Start-Sleep -Seconds 5
}
