@echo off
rem Command-line build of HalExtMmpTimer.dll for ARM (ARMNT), no Visual Studio project needed.
rem   build.cmd w10   - Windows 10 22H2 WDK (10.0.19041), run from "x86_arm" / "x64_arm" VS developer prompt
rem   build.cmd w8    - Windows 8.0 WDK, run from the WDK 8.0 "ARM Free Build Environment" prompt
rem Both produce HalExtMmpTimer.dll with the same source; w8 adds /DHALEXT_TARGET_WIN8=1.
setlocal
set T=%1
if "%T%"=="" set T=w10
set DEFS=/DARM /D_ARM_ /DNDEBUG /D_WIN32_WINNT=0x0602 /DNTDDI_VERSION=0x06020000
if /I "%T%"=="w10" set DEFS=/DARM /D_ARM_ /DNDEBUG /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=0x0A000008
if /I "%T%"=="w8" set DEFS=%DEFS% /DHALEXT_TARGET_WIN8=1
if not defined DDK_INC_PATH goto :needwdk
set INC=/I"%DDK_INC_PATH%" /I"%DDK_INC_PATH%\km" /I"%DDK_INC_PATH%\km\crt" /I"%DDK_INC_PATH%\shared"
cl /nologo /c /kernel /Gz /GS- /Oi /Zi /W3 /wd4201 /wd4214 %DEFS% %INC% HalExtension.c || exit /b 1
rc /nologo /I"%DDK_INC_PATH%" HalExtMmpTimer.rc 2>nul
link /nologo /DLL /NODEFAULTLIB /SUBSYSTEM:NATIVE /DRIVER /ENTRY:HalExtensionInit /INTEGRITYCHECK ^
     /DEF:HalExtMmpTimer.def /OUT:HalExtMmpTimer.dll ^
     HalExtension.obj halextlib.lib libcntpr.lib bufferoverflowfastfailk.lib || exit /b 1
echo built HalExtMmpTimer.dll for %T%
exit /b 0
:needwdk
echo Run this from a WDK/VS developer prompt for ARM (DDK_INC_PATH is not set).
exit /b 1
