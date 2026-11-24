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

rem ---- goyasdhc speed test (bench.flag next to this file turns it on) -------------------------------
rem Reads \sources\boot.wim (the WinPE image, ~190 MB) from this FAT32 partition, then copies it to the
rem NTFS partition (write) and reads that copy back. Start/end times go to diag.txt.
if not exist "%~dp0bench.flag" goto nobench
set WIN=
for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if not defined WIN if exist %%d:\Windows\System32\config\SYSTEM if /i not "%%d:"=="%~d0" set WIN=%%d:
echo --- goyasdhc speed test >> "%OUT%"
for %%F in ("%~d0\sources\boot.wim") do echo file %%~fF  %%~zF bytes >> "%OUT%"
echo read  FAT32 start %TIME% >> "%OUT%"
copy /b "%~d0\sources\boot.wim" nul >nul
echo read  FAT32 end   %TIME% >> "%OUT%"
if not defined WIN (echo no Windows partition found, write test skipped >> "%OUT%" & goto benchdone)
if not exist %WIN%\goya mkdir %WIN%\goya
echo write NTFS %WIN% start %TIME% >> "%OUT%"
copy /b /y "%~d0\sources\boot.wim" %WIN%\goya\bench.tmp >nul
echo write NTFS %WIN% end   %TIME% >> "%OUT%"
echo read  NTFS %WIN% start %TIME% >> "%OUT%"
copy /b %WIN%\goya\bench.tmp nul >nul
echo read  NTFS %WIN% end   %TIME% >> "%OUT%"
fc /b "%~d0\sources\boot.wim" %WIN%\goya\bench.tmp >nul && (echo verify: copy identical >> "%OUT%") || (echo verify: COPY DIFFERS >> "%OUT%")
del %WIN%\goya\bench.tmp
:benchdone
echo --- end %TIME% >> "%OUT%"
:nobench

rem ---- twrp.flag next to this file: restart straight into TWRP (the firmware sees \goya\reboot-recovery) ----
if not exist "%~dp0twrp.flag" goto :eof
if not exist "%~d0\goya" mkdir "%~d0\goya"
echo WinPE autorun > "%~d0\goya\reboot-recovery"
wpeutil reboot
