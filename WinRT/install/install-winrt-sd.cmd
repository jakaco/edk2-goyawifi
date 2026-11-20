@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem ===========================================================================
rem  install-winrt-sd.cmd - put Windows 8.0 RT (ARM32) on a microSD card for the
rem  Galaxy Tab 3 Lite 7.0 Wi-Fi (goyawifi, PXA986) running edk2-goyawifi.
rem
rem  Elevated cmd on a Windows 10/11 PC (uses inbox diskpart, dism, bcdboot,
rem  bcdedit, reg, powershell):
rem
rem    install-winrt-sd.cmd <disk#> <install.wim> [index] [/nounattend]
rem
rem    disk#        the microSD card's disk number (diskpart "list disk" or
rem                 Get-Disk). THE WHOLE CARD IS ERASED. Internal and boot disks
rem                 are refused.
rem    install.wim  YOUR Windows RT 8.0 install image (sources\install.wim of your
rem                 ARM media; .esd also works with a Windows 10 DISM). Nothing is
rem                 downloaded by this script.
rem    index        image index inside the wim, default 1 (the list is shown first)
rem    /nounattend  do not copy unattend.xml (you go through OOBE by hand; only
rem                 sensible once touch works)
rem
rem  Layout written to the card: GPT (MBR fallback on old diskpart builds)
rem    SYSTEM  260 MB FAT32  \EFI\Boot\bootarm.efi + BCD   <- the firmware boots this
rem    MSR      16 MB        (GPT only)
rem    Windows  rest  NTFS
rem
rem  Then: Goya drivers from ..\drivers (DISM /ForceUnsigned), the Goya test certificate
rem  (GoyaTestCert.cer from the signed CI package) trusted in the image, goyasdhc forced to
rem  boot start, HalExtMmpTimer.dll in System32 + OSExtensionDatabase entry (bare file name), BCD with
rem  testsigning / nointegritychecks / onecpu / no recovery, unattend.xml (skips
rem  OOBE, local account "Goya", no password) and a first-boot diagnostics script.
rem ===========================================================================
set DISK=%~1
set WIM=%~f2
set IDX=1
set UNATTEND=1
for %%A in (%3 %4) do (
  if /i "%%~A"=="/nounattend" (set UNATTEND=0) else (set IDX=%%~A)
)
set HERE=%~dp0
set DRV=%HERE%..\drivers
set HIVE=GOYART_SYSTEM
set SWHIVE=GOYART_SOFTWARE
set HALEXT_REGPATH=HalExtMmpTimer.dll
set DP=%TEMP%\goya-diskpart.txt

