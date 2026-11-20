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
rem ===========================================================================
set SD=%~1
set MEDIA=%~f2
set HALEXT=%~f3
set WORK=C:\GoyaPE
set HERE=%~dp0
set MNT=%TEMP%\goyape-mount
set HIVE=GOYAPE_SYSTEM
rem Value written to OSExtensionDatabase\VEN_MRVL&DEV_0001 "HALExtension".
rem If the HAL does not pick the extension up, this path format is the first suspect.
set HALEXT_REGPATH=\SystemRoot\System32\HalExtMmpTimer.dll

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

reg load HKLM\%HIVE% "%MNT%\Windows\System32\config\SYSTEM" >nul || goto discard
reg add "HKLM\%HIVE%\ControlSet001\Control\OSExtensionDatabase\VEN_MRVL&DEV_0001" /v HALExtension /t REG_SZ /d "%HALEXT_REGPATH%" /f >nul
set RC=%errorlevel%
reg unload HKLM\%HIVE% >nul
if not "%RC%"=="0" goto discard
echo   HAL extension registered: %HALEXT_REGPATH%

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
if not exist "%KITDRV%\goyasdhc.inf" goto perdriver
echo   adding all drivers from "%KITDRV%"
dism /Image:"%MNT%" /Add-Driver /Driver:"%KITDRV%" /Recurse /ForceUnsigned || goto discard
goto drvdone
:perdriver
for %%N in (goyagpio goyai2c goyatouch goyakeys goyapinmux goyasdhc) do call :optdrv %%N || goto discard
:drvdone

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

echo === 3/4 BCD
call "%HERE%bcd.cmd" goya "%WORK%" || exit /b 1

echo === 4/4 copying to %SD%\
robocopy "%WORK%" %SD%\ /E /NFL /NDL /NJH /NP
if errorlevel 8 (echo copy to card failed & exit /b 1)
for %%F in (install.wim install.esd install.swm) do if exist %SD%\sources\%%F echo NOTE: %SD%\sources\%%F is left over from an earlier copy and is not needed for WinPE - you can delete it.
if exist "%HERE%goyape\autorun.cmd" robocopy "%HERE%goyape" %SD%\goyape /E /NFL /NDL /NJH /NP >nul
echo.
echo Done. Card in the tablet, then boot the EDK2 firmware: goyawifi-uefi-winrt-release.img
echo The EDK2 image to use is the "winrt" one (RTC set to 2012-08-01).
exit /b 0

:optdrv
set DDIR=
if exist "%HERE%%1.inf" set DDIR=%HERE%
if not defined DDIR if exist "%HERE%..\drivers\%1\out\%1.inf" set DDIR=%HERE%..\drivers\%1\out\
if not defined DDIR (echo   %1: not found, skipping & exit /b 0)
echo   adding %1
dism /Image:"%MNT%" /Add-Driver /Driver:"%DDIR%%1.inf" /ForceUnsigned
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
