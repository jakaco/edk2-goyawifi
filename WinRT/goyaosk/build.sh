#!/bin/bash
# Build goyaosk.exe for Windows ARM32 (Thumb-2) with clang/lld, no C runtime.
#   bash build.sh            -> out/goyaosk.exe (unsigned; CI signs it with the Goya test cert)
#   bash build.sh --sign     -> also test-sign it locally with a throwaway certificate
# Needs clang, lld, llvm (llvm-dlltool), mingw-w64-common headers (+ osslsigncode, openssl for --sign).
set -e
SRC="$(cd "$(dirname "$0")" && pwd)"
B="${TMPDIR:-/tmp}/goyaosk-build"
mkdir -p "$B" "$SRC/out" && cd "$B"

def() { local dll=$1; shift; { echo "LIBRARY $dll.dll"; echo EXPORTS; printf '%s\n' "$@"; } > $dll.def; }
def user32 BeginPaint CreateWindowExW DefWindowProcW DispatchMessageW DrawTextW EndPaint EnumWindows \
  FillRect GetClassNameW GetClientRect GetDesktopWindow GetForegroundWindow GetMessageW GetSystemMetrics \
  GetWindowThreadProcessId InflateRect InvalidateRect IsWindow IsWindowVisible KillTimer LoadCursorW \
  MapVirtualKeyW PostQuitMessage PtInRect RegisterClassW ScreenToClient SendInput SetForegroundWindow \
  SetTimer SetWindowPos TranslateMessage wsprintfW wvsprintfA
def gdi32 CreateFontW CreateSolidBrush DeleteObject GetStockObject SelectObject SetBkMode SetTextColor
def kernel32 CloseHandle CompareStringW CreateFileW CreateProcessW ExitProcess GetCommandLineW \
  GetFileAttributesW GetLastError GetModuleHandleW GetTickCount MulDiv Sleep WriteFile lstrcmpW lstrlenW
def advapi32 RegCloseKey RegCreateKeyExW RegSetValueExW
def ntdll memset memcpy
for l in user32 gdi32 kernel32 advapi32 ntdll; do llvm-dlltool -m arm -d $l.def -l $l.lib; done

RES=$(clang -print-resource-dir)/include
CF="--target=armv7-w64-mingw32 -mthumb -mcpu=cortex-a9 -O2 -ffreestanding -fno-builtin -fno-stack-protector -funwind-tables -nostdinc \
    -isystem $RES -isystem /usr/share/mingw-w64/include -D_WIN32_WINNT=0x0602 -DUNICODE -Wall -Werror"
clang $CF -c "$SRC/goyaosk.c" -o goyaosk.o
clang $CF -c "$SRC/loadcfg.c" -o loadcfg.o
lld-link /nologo /subsystem:windows,6.02 /osversion:6.2 /entry:WinMainCRTStartup /machine:arm \
  /dynamicbase /nxcompat /nodefaultlib /out:goyaosk.exe goyaosk.o loadcfg.o \
  user32.lib gdi32.lib kernel32.lib advapi32.lib ntdll.lib
cp goyaosk.exe "$SRC/out/goyaosk.exe"

if [ "$1" = "--sign" ]; then
  [ -f testsign.key ] || openssl req -x509 -newkey rsa:2048 -sha256 -days 10950 -nodes -keyout testsign.key \
    -out testsign.crt -subj "/CN=goyaosk local test signing" -addext "extendedKeyUsage=codeSigning" \
    -not_before 20110101000000Z 2>/dev/null
  osslsigncode sign -certs testsign.crt -key testsign.key -h sha256 -n goyaosk -in goyaosk.exe \
    -out "$SRC/out/goyaosk.exe" >/dev/null
fi
ls -la "$SRC/out/goyaosk.exe"
