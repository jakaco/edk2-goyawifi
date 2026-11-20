@echo off
rem Builds an sdport (SD host miniport) driver, ARM32, with the Windows 10 19041 WDK.
rem   build-sdport.cmd goyasdhc
rem Same environment as build-kmdf.cmd. sdport.h is only visible with NTDDI_VERSION >= WIN10 (whole header is guarded), so this one driver is compiled with Win10 target defines even though it must run on 8.0 RT (only basic ntoskrnl/sdport imports are used).
setlocal EnableExtensions
set N=%1
if "%N%"=="" goto usage
if not defined DDK_INC_PATH goto nowdk
for %%I in ("%DDK_INC_PATH%") do set "INCV=%%~sI"
for %%I in ("%DDK_INC_PATH%\..\..") do set "KITS=%%~sI"
for %%I in ("%~dp0%N%") do set "SRC=%%~sI"
cd /d %SRC% || exit /b 1

if not exist %INCV%\km\sdport.h (echo sdport.h missing & exit /b 1)
if not exist %KITS%\Lib\10.0.19041.0\km\arm\sdport.lib (echo sdport.lib missing & exit /b 1)

set DEFS=/DARM /D_ARM_ /DNDEBUG /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=0x0A000000 /DWINNT=1
set INC=/I%INCV% /I%INCV%\km /I%INCV%\km\crt /I%INCV%\shared
cl /nologo /c /kernel /GS- /Oi /Zi /W3 /wd4201 /wd4214 /wd4100 %DEFS% %INC% %N%.c || exit /b 1
link /nologo /DRIVER /NODEFAULTLIB /SUBSYSTEM:NATIVE /ENTRY:DriverEntry /INTEGRITYCHECK /LIBPATH:%KITS%\Lib\10.0.19041.0\km\arm ^
     /OUT:%N%.sys %N%.obj ntoskrnl.lib hal.lib sdport.lib BufferOverflowFastFailK.lib libcntpr.lib || exit /b 1
echo built %N%.sys
exit /b 0

:usage
echo usage: build-sdport.cmd name
exit /b 1
:nowdk
echo run from a WDK/VS ARM prompt with DDK_INC_PATH set
exit /b 1
