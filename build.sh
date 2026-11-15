#!/usr/bin/env bash
# Build the goyawifi UEFI firmware and pack it as a boot.img for S-Boot.
#
#   ./build.sh [DEBUG|RELEASE]
#
# Needs: arm-linux-gnueabi(hf)-gcc, python3, uuid-dev, nasm/iasl not needed.
set -euo pipefail

TARGET=${1:-DEBUG}
HERE=$(cd "$(dirname "$0")" && pwd)
EDK2=${EDK2:-$HERE/edk2}
EDK2_TAG=edk2-stable202508.01   # last tianocore tag with 32-bit ARM support

if [ ! -d "$EDK2/BaseTools" ]; then
  git clone --branch "$EDK2_TAG" --depth 1 https://github.com/tianocore/edk2 "$EDK2"
  git -C "$EDK2" submodule update --init --depth 1
fi

if command -v arm-linux-gnueabi-gcc >/dev/null; then
  export GCC_ARM_PREFIX=arm-linux-gnueabi-
else
  export GCC_ARM_PREFIX=arm-linux-gnueabihf-
fi

export WORKSPACE=$HERE/workspace
export PACKAGES_PATH=$EDK2:$HERE
mkdir -p "$WORKSPACE"

make -C "$EDK2/BaseTools" -j"$(nproc)" >/dev/null
set +u
set --            # edksetup.sh parses the caller's positional arguments
# shellcheck disable=SC1091
(cd "$EDK2" && . ./edksetup.sh >/dev/null)
cd "$EDK2" && . ./edksetup.sh >/dev/null
cd "$WORKSPACE"
set -u

build -a ARM -t GCC -b "$TARGET" -n "$(nproc)" \
      -p Platform/Samsung/GoyaWifiPkg/GoyaWifi.dsc

FD=$WORKSPACE/Build/GoyaWifi/${TARGET}_GCC/FV/GOYAWIFI_EFI.fd
mkdir -p "$HERE/out"
python3 "$HERE/tools/mkbootimg-goyawifi.py" -p "$FD" \
        -o "$HERE/out/goyawifi-uefi-boot.img" --odin
echo "Done: out/goyawifi-uefi-boot.img (+ .tar.md5 for Odin)"
