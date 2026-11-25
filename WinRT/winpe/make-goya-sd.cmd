@echo off
setlocal EnableExtensions
rem ===========================================================================
rem  make-goya-sd.cmd - Windows 8.0 RT (ARM32) WinPE SD card for the Galaxy Tab 3
rem  Lite 7.0 Wi-Fi (goyawifi, PXA986) running edk2-goyawifi.
rem
rem  Elevated cmd on a Windows 10 PC (needs DISM, bcdedit, reg - all inbox):
rem
rem    make-goya-sd.cmd E: C:\path\to\winpe-arm-media [halext.dll]
rem
rem    E:                   FAT32 SD card (<= 32 GB)
rem    winpe-arm-media      folder with YOUR Windows 8 ARM WinPE files:
rem                           efi\boot\bootarm.efi, efi\microsoft\boot\BCD,
rem                           boot\boot.sdi, sources\boot.wim
rem                         (the Windows 8 ADK has no ARM WinPE and there is no
rem                          copype for ARM32, so the media must come from your
rem                          own Windows RT 8.0 build; this script never
rem                          downloads any Windows files)
rem    halext.dll           optional, default: HalExtMmpTimer.dll next to this script
rem                         or in ..\drivers\ (the goyawifi kit layout)
rem
rem  The media may also be full Windows RT install media: only efi\, boot\, bootmgr and
rem  the plain WinPE image of sources\boot.wim (index 1, exported alone and made
rem  bootable) are used - install.wim/install.esd and the setup files are never copied.
rem
rem  It will: copy those files to C:\GoyaPE, add HalExtMmpTimer.dll and its
rem  OSExtensionDatabase entry to the offline SYSTEM hive, add every Goya driver
rem  found in ..\drivers\ (kit layout) or next to this script (goyagpio, goyai2c,
rem  goyatouch, goyakeys, goyapinmux, goyasdhc) with
rem  DISM /ForceUnsigned, set the BCD (testsigning, nointegritychecks, onecpu),
rem  and copy everything to the card together with \goyape\autorun.cmd.
rem
rem  GOYA_PE_ENTRY=1: E: is instead the SYSTEM partition of a full Windows RT card
rem  (..\install\install-winrt-sd.cmd); WinPE is added to its boot menu as a RAM-disk
rem  entry and nothing else on the card changes (see :addentry).
rem ===========================================================================
set SD=%~1
set MEDIA=%~f2
set HALEXT=%~f3
set WORK=C:\GoyaPE
set HERE=%~dp0
set MNT=%TEMP%\goyape-mount
set HIVE=GOYAPE_SYSTEM
rem Value written to OSExtensionDatabase\VEN_MRVL&DEV_0001 "HALExtension": a bare file name in
rem System32, the same format as the inbox entry VEN_ARMH&DEV_0310 = HalExtPL310.dll.
set HALEXT_REGPATH=HalExtMmpTimer.dll

net session >nul 2>&1 || (echo Needs an elevated prompt ^(Run as administrator^). & exit /b 1)
if "%SD%"=="" goto usage
if "%MEDIA%"=="" goto usage
set SD=%SD:~0,2%
if not exist %SD%\ (echo Drive %SD% not found. & exit /b 1)
for %%F in ("%MEDIA%\efi\boot\bootarm.efi" "%MEDIA%\boot\boot.sdi" "%MEDIA%\sources\boot.wim" "%MEDIA%\efi\microsoft\boot\BCD") do (
  if not exist %%F (echo Missing %%~F in the WinPE media folder. & exit /b 1)
)
if "%HALEXT%"=="" set HALEXT=%HERE%HalExtMmpTimer.dll
if not exist "%HALEXT%" if exist "%HERE%..\drivers\HalExtMmpTimer.dll" set HALEXT=%HERE%..\drivers\HalExtMmpTimer.dll
if not exist "%HALEXT%" (echo Missing HalExtMmpTimer.dll ^(build it: WinRT\HalExtMmpTimer, or the GitHub Actions artifact^). & exit /b 1)

