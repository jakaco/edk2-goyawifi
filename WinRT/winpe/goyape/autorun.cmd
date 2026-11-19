@echo off
rem autorun.cmd - runs at every WinPE boot from the SD card. Edit it on the PC.
rem Writes a diagnostics file next to it so the card can be sent back for analysis.
set OUT=%~dp0diag.txt
echo === %DATE% %TIME% > "%OUT%"
echo --- cpu / os >> "%OUT%"
echo %NUMBER_OF_PROCESSORS% processors, %PROCESSOR_ARCHITECTURE% >> "%OUT%"
ver >> "%OUT%"
echo --- drivers >> "%OUT%"
driverquery /v /fo list >> "%OUT%" 2>&1
echo --- devices ^(problem codes^) >> "%OUT%"
wmic path Win32_PnPEntity get Name,ConfigManagerErrorCode,DeviceID /format:list >> "%OUT%" 2>&1
echo --- timers ^(HAL extension^) >> "%OUT%"
reg query "HKLM\SYSTEM\CurrentControlSet\Control\OSExtensionDatabase" /s >> "%OUT%" 2>&1
echo --- boot log >> "%OUT%"
if exist X:\Windows\ntbtlog.txt type X:\Windows\ntbtlog.txt >> "%OUT%"
rem ---- your own commands below ----
