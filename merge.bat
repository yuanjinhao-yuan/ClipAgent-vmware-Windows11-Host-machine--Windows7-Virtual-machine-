@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

set "PARTDIR=parts"
set "TARGETS=ClipProtocol.h ClipAgent.cpp miniz.c miniz.h build.bat run-host.bat run-guest.bat install-autorun.bat README.txt"
set "DO_BUILD=1"

if /i "%~1"=="--no-build" set "DO_BUILD=0"
if /i "%~1"=="clean"      goto :CLEAN
if not exist "%PARTDIR%" mkdir "%PARTDIR%"

echo [*] merge.bat -- one part per turn, no full-source review
echo [*] naming: parts\^<target^>.^<NN^>.part   NN=01,02,...
echo.

rem ---- 1) preflight ----
set "MISSING=0"
for %%T in (%TARGETS%) do (
    set "CNT=0"
    for /f "delims=" %%P in ('dir /b /o:n "%PARTDIR%\%%T.*.part" 2^>nul') do set /a CNT+=1
    if !CNT!==0 (
        if exist "%%T" (
            echo [have] %%T  ^(full file, no parts^)
        ) else (
            echo [wait] %%T  -- no parts yet
            set "MISSING=1"
        )
    ) else (
        echo [have] %%T  -- !CNT! part^(s^)
    )
)
if "%MISSING%"=="1" (
    echo.
    echo [i] not all files delivered yet. Normal between turns.
    echo [i] run merge.bat --no-build after each part to check integrity.
    exit /b 0
)

rem ---- 2) assemble ----
echo.
echo [*] assembling ------------------------------------------
for %%T in (%TARGETS%) do call :ASSEMBLE "%%T"
if errorlevel 1 exit /b 1

rem ---- 3) final mechanical checks, NO code review ----
echo.
echo [*] mechanical checks ------------------------------------
for %%T in (%TARGETS%) do (
    findstr /n /c:"`" "%%T" >nul
    if not errorlevel 1 echo   [!] backtick 0x60 in %%T -- see row table above
)
echo   [ok] byte sums match, no full-source read performed

echo.
echo [*] row table: compile error line --^> which part to resend
echo     ^(printed above as [start..end] ^<- partname^)

if "%DO_BUILD%"=="0" (
    echo.
    echo [+] --no-build: assembled only.
    exit /b 0
)

echo.
if exist build.bat (
    echo [*] calling build.bat
    call build.bat
) else (
    echo [!] build.bat not present yet
)
exit /b 0


:ASSEMBLE
set "T=%~1"
set "CNT=0"
for /f "delims=" %%P in ('dir /b /o:n "%PARTDIR%\%T%.*.part" 2^>nul') do set /a CNT+=1
if "%CNT%"=="0" goto :EOF

if exist "%T%" copy /y "%T%" "%T%.bak" >nul
set "OUT=%T%.merging"
if exist "%OUT%" del /q "%OUT%"
if exist "%OUT%.tmp" del /q "%OUT%.tmp"

set "FIRST=1"
set "TOTAL=0"
set "LINE=0"
for /f "delims=" %%P in ('dir /b /o:n "%PARTDIR%\%T%.*.part"') do (
    set "F=%PARTDIR%\%%P"
    for %%A in ("!F!") do set "SZ=%%~zA"
    if "!FIRST!"=="1" (
        copy /b /y "!F!" "%OUT%" >nul
        set "FIRST=0"
    ) else (
        copy /b /y "%OUT%"+"!F!" "%OUT%.tmp" >nul
        move /y "%OUT%.tmp" "%OUT%" >nul
    )
    set /a TOTAL+=!SZ!
    for /f %%L in ('find /c /v "" ^< "!F!"') do set "NL=%%L"
    set /a START=LINE+1
    set /a LINE+=NL
    echo   %T%  [!START!..!LINE!]  ^<- %%P  ^(!SZ! B^)
)
move /y "%OUT%" "%T%" >nul
for %%A in ("%T%") do set "GOT=%%~zA"
if not "!GOT!"=="!TOTAL!" (
    echo   [X] SIZE MISMATCH %T% : expected !TOTAL! got !GOT!
    echo       re-paste that one part only; do not touch the others.
    exit /b 1
)
goto :EOF


:CLEAN
for %%T in (%TARGETS%) do (
    if exist "%%T" del /q "%%T"
)
for %%T in (%TARGETS%) do (
    if exist "%%T.merging" del /q "%%T.merging"
    if exist "%%T.merging.tmp" del /q "%%T.merging.tmp"
)
echo [*] generated files removed. parts\ kept.
exit /b 0