echo === 1/4 copying only the WinPE boot files to %WORK%
dism /Unmount-Image /MountDir:"%MNT%" /Discard >nul 2>&1
reg unload HKLM\%HIVE% >nul 2>&1
if exist "%WORK%" rmdir /s /q "%WORK%"
mkdir "%WORK%\sources" || exit /b 1
rem efi\ + boot\ (+ bootmgr) only: no install.wim / install.esd / setup files / support\
robocopy "%MEDIA%\efi" "%WORK%\efi" /E /NFL /NDL /NJH /NP >nul
if errorlevel 8 (echo copy of efi\ failed & exit /b 1)
robocopy "%MEDIA%\boot" "%WORK%\boot" /E /NFL /NDL /NJH /NP >nul
if errorlevel 8 (echo copy of boot\ failed & exit /b 1)
for %%F in (bootmgr bootmgr.efi) do if exist "%MEDIA%\%%F" copy /y "%MEDIA%\%%F" "%WORK%\" >nul
rem install media: boot.wim = 1 "Windows PE" + 2 "Windows Setup" (the bootable one). Export the
rem plain WinPE image alone and mark it bootable, so the image we modify is the one that boots.
rem Other index: set GOYA_PE_INDEX=n before running this script.
set PEIDX=1
if defined GOYA_PE_INDEX set PEIDX=%GOYA_PE_INDEX%
dism /English /Get-ImageInfo /ImageFile:"%MEDIA%\sources\boot.wim" | findstr /i /c:"Index" /c:"Name"
echo   using boot.wim index %PEIDX%
dism /Export-Image /SourceImageFile:"%MEDIA%\sources\boot.wim" /SourceIndex:%PEIDX% /DestinationImageFile:"%WORK%\sources\boot.wim" /Bootable || exit /b 1
attrib -r "%WORK%\sources\boot.wim" >nul 2>&1

echo === 2/4 boot.wim: HAL extension, drivers, autorun
if exist "%MNT%" rmdir /s /q "%MNT%"
mkdir "%MNT%"
dism /Mount-Image /ImageFile:"%WORK%\sources\boot.wim" /Index:1 /MountDir:"%MNT%" || goto fail
copy /y "%HALEXT%" "%MNT%\Windows\System32\HalExtMmpTimer.dll" >nul || goto discard


rem kit layout: every INF in ..\drivers (goyasdhc gives WinPE access to the card as a disk);
rem otherwise each driver next to this script or in ..\drivers\<name>\out (GitHub Actions artifacts)
set KITDRV=%HERE%..\drivers
if not exist "%KITDRV%\GoyaTestCert.cer" goto nocert
reg load HKLM\GOYAPE_SOFTWARE "%MNT%\Windows\System32\config\SOFTWARE" >nul || goto discard
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%import-testcert.ps1" -Cer "%KITDRV%\GoyaTestCert.cer" -Hive GOYAPE_SOFTWARE
set RC=%errorlevel%
reg unload HKLM\GOYAPE_SOFTWARE >nul
if not "%RC%"=="0" goto discard
:nocert
rem Drivers put on a WinPE card. Default: goyatouch (mouse, drives its I2C bus itself), goyakeys and
rem goyasdhc (the microSD card as a disk; GOYA_SDHC=0 leaves it out).
rem Left out by default, each can be added back by setting the variable to 1 before running:
rem   GOYA_PINMUX=1  goyapinmux - with it, Windows stopped intermittently (KMODE_EXCEPTION_NOT_HANDLED)
rem   GOYA_I2C=1     goyai2c    - conflicts: goyatouch binds to the same I2C1 device and drives it
rem   GOYA_GPIO=1    goyagpio   - the 8520 GPIO class extension stops Windows when it starts
rem HalExtMmpTimer.inf is never added: the HAL extension entry is set directly below.
set GOYA_DRVLIST=goyatouch goyakeys
if "%GOYA_PINMUX%"=="1" set GOYA_DRVLIST=%GOYA_DRVLIST% goyapinmux
if "%GOYA_I2C%"=="1" set GOYA_DRVLIST=%GOYA_DRVLIST% goyai2c
if not "%GOYA_SDHC%"=="0" set GOYA_DRVLIST=%GOYA_DRVLIST% goyasdhc
if "%GOYA_GPIO%"=="1" set GOYA_DRVLIST=goyagpio %GOYA_DRVLIST%
for %%N in (%GOYA_DRVLIST%) do call :optdrv %%N || goto discard
:drvdone

