#!/bin/sh
# Syntax/codegen check only, against a hand-written SHIM of nthalext.h (types guessed, NOT the WDK's).
# The object is not a usable HAL extension. Real build: HalExtMmpTimer.vcxproj on Windows with the WDK.
cd "$(dirname "$0")" && clang --target=thumbv7-pc-windows-msvc -O2 -Wall -Wextra -Wno-unused-function -I. -I.. -c ../HalExtension.c -o /tmp/HalExtension.obj && echo OK
