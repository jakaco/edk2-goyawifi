@echo off
rem Builds a Storport miniport (ARM32) with the Windows 10 19041 WDK, targeting Windows 8.0.
rem   build-storport.cmd goyasdhc
rem Same environment as build-kmdf.cmd (ARM VS prompt, DDK_INC_PATH set). Not a KMDF driver.
setlocal EnableExtensions
set N=%1
if "%N%"=="" goto usage
if not defined DDK_INC_PATH goto nowdk
for %%I in ("%DDK_INC_PATH%") do set "INCV=%%~sI"
for %%I in ("%DDK_INC_PATH%\..\..") do set "KITS=%%~sI"
for %%I in ("%~dp0%N%") do set "SRC=%%~sI"
cd /d %SRC% || exit /b 1
set KL=%KITS%\Lib\10.0.19041.0\km\arm

set DEFS=/DARM /D_ARM_ /DNDEBUG /D_WIN32_WINNT=0x0602 /DNTDDI_VERSION=0x06020000 /DWINNT=1
set INC=/I%INCV% /I%INCV%\km /I%INCV%\km\crt /I%INCV%\shared
cl /nologo /c /kernel /GS- /Oi /Zi /W3 /WX /wd4201 /wd4214 /wd4100 %DEFS% %INC% %N%.c || exit /b 1
link /nologo /DRIVER /NODEFAULTLIB /SUBSYSTEM:NATIVE /ENTRY:DriverEntry /INTEGRITYCHECK /LIBPATH:%KL% ^
     /OUT:%N%.sys %N%.obj storport.lib ntoskrnl.lib hal.lib BufferOverflowFastFailK.lib libcntpr.lib || exit /b 1
echo ---- imports (must only be storport.sys / ntoskrnl / hal) ----
dumpbin /nologo /imports %N%.sys | findstr /i ".sys .dll"
echo built %N%.sys
exit /b 0

:usage
echo usage: build-storport.cmd name
exit /b 1
:nowdk
echo run from a WDK/VS ARM prompt with DDK_INC_PATH set
exit /b 1