rem HAL extension entry LAST, so that nothing added above can overwrite it
reg load HKLM\%HIVE% "%MNT%\Windows\System32\config\SYSTEM" >nul || goto discard
reg add "HKLM\%HIVE%\ControlSet001\Control\OSExtensionDatabase\VEN_MRVL&DEV_0001" /v HALExtension /t REG_SZ /d "%HALEXT_REGPATH%" /f >nul
set RC=%errorlevel%
rem goyatouch is a mouse port driver: mouclass must be the Mouse class upper filter (it normally is)
set MCLS=HKLM\%HIVE%\ControlSet001\Control\Class\{4d36e96f-e325-11ce-bfc1-08002be10318}
reg query "%MCLS%" /v UpperFilters 2>nul | find /i "mouclass" >nul || (
  echo   Mouse class had no mouclass upper filter: adding it
  reg add "%MCLS%" /v UpperFilters /t REG_MULTI_SZ /d mouclass /f >nul
  reg add "%MCLS%" /v Class /t REG_SZ /d Mouse /f >nul
)
reg query "HKLM\%HIVE%\ControlSet001\Services\mouclass" >nul 2>&1 || echo   WARNING: this WinPE image has no mouclass service - no mouse/touch pointer possible
rem goyabug: boot driver that writes the stop code, parameters and the faulting driver on the stop
rem screen (8520 shows no parameters). Set GOYA_BUG=0 to leave it out.
set BUGSYS=
if exist "%KITDRV%\goyabug.sys" set BUGSYS=%KITDRV%\goyabug.sys
if not defined BUGSYS if exist "%HERE%goyabug.sys" set BUGSYS=%HERE%goyabug.sys
if "%GOYA_BUG%"=="0" set BUGSYS=
if defined BUGSYS (
  copy /y "%BUGSYS%" "%MNT%\Windows\System32\drivers\goyabug.sys" >nul
  reg add "HKLM\%HIVE%\ControlSet001\Services\goyabug" /v Type /t REG_DWORD /d 1 /f >nul
  reg add "HKLM\%HIVE%\ControlSet001\Services\goyabug" /v Start /t REG_DWORD /d 0 /f >nul
  reg add "HKLM\%HIVE%\ControlSet001\Services\goyabug" /v ErrorControl /t REG_DWORD /d 0 /f >nul
  rem first boot-driver group, so that it is ready as early as a driver can be
  reg add "HKLM\%HIVE%\ControlSet001\Services\goyabug" /v Group /t REG_SZ /d "System Reserved" /f >nul
  reg add "HKLM\%HIVE%\ControlSet001\Services\goyabug" /v ImagePath /t REG_EXPAND_SZ /d "System32\drivers\goyabug.sys" /f >nul
  echo   goyabug boot driver added
)
rem Bring-up: the stop screen stays up and shows the four parameters
reg add "HKLM\%HIVE%\ControlSet001\Control\CrashControl" /v AutoReboot /t REG_DWORD /d 0 /f >nul
reg add "HKLM\%HIVE%\ControlSet001\Control\CrashControl" /v DisplayParameters /t REG_DWORD /d 1 /f >nul
reg unload HKLM\%HIVE% >nul
if not "%RC%"=="0" goto discard
echo   HAL extension registered: %HALEXT_REGPATH%

if not exist "%MNT%\Windows\System32\startnet.cmd" > "%MNT%\Windows\System32\startnet.cmd" echo wpeinit
>> "%MNT%\Windows\System32\startnet.cmd" echo for %%%%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if exist %%%%d:\goyape\autorun.cmd call %%%%d:\goyape\autorun.cmd

