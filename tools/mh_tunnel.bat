@echo off
rem Thin wrapper -- the logic lives in mh_tunnel.ps1 beside this file. Kept as a .bat so the way you
rem START the tunnel is unchanged (double-click / `mh_tunnel.bat`), while the loop itself runs in
rem PowerShell, where $LASTEXITCODE after a pipe is SSH's and not the log-stamping filter's.
rem
rem Optional args:  mh_tunnel.bat [user@vps] [port]
rem
rem The relay host is NOT baked in (fork F5B): argument first, then %MH_TUNNEL_VPS%, and with neither
rem the .ps1 refuses rather than connecting to whatever host happened to be committed here.
setlocal
set "VPS=%~1"
set "PORT=%~2"
if "%VPS%"==""  set "VPS=%MH_TUNNEL_VPS%"
if "%PORT%"=="" set "PORT=6501"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0mh_tunnel.ps1" -Vps "%VPS%" -Port %PORT%
endlocal
