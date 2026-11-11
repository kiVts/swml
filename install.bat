@echo off
setlocal
title SWML installer
if not exist "%~dp0swml_startup_setup.exe" goto not_extracted
"%~dp0swml_startup_setup.exe" --install
if errorlevel 1 goto failed
start "" "%LOCALAPPDATA%\swml\swml.exe"
exit /b 0
:not_extracted
echo Extract the entire SWML ZIP before running install.bat.
pause
exit /b 1
:failed
echo SWML was not installed. Close Stormworks and SWML, then try again.
pause
exit /b 1
