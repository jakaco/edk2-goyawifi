@echo off
rem goya-diag.cmd <W:> <action> [arg]  - offline diagnostics for Windows RT on the goyawifi card.
rem Elevated. Actions: info, collect, verifier-off, bootlog on|off, minidump on, disable|enable <service>.
net session >nul 2>&1 || (echo Needs an elevated prompt ^(Run as administrator^). & exit /b 1)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0goya-diag.ps1" -Win %1 -Action %2 -Arg "%~3"
