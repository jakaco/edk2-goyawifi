# HalExtMmpTimer - Windows HAL extension for the Marvell PXA986/PXA988 APB timer

Ported from `ms-iot/imx-iotcore` `hals/halext/HalExtiMX6Timers` (MIT). **Not compiled or tested**: it needs the
Windows WDK (`nthalext.h`, `halextlib.lib`) and MSVC for ARM (ARMNT), which are not available in the Linux
sandbox this was written in.

What it does
- Cortex-A9 has no Generic Timer, so the Windows HAL needs a HAL extension to get a clock tick and a
  performance counter. The extension is bound to the CSRT resource group `MRVL` / DeviceId 0x0001, created by
  `Platform/Samsung/GoyaWifiPkg/Drivers/GoyaWifiAcpiDxe`.
- CSRT descriptor UID 0, TimerType 1: counter 0 of timer block 0xD4014000, match interrupt (GIC IRQ 45 =
  SPI 13), one-shot + periodic emulation. Logic follows the vendor kernel's `arch/arm/mach-mmp/time.c`.
- CSRT descriptor UID 1, TimerType 2: counter 1, free running, read through the CVWR latch (3 reads), same as
  the firmware `MmpTimerLib`. Clock 3.25 MHz.

Build (Windows) - one source, two WDKs
- `build.cmd w10`: use the WDK **10.0.22621** (Microsoft lists it as supported for Windows 10 x86/ARM32 driver
  development, with VS 2022) or its EWDK ISO (self-contained; works next to VS 2026). WDKs from 22000 on up to
  the VS 2026 one (28000) cannot build ARM32 kernel code. Run from an ARM build prompt (EWDK `setenv arm fre`,
  or a VS 2022 ARM prompt with `DDK_INC_PATH` and `LIB` set). `HalExtMmpTimer.vcxproj` also works in VS 2022.
- `build.cmd w8`: Windows 8.0 WDK, from its ARM build environment. Adds `/DHALEXT_TARGET_WIN8=1`.
- `HalExtCompat.h` gives fallbacks only for names an older `nthalext.h` lacks (`READ/WRITE_REGISTER_NOFENCE_ULONG`,
  `TIMER_ALWAYS_ON`, `TIMER_GENERATES_LINE_BASED_INTERRUPTS`); the newer header's own definitions always win. On
  8.0 without `TIMER_ALWAYS_ON` the tick timer is just not flagged always-on.
- Still unverified: the layout of `TIMER_INITIALIZATION_BLOCK` in the 8.0 header (`Interrupt.Gsi`,
  `INITIALIZE_TIMER_HEADER`). If the 8.0 compile fails there, that block is the only part to adapt.
- Neither path was compiled by me (no Windows toolchain here). `build.cmd` flags follow the imx-iotcore project
  settings and standard kernel-mode DLL flags, so a link or flag error is likely on the first run; send me the log.
- Unsigned DLL: Windows needs `bcdedit /set testsigning on` (and `nointegritychecks` on 8.0 RT) to load it.

Install: the INF matches `ACPI\MRVL0002` (device `TMR0` in the DSDT) and registers the DLL in the OS
extension database under `VEN_MRVL&DEV_0001`. For an offline WinPE image, copy the DLL to `System32` and add the
registry value `HALExtension` at
`HKLM\SYSTEM\CurrentControlSet\Control\OSExtensionDatabase\VEN_MRVL&DEV_0001` pointing to it.

Open questions
- Whether the Windows 8.0 HAL accepts a CSRT timer with these type/subtype codes (see the TODO in the ACPI driver).
- Counter 1's functional clock is left as configured by the firmware (3.25 MHz via APBC_TIMERS0).
