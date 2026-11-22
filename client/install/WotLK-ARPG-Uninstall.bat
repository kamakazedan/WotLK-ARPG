@echo off
rem Puts the original DLL name back into Wow.exe in this folder.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0WotLK-ARPG-Setup.ps1" -Uninstall %*
echo.
pause
