@echo off
REM Cddacoop launcher (Windows).
REM
REM Mirrors the macOS .app's osascript launcher: presents a Host / Join /
REM Single-player picker and offers to download the CC-Sounds pack on first
REM launch.  Sits alongside cataclysm-tiles.exe in the portable ZIP.

setlocal enabledelayedexpansion
title Cddacoop
cd /d "%~dp0"

REM ---- First-run sound pack check ------------------------------------------

set "USER_SOUND_DIR=%APPDATA%\Cataclysm\sound"
set "USER_CC=%USER_SOUND_DIR%\CC-Sounds"
set "BUNDLE_CC=%~dp0data\sound\CC-Sounds"
REM /releases/latest/download/<name> is a GitHub redirect to the latest
REM non-draft release's matching asset.  Works once v0.1.0 ships.
set "CC_URL=https://github.com/busterbogheart/Cataclysm-DDA-multi/releases/latest/download/cc-sounds.zip"

if not exist "%BUNDLE_CC%" if not exist "%USER_CC%" (
    echo.
    echo   CC-Sounds pack not installed ^(~135 MB^).
    echo   Skip to play silently; download to install.
    echo.
    choice /c SD /n /m "  Press [S]kip or [D]ownload: "
    if !ERRORLEVEL!==2 (
        if not exist "%USER_SOUND_DIR%" mkdir "%USER_SOUND_DIR%"
        echo.
        echo   Downloading...
        curl -L --fail "%CC_URL%" -o "%TEMP%\cc-sounds.zip"
        if !ERRORLEVEL!==0 (
            echo   Extracting...
            powershell -NoProfile -Command "Expand-Archive -Force '%TEMP%\cc-sounds.zip' '%USER_SOUND_DIR%'"
            del "%TEMP%\cc-sounds.zip" >nul 2>&1
            echo.
            echo   Done.  Re-run cddacoop.bat to play with sound.
            pause
            exit /b 0
        ) else (
            echo   Download failed.  Continuing without sound pack.
            echo.
            pause
        )
    )
)

REM ---- Main menu ------------------------------------------------------------

:menu
cls
echo.
echo   ===================
echo    C D D A C O O P
echo   ===================
echo.
echo   1. Single-player
echo   2. Host a session
echo   3. Join a session
echo   Q. Quit
echo.
choice /c 123Q /n /m "  Pick a mode: "
set "CHOICE=!ERRORLEVEL!"

if "!CHOICE!"=="1" goto single
if "!CHOICE!"=="2" goto host
if "!CHOICE!"=="3" goto join
if "!CHOICE!"=="4" exit /b 0
goto menu

:single
"%~dp0cataclysm-tiles.exe"
exit /b 0

:host
"%~dp0cataclysm-tiles.exe" --host
exit /b 0

:join
echo.
set "IP="
set /p "IP=  Host address (e.g. 100.64.0.5 or host.com:8080): "
if "!IP!"=="" goto menu
REM Default to port 8080 when the user didn't include one.
echo !IP! | findstr ":" >nul
if errorlevel 1 set "IP=!IP!:8080"
"%~dp0cataclysm-tiles.exe" --client !IP!
exit /b 0