rem on-screen keyboard as the WinPE shell: goyaosk starts cmd /k startnet.cmd above the keyboard;
rem if it cannot run, winpeshl moves on to the plain cmd line
set OSK=
if exist "%HERE%goyaosk.exe" set OSK=%HERE%goyaosk.exe
if exist "%HERE%..\drivers\goyaosk.exe" set OSK=%HERE%..\drivers\goyaosk.exe
if not defined OSK goto noosk
copy /y "%OSK%" "%MNT%\Windows\System32\goyaosk.exe" >nul || goto discard
> "%MNT%\Windows\System32\winpeshl.ini" (
  echo [LaunchApps]
  echo %%SYSTEMROOT%%\System32\goyaosk.exe, /shell
  echo %%SYSTEMROOT%%\System32\cmd.exe, /k %%SYSTEMROOT%%\System32\startnet.cmd
)
echo   goyaosk.exe installed as the WinPE shell ^(winpeshl.ini^)
:noosk
dism /Unmount-Image /MountDir:"%MNT%" /Commit || goto fail

if "%GOYA_PE_ENTRY%"=="1" goto addentry

echo === 3/4 BCD
call "%HERE%bcd.cmd" goya "%WORK%" || exit /b 1

echo === 4/4 copying to %SD%\
robocopy "%WORK%" %SD%\ /E /NFL /NDL /NJH /NP
if errorlevel 8 (echo copy to card failed & exit /b 1)
for %%F in (install.wim install.esd install.swm) do if exist %SD%\sources\%%F echo NOTE: %SD%\sources\%%F is left over from an earlier copy and is not needed for WinPE - you can delete it.
call :goyape
echo.
echo Done. Card in the tablet, then boot the EDK2 firmware: goyawifi-uefi-winrt-release.img
echo The EDK2 image to use is the "winrt" one (RTC set to 2012-08-01).
exit /b 0

rem ---- GOYA_PE_ENTRY=1: WinPE as an extra boot entry on a Windows RT card ------------------------------
rem %SD% is the card's FAT32 SYSTEM partition made by ..\install\install-winrt-sd.cmd. Its boot manager
rem and BCD stay; only boot\boot.sdi, sources\boot.wim and \goyape are copied, and a RAM-disk WinPE
rem entry is added to the BCD as the default (the full install stays in the menu: Vol Up/Down + Home).
:addentry
set B=%SD%\EFI\Microsoft\Boot\BCD
if not exist "%B%" (echo %SD% has no \EFI\Microsoft\Boot\BCD - not a Windows RT card system partition. & exit /b 1)
echo === 3/4 copying WinPE to %SD%\ (boot.sdi, boot.wim, goyape)
if not exist %SD%\boot mkdir %SD%\boot
if not exist %SD%\sources mkdir %SD%\sources
copy /y "%WORK%\boot\boot.sdi" %SD%\boot\boot.sdi >nul || (echo copy of boot.sdi failed & exit /b 1)
copy /y "%WORK%\sources\boot.wim" %SD%\sources\boot.wim >nul || (echo copy of boot.wim failed - is the partition big enough? & exit /b 1)
call :goyape
echo === 4/4 BCD: RAM-disk WinPE entry
bcdedit /store "%B%" /enum {ramdiskoptions} >nul 2>&1 || (bcdedit /store "%B%" /create {ramdiskoptions} /d "Goya WinPE ramdisk" >nul || exit /b 1)
bcdedit /store "%B%" /set {ramdiskoptions} ramdisksdidevice partition=%SD% >nul || exit /b 1
bcdedit /store "%B%" /set {ramdiskoptions} ramdisksdipath \boot\boot.sdi >nul || exit /b 1
rem an entry from an earlier run is replaced
for /f "tokens=2 delims={}" %%G in ('bcdedit /store "%B%" /enum osloader /v ^| findstr /i /c:"identifier"') do (
  bcdedit /store "%B%" /enum {%%G} | findstr /i /c:"WinPE (RAM)" >nul && bcdedit /store "%B%" /delete {%%G} /cleanup >nul
)
set PEG=
for /f "tokens=2 delims={}" %%G in ('bcdedit /store "%B%" /create /d "WinPE (RAM)" /application osloader') do set PEG={%%G}
if not defined PEG (echo could not create the WinPE entry & exit /b 1)
bcdedit /store "%B%" /set %PEG% device ramdisk=[%SD%]\sources\boot.wim,{ramdiskoptions} >nul || exit /b 1
bcdedit /store "%B%" /set %PEG% osdevice ramdisk=[%SD%]\sources\boot.wim,{ramdiskoptions} >nul || exit /b 1
bcdedit /store "%B%" /set %PEG% path \windows\system32\winload.efi >nul || exit /b 1
bcdedit /store "%B%" /set %PEG% systemroot \windows >nul || exit /b 1
bcdedit /store "%B%" /set %PEG% winpe yes >nul
bcdedit /store "%B%" /set %PEG% detecthal yes >nul
bcdedit /store "%B%" /set %PEG% locale en-US >nul
bcdedit /store "%B%" /set %PEG% testsigning on >nul || exit /b 1
bcdedit /store "%B%" /set %PEG% nointegritychecks on >nul || exit /b 1
bcdedit /store "%B%" /set %PEG% onecpu on >nul
bcdedit /store "%B%" /displayorder %PEG% /addfirst >nul
bcdedit /store "%B%" /default %PEG% >nul
bcdedit /store "%B%" /timeout 5 >nul
bcdedit /store "%B%" /enum %PEG%
echo.
echo Done. WinPE is the default boot entry on the card (5 s menu; the full install is the second entry).
echo Back to the full install as default:  bcdedit /store %B% /default ^<its identifier^>
exit /b 0

