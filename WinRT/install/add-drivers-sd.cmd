@echo off
setlocal EnableExtensions
rem ===========================================================================
rem  add-drivers-sd.cmd - add or update Goya driver packages in a Windows RT install on a microSD card,
rem  without reinstalling. Elevated cmd on the PC, with the card in a card reader:
rem
rem    add-drivers-sd.cmd <W:> [name ...]
rem
rem    W:     drive letter of the card's Windows (NTFS) partition
rem    name   driver packages from ..\drivers (name.inf), default: goyasdio goyawifi (Wi-Fi)
rem
rem  Trusts this kit's GoyaTestCert.cer in the card's image (every CI run signs with a new certificate),
rem  adds the packages with DISM /ForceUnsigned, then flushes and dismounts the card.
rem ===========================================================================
set W=%~1
if "%W%"=="" goto usage
set W=%W:~0,1%
set HERE=%~dp0
set DRV=%HERE%..\drivers
set SWHIVE=GOYART_SOFTWARE
net session >nul 2>&1 || (echo Needs an elevated prompt ^(Run as administrator^). & exit /b 1)
if not exist %W%:\Windows\System32\config\SOFTWARE (echo %W%: is not a Windows partition. & exit /b 1)
if not exist "%DRV%\GoyaTestCert.cer" (echo No GoyaTestCert.cer in %DRV%. & exit /b 1)

shift
set LIST=
:args
if "%~1"=="" goto argsdone
set LIST=%LIST% %~1
shift
goto args
:argsdone
if "%LIST%"=="" set LIST=goyasdio goyawifi

echo === test certificate
reg unload HKLM\%SWHIVE% >nul 2>&1
reg load HKLM\%SWHIVE% %W%:\Windows\System32\config\SOFTWARE >nul || goto fail
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%import-testcert.ps1" -Cer "%DRV%\GoyaTestCert.cer" -Hive %SWHIVE%
set RC=%ERRORLEVEL%
reg unload HKLM\%SWHIVE% >nul
if not "%RC%"=="0" goto fail

echo === drivers
for %%N in (%LIST%) do (
  if not exist "%DRV%\%%N.inf" (echo   %%N.inf not found in %DRV% & goto fail)
  echo   adding %%N.inf
  dism /Image:%W%:\ /Add-Driver /Driver:"%DRV%\%%N.inf" /ForceUnsigned >nul || goto fail
)

echo === finishing: write everything to the card and dismount it
fsutil volume dismount %W%: >nul
echo   done - flash the matching firmware (goyawifi-uefi-winrt-*.img) too, then boot the card
exit /b 0

:usage
echo usage: add-drivers-sd.cmd W: [name ...]   (default: goyasdio goyawifi)
exit /b 1
:fail
reg unload HKLM\%SWHIVE% >nul 2>&1
echo FAILED
exit /b 1
