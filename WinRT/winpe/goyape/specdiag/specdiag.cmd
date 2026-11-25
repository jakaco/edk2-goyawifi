@echo off
rem Runs inside Windows RT Setup's specialize pass (unattend RunSynchronous, as SYSTEM), just before
rem Setup's own BCD update (ModifyBootEntries). Writes C:\goya\specdiag.txt, which WinPE's
rem setuplogs.flag copies to the card. Checks that Windows takes the ESP's BCD as the system store
rem (it needs Description\System and Description\TreatAsSystem, see fixbcd.flag in autorun.cmd).
rem Always exits 0 so Setup continues.
set O=%SystemDrive%\goya\specdiag.txt
echo === specdiag %date% %time% > "%O%"
reg query HKLM\SYSTEM\CurrentControlSet\Control\hivelist /v \REGISTRY\MACHINE\BCD00000000 >> "%O%" 2>&1
reg query HKLM\BCD00000000\Description >> "%O%" 2>&1
bcdedit /enum {bootmgr} >> "%O%" 2>&1
echo bcdedit system store exit code %ERRORLEVEL% >> "%O%"
bcdedit /enum firmware >> "%O%" 2>&1
echo bcdedit /enum firmware exit code %ERRORLEVEL% >> "%O%"
echo === end %time% >> "%O%"
exit /b 0
