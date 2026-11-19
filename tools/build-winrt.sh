#!/bin/bash
# Windows 8.0 RT build: the RTC defaults to 2012-08-01 (build 8520 "fbl_eeap"
# has a build-date timebomb). VirtualRealTimeClockLib takes its default epoch
# from SOURCE_DATE_EPOCH at compile time; it is only used until the OS (or the
# Shell `date`/`time` command) sets the time.
# Usage: tools/build-winrt.sh [DEBUG|RELEASE]   (needs edk2 + arm-linux-gnueabi-gcc)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
export SOURCE_DATE_EPOCH=1343779200   # 2012-08-01 00:00:00 UTC
export WORKSPACE=$ROOT/workspace
export PACKAGES_PATH=${EDK2_PATH:-/home/claude/src/edk2}:$ROOT
export GCC_ARM_PREFIX=arm-linux-gnueabi-
T=${1:-RELEASE}; set --
rm -rf "$WORKSPACE"/Build/GoyaWifi/${T}_GCC/ARM/EmbeddedPkg/Library/VirtualRealTimeClockLib
cd "${EDK2_PATH:-/home/claude/src/edk2}"; set +e; . ./edksetup.sh >/dev/null 2>&1; set -e
cd "$WORKSPACE" && build -a ARM -t GCC -b "$T" -n 2 -p Platform/Samsung/GoyaWifiPkg/GoyaWifi.dsc
