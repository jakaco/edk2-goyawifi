# edk2-goyawifi

UEFI firmware (TianoCore EDK2) for the **Samsung Galaxy Tab 3 Lite 7.0 Wi-Fi
(SM-T110, `goyawifi`)**, built on the **Marvell PXA986** (PXA988 family,
2x Cortex-A9, 1 GiB LPDDR2).

> **Status: builds, not yet tested on hardware.** The same boot path
> (S-Boot -> KERNEL partition -> 0x10008000, zImage header) is proven by the
> goyawifi U-Boot port, which runs on the tablet. DEBUG builds print their
> log **on the screen**, so no UART is needed (see [Debugging](#debugging)).

## How it boots

The Samsung boot chain (BootROM → OBM → S-Boot) is signed and stays in place.
Like lk2nd or the U-Boot port, this firmware replaces the *Linux kernel*:

```
S-Boot ──loads KERNEL partition boot.img──> 0x10008000 (our FD)
   FD+0x0000  zImage-style header: "b 0x1000", magic 0x016f2818 @ +0x24
   FD+0x1000  FVMAIN_COMPACT: zero vector patched by GenFv -> PeilessSec
              PeilessSec: L2 off, UART, MMU, HOBs, LZMA-decompress FvMain
              DXE: GIC, MMP timer, GOP on S-Boot's splash, SD/eMMC, BDS
```

S-Boot leaves DRAM, PLLs, the PMIC and the display (LCD + DSI + VX5B3D
bridge, 1024x600 a8r8g8b8 at `0x17000000`) initialised, so the firmware only
reuses them.

It can also be chain-loaded from the goyawifi U-Boot port (`bootmenu` →
"UEFI firmware", which loads `GOYAWIFI_EFI.fd` from microSD/eMMC partition 1
and starts it with `bootz`).

## Layout

```
Silicon/Marvell/Pxa988Pkg/            reusable SoC support
  Include/Pxa988.h                    register map
  Library/MmpTimerLib                 TimerLib on the MMP APB timer (A9 has no generic timer)
  Library/PxaUartSerialPortLib        PXA UART (16550 + UUE bit), sets up its clock
  Library/Pxa988PlatformLib           ArmPlatformLib: L2 hand-off, MPCore info, memory map
  Library/Pxa988ResetSystemLib        WDT reset, 88PM822 power-off, "recovery" reboot
  Drivers/MmpTimerDxe                 EFI_TIMER_ARCH_PROTOCOL (timer 0 / GIC SPI 13)
  Drivers/SimpleFbDxe                 GOP on the pre-initialised framebuffer
  Drivers/Pxa988SdhciDxe              SDHCI clocks/pinmux + SdMmcOverride (PXAv3 quirks)
Platform/Samsung/GoyaWifiPkg/
  GoyaWifi.dsc / GoyaWifi.fdf         platform description, FD at 0x10008000 (2 MiB)
  Library/GoyaWifiPlatformPeiLib      FV HOB + reserved regions (OBM, CP, ramoops, FB)
  Drivers/GoyaWifiKeypadDxe           Vol Up / Vol Down / Home as Up / Down / Enter
  Dtb/pxa986-samsung-goyawifi.dtb     DT installed for the OS (from the 4.19 kernel tree)
tools/mkbootimg-goyawifi.py           boot.img (+ Odin .tar.md5) packer
build.sh                              fetch EDK2, build, pack
```

## EDK2 version

TianoCore removed 32-bit ARM after **`edk2-stable202508.01`**, which is the
tag `build.sh` checks out. Moving to a newer tag would mean carrying ARM
support out of tree.

## Building

```bash
sudo apt install gcc-arm-linux-gnueabi uuid-dev python3 build-essential
./build.sh            # DEBUG (serial logging)
./build.sh RELEASE
```

Outputs `out/goyawifi-uefi-boot.img` and `out/goyawifi-uefi-boot.img.tar.md5`.
Set `EDK2=/path/to/edk2` to use an existing checkout of the same tag.

## Flashing

Download mode: **Volume Down + Home + Power**, then

```bash
heimdall flash --KERNEL out/goyawifi-uefi-boot.img
# or Odin: AP/PDA = goyawifi-uefi-boot.img.tar.md5
```

`RECOVERY` is untouched (Volume Up + Home + Power). To get back to Android,
flash a stock/LineageOS `boot.img` to `KERNEL`.

## What is implemented

| Area | Implementation | Notes |
|---|---|---|
| CPU / MMU / caches | ArmPkg CpuDxe, short-descriptor MMU | L2 (PL310) cleaned and left **off** |
| Interrupts | ArmGicV2Dxe | A9 MPCore GIC @ `0xD1DFF000` / `0xD1DFE100` |
| Timer | MmpTimerDxe + MmpTimerLib | 3.25 MHz APB timer, 10 ms tick |
| Serial | PxaUartSerialPortLib | UART1 `0xD4017000`, 115200 8N1 |
| Display | SimpleFbDxe | reuses S-Boot's splash buffer, no re-init |
| Storage | SdMmcPciHcDxe via NonDiscoverablePciDeviceDxe | eMMC (SDH3, 26 MHz, clk-delay 15) and microSD (SDH1, 50 MHz, GPIO 11 CD, LDO4/LDO5 enabled) |
| Input | GoyaWifiKeypadDxe | Vol Up / Vol Down = Up / Down, Home / Power = Enter; keys taken out of keypad-controller mode with pad pull-ups (as the vendor kernel does) |
| Interrupt routing | Pxa988PlatformLib | legacy ICU global IRQ/FIQ outputs masked (`0xD4282114/144`), as 4.19 `pxa988_dt_init_irq()` |
| PMIC | Pm822Lib | polled TWSI2; base page `0x30`, power page `0x31` |
| On-screen log | PxaUartSerialPortLib | DEBUG builds mirror the log to the framebuffer (8x19 font, 128x31) |
| Reset / power off | Pxa988ResetSystemLib | WDT; 88PM822 `SW_PDOWN` over TWSI2 |
| Variables | emulated (RAM) | not persistent across reboots |
| OS hand-off | DtPlatformDxe | DT config table, no ACPI |

Not implemented: USB, SMP bring-up of CPU1 (left to the OS), DVFS, persistent
variables, charging.

## Memory map

| Range | Use |
|---|---|
| `0x00000000-0x000FFFFF` | OBM / BootROM, reserved |
| `0x06000000-0x080FFFFF` | CP carveout (hardware protected by S-Boot), reserved |
| `0x08100000-0x0817FFFF` | ramoops / last_kmsg, reserved |
| `0x10008000-0x10207FFF` | firmware device (FD) |
| `0x17000000-0x17257FFF` | framebuffer (write-through), reserved |
| `0x3C000000-0x3FFFFFFF` | UEFI PI memory + stack |

## Booting Linux

32-bit ARM Linux boots through its EFI stub (`CONFIG_EFI=y`,
`CONFIG_EFI_STUB=y`) using the DT this firmware installs. The goyawifi 4.19
kernel uses `CONFIG_ARM_SAMSUNG_PXA_BOOTFIXUP` (it forces decompression to
physical `0x00108000` for S-Boot), which isn't EFI-stub aware. For EFI boot,
build that kernel with the fixup **off** and the regions above kept reserved
(they are, through the EFI memory map). That kernel-side change hasn't been
tested yet.

## Debugging

* **Screen (DEBUG builds):** from the very first SEC line, everything printed
  through `DEBUG()` also appears on the panel in white on black. The log
  wraps to the top when it reaches the bottom; the blank line marks where the
  newest output is. If the tablet hangs, a photo of the screen shows the last
  thing that ran. The log state lives in the page right after the framebuffer
  (`0x17258000`), so every module shares one cursor.
* **UART:** UART1 is routed to micro-USB by the RT8973A MUIC with a
  **619 kOhm JIG** (ID to GND), at 3.3 V levels, 115200 8N1. DEBUG builds log
  everything from SEC onwards.
* **Splash stays, nothing printed:** the image never reached SEC's
  `ArmPlatformInitialize` (the first thing it does is clear the screen).
* **Hangs before any output:** the most likely culprits are, in order:
  1. S-Boot rejecting or not jumping to the image (check the header at FD+0x24),
  2. S-Boot entering in a mode other than SVC/MMU off (PeilessSec expects the
     Linux boot protocol),
  3. the UART clock (`MPMU_UART_PLL`, `APBC_UART1`).
* **Hangs after "DXE":** check the GIC (secure vs non-secure entry) and the
  timer IRQ number (SPI 13 → ID 45).
* `ramoops` at `0x08100000` survives a warm reset, so it's possible to dump
  a log from Linux/TWRP if UART isn't available.

## Credits / sources

* Hardware description: the goyawifi Linux 4.19 port
  (`samsung-goya/android_kernel_samsung_goyawifi`), the Samsung 3.4 vendor
  kernel and the Marvell PXA988 BSP.
* Structure modelled on ArmVirtPkg and edk2-platforms.

License: BSD-2-Clause-Patent (same as EDK2).
