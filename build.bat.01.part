@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

REM ============================================================
REM  ClipAgent build script. Win7 x64 / Win11 x64, zero dependency.
REM  Keep this file PURE ASCII, NO BOM. cmd.exe breaks on the BOM.
REM  Sources are pure ASCII too; /utf-8 is a harmless safety net.
REM ============================================================

set "CFG=release"
set "ARCH=x64"
set "ARTIFACT=ClipAgent.exe"

if not exist release mkdir release

where cl >nul 2>&1
if errorlevel 1 (
    echo [X] cl.exe not found on PATH.
    echo [i] Run this from a VS/x64 Native Tools Command Prompt, or call:
    echo     "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    exit /b 1
)

echo [*] compiler :
where cl
echo [*] config   : %CFG% / %ARCH% / static /MT
echo.

REM ---- compile + link in one step. /utf-8 = BOM-free safety net. ----
cl /nologo /W3 /O2 /MT /utf-8 /EHsc /GS /Gy /DNDEBUG ^
   /D_WIN32_WINNT=0x0601 /DWINVER=0x0601 ^
   /D_UNICODE /DUNICODE ^
   /Fo"release\\" /Fe"release\%ARTIFACT%" ^
   ClipAgent.cpp miniz.c ^
   /link /SUBSYSTEM:CONSOLE,6.01 ^
         /OPT:REF /OPT:ICF ^
         kernel32.lib user32.lib shell32.lib gdi32.lib ^
         ws2_32.lib rstrtmgr.lib ole32.lib oleaut32.lib uuid.lib ^
         advapi32.lib
if errorlevel 1 (
    echo.
    echo [X] build FAILED.
    echo [i] Map the error line to a part with the merge.bat row table:
    echo     merge.bat --no-build
    exit /b 1
)

echo.
echo [+] built: release\%ARTIFACT%
echo [*] static runtime (/MT): no vcruntime DLL needed on Win7.
echo.

REM ---- optional signing, same flow as MonDrv7.sys / MonClient.exe ----
set "SIGN_NAME=Your Name or Org"
set "SIGN_TS=/tr http://timestamp.digicert.com /td sha256"
if defined SIGN_PFX (
    if exist "%SIGN_PFX%" (
        echo [*] signing with %SIGN_PFX%
        signtool sign /f "%SIGN_PFX%" /p "%SIGN_PWD%" ^
                      /n "%SIGN_NAME%" %SIGN_TS% /fd sha256 /a ^
                      "release\%ARTIFACT%"
        if not errorlevel 1 (
            signtool verify /pa "release\%ARTIFACT%"
            echo [+] signed and verified
        ) else (
            echo [!] signing failed, unsigned binary kept
        )
    )
) else (
    echo [i] SIGN_PFX not set^; skipping code signing.
    echo     set SIGN_PFX=your.pfx ^& set SIGN_PWD=... ^& build.bat
)

echo.
echo [*] next: merge.bat to assemble + build, or run ^^^^
echo       release\ClipAgent.exe --listen 8080 --key mon --outdir C:\ClipInbox
exit /b 0