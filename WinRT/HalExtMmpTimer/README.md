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

Build (Windows)
- VS + WDK with ARM driver support; open `HalExtMmpTimer.vcxproj`, config Release|ARM. Entry point
  `HalExtensionInit`, link `halextlib.lib`. Needs `/SUBSYSTEM:NATIVE` and `/INTEGRITYCHECK`.
- The template targets the Windows 10 WDK. `TIMER_INITIALIZATION_BLOCK` fields used here (`Interrupt.Gsi`,
  `TIMER_ALWAYS_ON`, `INITIALIZE_TIMER_HEADER`) may not exist in the Windows 8.0 `nthalext.h`; if building
  against the 8.0 WDK, adapt the block to that header's layout.
- Windows 8.0 loads HAL extensions only if signed or test-signing is on; expect to need
  `bcdedit /set testsigning on` and `nointegritychecks` (or an integrity-check-patched DLL).

Install: the INF matches `ACPI\MRVL0002` (device `TMR0` in the DSDT) and registers the DLL in the OS
extension database under `VEN_MRVL&DEV_0001`. For an offline WinPE image, copy the DLL to `System32` and add the
registry value `HALExtension` at
`HKLM\SYSTEM\CurrentControlSet\Control\OSExtensionDatabase\VEN_MRVL&DEV_0001` pointing to it.

Open questions
- Whether the Windows 8.0 HAL accepts a CSRT timer with these type/subtype codes (see the TODO in the ACPI driver).
- Counter 1's functional clock is left as configured by the firmware (3.25 MHz via APBC_TIMERS0).
