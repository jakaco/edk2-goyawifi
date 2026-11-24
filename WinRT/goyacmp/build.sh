#!/bin/bash
# Build goyacmp.exe (console) for Windows ARM32 (Thumb-2) with clang/lld, no C runtime.
#   bash build.sh   -> out/goyacmp.exe (unsigned; CI signs it with the Goya test cert)
# Needs clang, lld, llvm (llvm-dlltool), mingw-w64-common headers. Same setup as ../goyaosk/build.sh.
set -e
SRC="$(cd "$(dirname "$0")" && pwd)"
B="${TMPDIR:-/tmp}/goyacmp-build"
mkdir -p "$B" "$SRC/out" && cd "$B"

def() { local dll=$1; shift; { echo "LIBRARY $dll.dll"; echo EXPORTS; printf '%s\n' "$@"; } > $dll.def; }
def user32 wvsprintfA
def kernel32 CloseHandle CreateFileW ExitProcess GetCommandLineW GetFileSizeEx GetLastError GetStdHandle \
  GetTickCount ReadFile VirtualAlloc WriteFile
for l in user32 kernel32; do llvm-dlltool -m arm -d $l.def -l $l.lib; done

RES=$(clang -print-resource-dir)/include
CF="--target=armv7-w64-mingw32 -mthumb -mcpu=cortex-a9 -O2 -ffreestanding -fno-builtin -fno-stack-protector -funwind-tables -nostdinc \
    -isystem $RES -isystem /usr/share/mingw-w64/include -D_WIN32_WINNT=0x0602 -DUNICODE -Wall -Werror"
clang $CF -c "$SRC/goyacmp.c" -o goyacmp.o
clang $CF -c "$SRC/../goyaosk/loadcfg.c" -o loadcfg.o       # load config + security cookie (WinPE needs it)
lld-link /nologo /subsystem:console,6.02 /osversion:6.2 /entry:mainCRTStartup /machine:arm \
  /dynamicbase /nxcompat /nodefaultlib /out:goyacmp.exe goyacmp.o loadcfg.o user32.lib kernel32.lib
cp goyacmp.exe "$SRC/out/goyacmp.exe"
ls -la "$SRC/out/goyacmp.exe"
