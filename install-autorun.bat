@echo off
REM Install / remove HKCU autorun for ClipAgent. No admin needed.
REM PURE ASCII, NO BOM.
setlocal EnableDelayedExpansion
cd /d "%~dp0"

set "APPNAME=ClipAgent"
set "RUNKEY=HKCU\Software\Microsoft\Windows\CurrentVersion\Run"
set "MODE=%~1"
set "HOST_IP=192.168.1.50"
set "PORT=8080"
set "KEY=mon"
set "OUTDIR=C:\ClipInbox"

if /i "%MODE%"=="--remove" goto REMOVE
if /i "%MODE%"=="-r"      goto REMOVE

set "EXEP=%~dp0release\ClipAgent.exe"
if not exist "%EXEP%" (
    echo [X] %EXEP% not found. Build first.
    exit /b 1
)
set "VAL="%EXEP%" --connect %HOST_IP%:%PORT% --key %KEY% --outdir "%OUTDIR%" --quiet"
reg add "%RUNKEY%" /v "%APPNAME%" /t REG_SZ /d %VAL% /f >nul
if errorlevel 1 (
    echo [X] reg add failed
    exit /b 1
)
echo [+] autorun installed ^(guest mode^):
echo     %VAL%
echo [i] edit HOST_IP/PORT/KEY/OUTDIR at the top of this script first.
echo [i] remove with: install-autorun.bat --remove
exit /b 0

:REMOVE
reg delete "%RUNKEY%" /v "%APPNAME%" /f >nul 2>&1
echo [+] autorun entry removed if it existed
exit /b 0