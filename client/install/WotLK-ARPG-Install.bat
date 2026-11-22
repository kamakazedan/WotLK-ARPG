@echo off
rem Makes Wow.exe in this folder load WotLK-ARPG.dll. See WotLK-ARPG-Setup.ps1 for what it changes.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0WotLK-ARPG-Setup.ps1" %*
echo.
pause
