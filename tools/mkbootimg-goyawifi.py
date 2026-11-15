#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""
Pack a bootloader payload (U-Boot or EDK2) into an Android v0 boot.img that
Samsung S-Boot on the SM-T110 (goyawifi, PXA986) will load from the KERNEL
partition and jump to at 0x10008000, exactly like a Linux zImage.

Optionally appends a second, "real" boot.img at 1 MiB into the partition
(lk2nd style); U-Boot's `boot_appended_bootimg` boots it.

Optionally wraps the result in an Odin flashable .tar.md5.

  mkbootimg-goyawifi.py -p u-boot.bin -o uboot-boot.img [--odin]
  mkbootimg-goyawifi.py -p u-boot.bin --append boot-linux.img -o boot.img
"""

import argparse
import hashlib
import io
import os
import struct
import sys
import tarfile

PAGE = 2048
BASE = 0x10000000
KERNEL_OFF = 0x00008000
RAMDISK_OFF = 0x01000000          # BOARD_MKBOOTIMG_ARGS --ramdisk_offset
SECOND_OFF = 0x00f00000
TAGS_OFF = 0x00000100
APPEND_AT = 1 << 20              # 1 MiB into the KERNEL partition
PART_SIZE = 16 << 20             # BOARD_BOOTIMAGE_PARTITION_SIZE
ZIMAGE_MAGIC = 0x016f2818


def pad(b, align=PAGE):
    return b + b"\0" * ((-len(b)) % align)


def fix_zimage_header(payload):
    """Make the zImage 'end' word cover the whole payload incl. appended DTB."""
    if len(payload) >= 0x30:
        magic, = struct.unpack_from("<I", payload, 0x24)
        if magic == ZIMAGE_MAGIC:
            b = bytearray(payload)
            struct.pack_into("<II", b, 0x28, 0, len(payload))
            return bytes(b)
    return payload


def bootimg(kernel, cmdline, board):
    ramdisk = b""
    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, b""):
        sha.update(blob)
        sha.update(struct.pack("<I", len(blob)))
    hdr = struct.pack(
        "<8s10I16s512s32s1024s",
        b"ANDROID!",
        len(kernel), BASE + KERNEL_OFF,
        len(ramdisk), BASE + RAMDISK_OFF,
        0, BASE + SECOND_OFF,
        BASE + TAGS_OFF, PAGE,
        0, 0,                      # header_version 0, os_version
        board.encode()[:16],
        cmdline.encode()[:512],
        sha.digest().ljust(32, b"\0"),
        b"",
    )
    return pad(hdr) + pad(kernel)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--payload", required=True,
                    help="u-boot.bin / EDK2 GOYAWIFI_EFI.fd")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--append", help="real boot.img to place at 1 MiB")
    ap.add_argument("--cmdline", default="")
    ap.add_argument("--board", default="MRVL")
    ap.add_argument("--odin", action="store_true",
                    help="also write <output>.tar.md5 for Odin (AP/PDA slot)")
    a = ap.parse_args()

    payload = fix_zimage_header(open(a.payload, "rb").read())
    img = bootimg(payload, a.cmdline, a.board)

    if a.append:
        if len(img) > APPEND_AT:
            sys.exit(f"payload image is {len(img)} bytes, > 1 MiB; cannot append")
        inner = open(a.append, "rb").read()
        if inner[:8] != b"ANDROID!":
            sys.exit("--append image is not an Android boot.img")
        img = img.ljust(APPEND_AT, b"\0") + inner

    if len(img) > PART_SIZE:
        sys.exit(f"image is {len(img)} bytes, KERNEL partition is {PART_SIZE}")

    open(a.output, "wb").write(img)
    print(f"{a.output}: {len(img)} bytes (payload {len(payload)} bytes)")

    if a.odin:
        tar_path = a.output + ".tar"
        with tarfile.open(tar_path, "w", format=tarfile.USTAR_FORMAT) as t:
            ti = tarfile.TarInfo("boot.img")
            ti.size = len(img)
            ti.mode = 0o644
            t.addfile(ti, io.BytesIO(img))
        data = open(tar_path, "rb").read()
        md5 = hashlib.md5(data).hexdigest()
        with open(tar_path + ".md5", "wb") as f:
            f.write(data)
            f.write(f"{md5}  {os.path.basename(tar_path)}\n".encode())
        os.remove(tar_path)
        print(f"{tar_path}.md5: Odin package (flash in AP/PDA)")


if __name__ == "__main__":
    main()
