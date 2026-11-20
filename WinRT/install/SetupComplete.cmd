@echo off
rem Runs once at the end of Windows setup on the tablet. Writes C:\goya\diag.txt
rem (read it on a PC from the Windows partition of the card).
if not exist C:\goya mkdir C:\goya
set OUT=C:\goya\diag.txt
echo === %DATE% %TIME% > %OUT%
ver >> %OUT%
echo %NUMBER_OF_PROCESSORS% processors, %PROCESSOR_ARCHITECTURE% >> %OUT%
echo --- drivers >> %OUT%
driverquery /v /fo list >> %OUT% 2>&1
echo --- devices (ConfigManagerErrorCode 0 = OK) >> %OUT%
wmic path Win32_PnPEntity get Name,ConfigManagerErrorCode,DeviceID /format:list >> %OUT% 2>&1
echo --- HAL extensions >> %OUT%
reg query "HKLM\SYSTEM\CurrentControlSet\Control\OSExtensionDatabase" /s >> %OUT% 2>&1
echo --- disks >> %OUT%
echo list disk > %TEMP%\dp.txt
echo list volume >> %TEMP%\dp.txt
diskpart /s %TEMP%\dp.txt >> %OUT% 2>&1
echo --- setup log tail >> %OUT%
powershell -NoProfile -Command "Get-Content C:\Windows\Panther\setupact.log -Tail 80" >> %OUT% 2>&1
