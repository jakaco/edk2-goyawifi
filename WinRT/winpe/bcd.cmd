@echo off
setlocal EnableExtensions
rem BCD helper for the Goya WinPE card.  Elevated cmd.
rem   bcd.cmd show   E:\        list entries
rem   bcd.cmd goya   E:\        testsigning, nointegritychecks, onecpu on {default}
rem   bcd.cmd onecpu E:\ / smp E:\
rem   bcd.cmd debug  E:\        bootlog + sos (driver names on screen while booting)
set ACTION=%~1
if "%ACTION%"=="" set ACTION=show
set ROOT=%~f2
if "%ROOT:~-1%"=="\" set ROOT=%ROOT:~0,-1%
set S=%ROOT%\efi\microsoft\boot\BCD
net session >nul 2>&1 || (echo Needs an elevated prompt. & exit /b 1)
if not exist "%S%" (echo No BCD at %S% & exit /b 1)
if /i "%ACTION%"=="show"   (bcdedit /store "%S%" /enum all & exit /b %errorlevel%)
if /i "%ACTION%"=="goya" (
  bcdedit /store "%S%" /set {default} testsigning on || exit /b 1
  bcdedit /store "%S%" /set {default} nointegritychecks on || exit /b 1
  bcdedit /store "%S%" /set {default} onecpu on || exit /b 1
  bcdedit /store "%S%" /enum {default}
  exit /b 0
)
if /i "%ACTION%"=="onecpu" (bcdedit /store "%S%" /set {default} onecpu on & exit /b 0)
if /i "%ACTION%"=="smp"    (bcdedit /store "%S%" /deletevalue {default} onecpu & exit /b 0)
if /i "%ACTION%"=="debug"  (
  bcdedit /store "%S%" /set {default} bootlog yes
  bcdedit /store "%S%" /set {default} sos on
  exit /b 0
)
echo Unknown action "%ACTION%".
exit /b 1
