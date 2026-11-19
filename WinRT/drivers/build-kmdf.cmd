@echo off
rem Builds one Goya KMDF driver (ARM32) from the command line with the Windows 10 19041 WDK.
rem   build-kmdf.cmd goyagpio [extra-lib ...]
rem Run from an ARM-targeting VS developer prompt (vcvarsall x86_arm) with DDK_INC_PATH and
rem LIB set as in .github/workflows/goya-drivers.yml.
rem
rem KMDF version: Windows 8.0 ships KMDF 1.11. The script uses 1.11 if this WDK has it,
rem otherwise the lowest version it has and prints a warning (1.13 = Win8.1, 1.15 = Win10 1507:
rem such a build will NOT load on 8.0 RT; that case needs the 8.0/8.1 WDK).
setlocal EnableDelayedExpansion
set N=%1
if "%N%"=="" (echo usage: build-kmdf.cmd name & exit /b 1)
if not defined DDK_INC_PATH (echo run from a WDK/VS ARM prompt & exit /b 1)
cd /d %~dp0%N% || exit /b 1

set KM=
set KMINOR=
for %%V in (11 13 15 17 19 21 23 25 27 29 31) do (
  if not defined KM if exist "%DDK_INC_PATH%\wdf\kmdf\1.%%V" (set KM=%DDK_INC_PATH%\wdf\kmdf\1.%%V& set KMINOR=%%V)
)
if not defined KM (echo No KMDF headers under %DDK_INC_PATH%\wdf\kmdf & exit /b 1)
if not "%KMINOR%"=="11" echo WARNING: KMDF 1.11 ^(Windows 8.0^) not in this WDK, building against KMDF 1.%KMINOR%.
set KMLIB=%DDK_INC_PATH%\..\..\Lib\wdf\kmdf\arm\1.%KMINOR%
for /d %%D in ("%DDK_INC_PATH%\..\..\Lib\wdf\kmdf\arm\1.%KMINOR%") do set KMLIB=%%~fD

set DEFS=/DARM /D_ARM_ /DNDEBUG /D_WIN32_WINNT=0x0602 /DNTDDI_VERSION=0x06020000 /DKMDF_VERSION_MAJOR=1 /DKMDF_VERSION_MINOR=%KMINOR% /DWINNT=1
set INC=/I"%DDK_INC_PATH%" /I"%DDK_INC_PATH%\km" /I"%DDK_INC_PATH%\km\crt" /I"%DDK_INC_PATH%\shared" /I"%KM%"
cl /nologo /c /kernel /Gz /GS- /Oi /Zi /W3 /wd4201 /wd4214 /wd4100 %DEFS% %INC% %N%.c || exit /b 1

set EXTRA=
rem GpioClx / SpbCx import libraries, wherever the WDK keeps them
for %%L in (gpioclx.lib SpbCx.lib) do for /f "delims=" %%P in ('dir /s /b "%DDK_INC_PATH%\..\..\Lib\%%L" 2^>nul ^| findstr /i "arm" ^| findstr /v /i "arm64"') do set EXTRA=!EXTRA! "%%P"
echo extra libs: %EXTRA%
link /nologo /DRIVER /NODEFAULTLIB /SUBSYSTEM:NATIVE /ENTRY:FxDriverEntry /INTEGRITYCHECK /LIBPATH:"%KMLIB%" ^
     /OUT:%N%.sys %N%.obj WdfDriverEntry.lib WdfLdr.lib ntoskrnl.lib hal.lib wmilib.lib BufferOverflowFastFailK.lib libcntpr.lib %EXTRA% %2 %3 %4 || exit /b 1
echo built %N%.sys ^(KMDF 1.%KMINOR%^)
