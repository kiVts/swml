@echo off
rem SWML installer: copies the kit's swml folder to %LOCALAPPDATA%\swml and opens the mod loader.
rem Your swml.ini, mod settings, logs and saved mod data are not in the kit, so they are left as they are.
setlocal
title SWML installer
set "SOURCE=%~dp0swml"
set "TARGET=%LOCALAPPDATA%\swml"

if not exist "%SOURCE%\swml.exe" goto not_extracted

:check_running
tasklist /FI "IMAGENAME eq stormworks64.exe" 2>nul | find /I "stormworks64.exe" >nul
if not errorlevel 1 goto still_running
tasklist /FI "IMAGENAME eq swml.exe" 2>nul | find /I "swml.exe" >nul
if not errorlevel 1 goto still_running

echo Installing to %TARGET%
robocopy "%SOURCE%" "%TARGET%" /E /R:3 /W:1 /NFL /NDL /NJH /NJS /NP
if errorlevel 8 goto copy_failed

echo Done. Opening the mod loader.
start "" "%TARGET%\swml.exe"
exit /b 0

:still_running
echo.
echo Stormworks or the mod loader is still running. Close it, then press a key here.
pause >nul
goto check_running

:not_extracted
echo.
echo The swml folder wasn't found next to this file.
echo Extract the whole zip to a folder first, then run install.bat from that folder.
echo.
pause
exit /b 1

:copy_failed
echo.
echo Some files could not be copied to %TARGET%. Nothing was started.
echo.
pause
exit /b 1