rem ---- :goyape - \goyape on the card: autorun.cmd, goyacmp.exe (byte compare used by its tests) and
rem install\ with the goyasdhc package for install.flag (an empty file named install.flag in \goyape puts
rem that goyasdhc into the Windows RT install on the card at the next WinPE boot) ----
:goyape
if exist "%HERE%goyape\autorun.cmd" robocopy "%HERE%goyape" %SD%\goyape /E /NFL /NDL /NJH /NP >nul
set GDIR=
if exist "%KITDRV%\goyacmp.exe" set GDIR=%KITDRV%\
if not defined GDIR if exist "%HERE%goyacmp.exe" set GDIR=%HERE%
if defined GDIR copy /y "%GDIR%goyacmp.exe" %SD%\goyape\goyacmp.exe >nul
if not defined GDIR echo   goyacmp.exe not found: the compares in \goyape\autorun.cmd will fail
set GDIR=
if exist "%KITDRV%\goyasdhc.sys" set GDIR=%KITDRV%\
if not defined GDIR if exist "%HERE%goyasdhc.sys" set GDIR=%HERE%
if not defined GDIR exit /b 0
if not exist %SD%\goyape\install mkdir %SD%\goyape\install
for %%X in (sys inf cat) do if exist "%GDIR%goyasdhc.%%X" copy /y "%GDIR%goyasdhc.%%X" %SD%\goyape\install\ >nul
echo   \goyape: autorun.cmd, goyacmp.exe, install\goyasdhc.* (create \goyape\install.flag to use it)
exit /b 0

:optdrv
set DDIR=
if exist "%KITDRV%\%1.inf" set DDIR=%KITDRV%\
if not defined DDIR if exist "%HERE%%1.inf" set DDIR=%HERE%
if not defined DDIR if exist "%HERE%..\drivers\%1\out\%1.inf" set DDIR=%HERE%..\drivers\%1\out\
if not defined DDIR (echo   %1: not found, skipping & exit /b 0)
echo   adding %1
dism /Image:"%MNT%" /Add-Driver /Driver:"%DDIR%%1.inf" /ForceUnsigned >nul
exit /b %errorlevel%

:discard
reg unload HKLM\%HIVE% >nul 2>&1
reg unload HKLM\GOYAPE_SOFTWARE >nul 2>&1
dism /Unmount-Image /MountDir:"%MNT%" /Discard
:fail
echo FAILED, see the messages above.
exit /b 1

:usage
echo usage: %~nx0 E: C:\path\to\winpe-arm-media [HalExtMmpTimer.dll]
exit /b 1
