@echo off
rem Compile check for the goyagpu hardware layer (no driver yet): same ARM32 / NT 6.2 flags as the
rem other drivers, then the objects must not contain VFP/NEON or SDIV/UDIV instructions.
rem   build-gpu-check.cmd
setlocal EnableExtensions
if not defined DDK_INC_PATH goto nowdk
for %%I in ("%DDK_INC_PATH%") do set "INCV=%%~sI"
for %%I in ("%~dp0goyagpu") do set "SRC=%%~sI"
cd /d %SRC% || exit /b 1
set DEFS=/DARM /D_ARM_ /DNDEBUG /D_WIN32_WINNT=0x0602 /DNTDDI_VERSION=0x06020000 /DWINNT=1
set INC=/I%INCV% /I%INCV%\km /I%INCV%\km\crt /I%INCV%\shared
cl /nologo /c /kernel /GS- /Oi /Zi /W4 /WX /wd4201 /wd4214 /wd4100 %DEFS% %INC% gc1000_hw.c gc1000_mmu.c || exit /b 1
for %%O in (gc1000_hw gc1000_mmu) do (
  dumpbin /nologo /disasm %%O.obj > %%O.dis
  findstr /r /c:" vld" /c:" vst" /c:" vmov" /c:" vdup" /c:" vmrs" /c:" vmsr" %%O.dis && (echo ERROR: %%O.obj contains VFP/NEON instructions & exit /b 1)
  findstr /r /c:" sdiv" /c:" udiv" %%O.dis && (echo ERROR: %%O.obj contains SDIV/UDIV & exit /b 1)
)
echo goyagpu hardware layer compiles cleanly
exit /b 0
:nowdk
echo run from a WDK/VS ARM prompt with DDK_INC_PATH set
exit /b 1