net session >nul 2>&1 || (echo Needs an elevated prompt ^(Run as administrator^). & exit /b 1)
if "%DISK%"=="" goto usage
if "%~2"=="" goto usage
if not exist "%WIM%" (echo Image not found: %WIM% & exit /b 1)
if not exist "%DRV%\goyasdhc.inf" (echo Missing drivers: %DRV%\goyasdhc.inf ^(keep the kit folder layout^). & exit /b 1)
if not exist "%DRV%\goyasdhc.sys" (echo Missing %DRV%\goyasdhc.sys & exit /b 1)
if not exist "%DRV%\HalExtMmpTimer.dll" (echo Missing %DRV%\HalExtMmpTimer.dll & exit /b 1)
set SIGNED=0
if exist "%DRV%\GoyaTestCert.cer" if exist "%DRV%\goyasdhc.cat" set SIGNED=1
if "%SIGNED%"=="0" echo WARNING: drivers are not test-signed ^(no GoyaTestCert.cer / .cat in %DRV%^). Use the goya-drivers-signed CI package.
echo %DISK%| findstr /r "^[0-9][0-9]*$" >nul || (echo disk# must be a number. & exit /b 1)

echo === images in %WIM%
dism /English /Get-ImageInfo /ImageFile:"%WIM%" || exit /b 1
dism /English /Get-ImageInfo /ImageFile:"%WIM%" /Index:%IDX% | findstr /i "Architecture" | findstr /i "arm" >nul
if errorlevel 1 (echo Index %IDX% is not an ARM image - pick the Windows RT index. & exit /b 1)

echo === target disk
powershell -NoProfile -Command "$d=Get-Disk -Number %DISK% -ErrorAction Stop; Write-Host ('Disk {0}: {1}  {2:N1} GB  bus {3}' -f $d.Number,$d.FriendlyName,($d.Size/1GB),$d.BusType); if($d.IsBoot -or $d.IsSystem){exit 2}; if($d.Size -lt 8GB){exit 4}; if(@('SD','USB','MMC') -notcontains [string]$d.BusType){exit 3}; exit 0"
set RC=%errorlevel%
if "%RC%"=="1" (echo Disk %DISK% not found. & exit /b 1)
if "%RC%"=="2" (echo Disk %DISK% is this PC's boot/system disk - refused. & exit /b 1)
if "%RC%"=="4" (echo Disk %DISK% is smaller than 8 GB - Windows RT needs about 6 GB plus room. & exit /b 1)
if "%RC%"=="3" echo WARNING: disk %DISK% is not on an SD/USB/MMC bus. Make very sure this is the card.
set ANS=
set /p ANS=Type ERASE to wipe disk %DISK% and install Windows RT on it: 
if not "!ANS!"=="ERASE" (echo Cancelled. & exit /b 1)

rem free drive letters for the two partitions
set S=
set W=
for %%L in (S T U V R Q P O N M) do if not defined S if not exist %%L:\ set S=%%L
for %%L in (W X Y Z K L J I H G) do if not defined W if not exist %%L:\ if not "%%L"=="!S!" set W=%%L
if not defined S (echo No free drive letter. & exit /b 1)
if not defined W (echo No free drive letter. & exit /b 1)

echo === 1/6 partitioning disk %DISK% (SYSTEM=%S%: Windows=%W%:)
> "%DP%" (
  echo select disk %DISK%
  echo clean
  echo convert gpt
  echo create partition efi size=260
  echo format fs=fat32 quick label=SYSTEM
  echo assign letter=%S%
  echo create partition msr size=16
  echo create partition primary
  echo format fs=ntfs quick label=WindowsRT
  echo assign letter=%W%
  echo exit
)
diskpart /s "%DP%"
if errorlevel 1 (
  echo GPT on this removable card failed, using MBR ^(EDK2 boots both^).
  > "%DP%" (
    echo select disk %DISK%
    echo clean
    echo convert mbr
    echo create partition primary size=260
    echo format fs=fat32 quick label=SYSTEM
    echo assign letter=%S%
    echo active
    echo create partition primary
    echo format fs=ntfs quick label=WindowsRT
    echo assign letter=%W%
    echo exit
  )
  diskpart /s "%DP%" || goto fail
)
if not exist %S%:\ goto fail
if not exist %W%:\ goto fail

echo === 2/6 applying image index %IDX% to %W%:\ (takes a while on an SD card)
dism /Apply-Image /ImageFile:"%WIM%" /Index:%IDX% /ApplyDir:%W%:\ || goto fail

echo === 3/6 drivers (goya*.inf; the HAL extension is registered directly in step 4)
for %%I in ("%DRV%\goya*.inf") do (
  echo   adding %%~nxI
  dism /Image:%W%:\ /Add-Driver /Driver:"%%~fI" /ForceUnsigned >nul || goto fail
)

echo === 4/6 HAL extension, boot-start storage driver
copy /y "%DRV%\HalExtMmpTimer.dll" %W%:\Windows\System32\HalExtMmpTimer.dll >nul || goto fail
if not exist %W%:\Windows\System32\drivers\goyasdhc.sys copy /y "%DRV%\goyasdhc.sys" %W%:\Windows\System32\drivers\ >nul
reg unload HKLM\%HIVE% >nul 2>&1
reg load HKLM\%HIVE% %W%:\Windows\System32\config\SYSTEM >nul || goto fail
call :hive
set RC=%errorlevel%
reg unload HKLM\%HIVE% >nul
if not "%RC%"=="0" goto fail
if "%SIGNED%"=="0" goto nocert
reg unload HKLM\%SWHIVE% >nul 2>&1
reg load HKLM\%SWHIVE% %W%:\Windows\System32\config\SOFTWARE >nul || goto fail
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%import-testcert.ps1" -Cer "%DRV%\GoyaTestCert.cer" -Hive %SWHIVE%
set RC=%errorlevel%
reg unload HKLM\%SWHIVE% >nul
if not "%RC%"=="0" goto fail
:nocert

echo === 5/6 boot files and BCD
bcdboot %W%:\Windows /s %S%: /f UEFI || goto fail
if not exist %S%:\EFI\Boot mkdir %S%:\EFI\Boot
if not exist %S%:\EFI\Boot\bootarm.efi copy /y %S%:\EFI\Microsoft\Boot\bootmgfw.efi %S%:\EFI\Boot\bootarm.efi >nul
set B=%S%:\EFI\Microsoft\Boot\BCD
bcdedit /store %B% /set {default} testsigning on || goto fail
bcdedit /store %B% /set {default} nointegritychecks on || goto fail
bcdedit /store %B% /set {default} onecpu on
bcdedit /store %B% /set {default} recoveryenabled no
bcdedit /store %B% /set {default} bootstatuspolicy IgnoreAllFailures
bcdedit /store %B% /set {default} sos on
bcdedit /store %B% /set {bootmgr} timeout 3
bcdedit /store %B% /enum {default}

echo === 6/6 first boot: unattend + diagnostics
if "%UNATTEND%"=="1" (
  if not exist %W%:\Windows\Panther mkdir %W%:\Windows\Panther
  copy /y "%HERE%unattend.xml" %W%:\Windows\Panther\unattend.xml >nul || goto fail
  echo   unattend.xml installed ^(OOBE skipped, local account Goya, no password^)
)
if not exist %W%:\Windows\Setup\Scripts mkdir %W%:\Windows\Setup\Scripts
copy /y "%HERE%SetupComplete.cmd" %W%:\Windows\Setup\Scripts\SetupComplete.cmd >nul
mountvol %S%: /d >nul 2>&1

echo.
echo Done. Eject the card (Windows partition was %W%:), put it in the tablet and boot
echo the EDK2 firmware goyawifi-uefi-winrt-release.img (RTC fixed to 2012-08-01).
echo The first boot runs Windows setup (specialize + OOBE) and can take a long time
echo on an SD card with a polled driver. Afterwards C:\goya\diag.txt on the card
echo has drivers / devices / problem codes.
exit /b 0

:hive
set K=HKLM\%HIVE%\ControlSet001
reg add "%K%\Control\OSExtensionDatabase\VEN_MRVL&DEV_0001" /v HALExtension /t REG_SZ /d "%HALEXT_REGPATH%" /f >nul || exit /b 1
rem Bring-up: the stop screen stays up and shows the four parameters
reg add "%K%\Control\CrashControl" /v AutoReboot /t REG_DWORD /d 0 /f >nul
reg add "%K%\Control\CrashControl" /v DisplayParameters /t REG_DWORD /d 1 /f >nul
rem goyasdhc must be loaded by the boot loader no matter what DISM did with it
reg add "%K%\Services\goyasdhc" /v Type /t REG_DWORD /d 1 /f >nul || exit /b 1
reg add "%K%\Services\goyasdhc" /v Start /t REG_DWORD /d 0 /f >nul
reg add "%K%\Services\goyasdhc" /v ErrorControl /t REG_DWORD /d 3 /f >nul
reg add "%K%\Services\goyasdhc" /v Group /t REG_SZ /d "SCSI miniport" /f >nul
reg add "%K%\Services\goyasdhc" /v ImagePath /t REG_EXPAND_SZ /d "System32\drivers\goyasdhc.sys" /f >nul
reg add "%K%\Services\goyasdhc" /v BootFlags /t REG_DWORD /d 8 /f >nul
for %%I in (0 1 15 17) do reg add "%K%\Services\goyasdhc\Parameters\PnpInterface" /v %%I /t REG_DWORD /d 1 /f >nul
reg add "%K%\Control\CriticalDeviceDatabase\acpi#mrvl0007" /v Service /t REG_SZ /d goyasdhc /f >nul
reg add "%K%\Control\CriticalDeviceDatabase\acpi#mrvl0007" /v ClassGUID /t REG_SZ /d "{4D36E97B-E325-11CE-BFC1-08002BE10318}" /f >nul
echo   HAL extension: %HALEXT_REGPATH%
echo   goyasdhc: boot start
exit /b 0

:fail
reg unload HKLM\%HIVE% >nul 2>&1
reg unload HKLM\%SWHIVE% >nul 2>&1
echo FAILED, see the messages above.
exit /b 1

:usage
echo usage: %~nx0 ^<disk#^> ^<install.wim^> [index] [/nounattend]
echo   find the card's disk number with:  powershell Get-Disk
exit /b 1
