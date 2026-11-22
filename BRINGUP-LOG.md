# Windows RT on the Galaxy Tab 3 Lite — bring-up log

What it took to get Windows 8 build 8520 (Windows RT, ARM32) running on the
Samsung Galaxy Tab 3 Lite 7.0 Wi-Fi (SM-T110, `goyawifi`, Marvell PXA986,
Cortex-A9) with this EDK2 firmware: from the first stop screens to WinPE with
touch, keys and the microSD card, and a full Windows RT install on microSD.

Current status of every part (works / doesn't / untested): see
[README.md → Hardware status](README.md#hardware-status).

---

## Where it stands (2026-11-22)

* **WinPE** boots from microSD, runs for long periods without stopping, and has
  working **touch** (mouse pointer), **hardware keys** and the **microSD card as
  a disk with a drive letter**.
* **Full Windows RT** has been installed to a 16 GB microSD card and its first
  boot runs, with the card at full speed (26 MHz high-speed, 4-bit) and
  thousands of reads and writes without a single failure. It is slow and
  choppy because the SD driver copies by CPU and busy-waits (see "Next").
* Firmware to flash for Windows: `goyawifi-uefi-winrt-release.img.tar.md5`
  (DVFS off, RTC 2012-08-01, kernel fix-ups, DSDT with the SDH interrupt).

---

## 1. Early stop screens (kernel, HAL, ACPI)

| Stop | Cause | Fix |
|---|---|---|
| `0xC4 / 0x91` while the kernel starts | The 8520 kernel initialises the CPU's CP14 debug registers on every processor; they are fused off on this Cortex-A9 (undefined instruction) | **WinKernelFixupDxe**: at ExitBootServices, find the two calls to the per-processor debug-register setup in the loaded `ntoskrnl.exe` and replace them with NOPs (byte patterns from 6.2.8520.0) |
| `0x5C HAL_INITIALIZATION_FAILED` | CSRT timer descriptor used the wrong subtype | CSRT timer descriptors use Type 2 / **Subtype 0** |
| `KMODE_EXCEPTION_NOT_HANDLED` when drivers loaded | The compiler inlined NEON `memcpy`/`memset`; the 8.0 kernel does not allow NEON in drivers | `common/goyamem.c` (own NEON-free memcpy/memmove/memset) linked into every driver; CI fails on any NEON/VFP instruction |
| `SYSTEM_THREAD_EXCEPTION_NOT_HANDLED` | goyagpio under the pre-release 8520 GPIO class extension (GpioClx v1.0) | goyagpio left out of cards and installs (nothing needs it) |

Also done along the way: Windows 8.0 GpioClx/SpbCx packet versions, Storport
buffer mapping through `StorPortGetSystemAddress`, test signing in CI with a
`signed-drivers` branch, `goya-diag` offline diagnostics for cards.

## 2. Seeing crashes: goyabug and the on-screen traces

The 8520 stop screen shows no parameters and WinPE writes no dump, so crashes
were invisible. Built:

* **goyabug** — boot-start driver using `KeRegisterBugCheckCallback` and the
  exported `KiBugCheckData`. At a stop it draws a box with the stop name, code,
  P1–P4, which driver each parameter points into, and a heuristic call chain.
  Later versions add: the instruction bytes at the exception address, the
  owning image found by walking back to its MZ header and reading its PDB name
  (works for images missing from the module list), drivers that have unloaded
  (`~name`), the caller from the trap frame, uptime, current process and the
  last loaded driver. Vol Up + Vol Down for 3 s forces a test stop (0xE2).
* **goyatrace** — every driver writes "`<id> <stage>`" lines and values at the
  top right of the screen; a caught exception becomes a red line with the
  driver, site, code and image offset. Ids: 1 goyasdhc, 2 goyapinmux,
  3 goyagpio, 4 goyai2c, 5 goyatouch, 6 goyakeys, 7 goyabug.
* **HalExtMmpTimer** draws progress squares; **WinKernelFixupDxe** draws a
  marker in the bottom-right corner (green = all kernel sites patched).
* CI rejects SDIV/UDIV too: the PXA988's Cortex-A9 has no hardware divide.

## 3. Random early KMODE stops

Windows sometimes stopped with KMODE_EXCEPTION_NOT_HANDLED before any driver
loaded.

* **HalExtMmpTimer counter read** — the 3-read sequence could return a wrong
  value while the counter was being latched. Now: CVWR latch request, at least
  12 reads, then read until two values agree, never go backwards. Several clean
  boots after this.
* **Asynchronous aborts** — a RELEASE firmware runs with aborts masked, so a bus
  error from firmware code would only be delivered once Windows unmasks them.
  WinKernelFixupDxe checks ISR.A at ExitBootServices and takes a pending abort
  with its own handler (marker cyan / blue if one was pending).

## 4. Touch (goyatouch)

The longest bring-up. In order:

1. **Driver model.** A KMDF driver under mshidkmdf, then a WDM HID minidriver
   (modelled on j5touch) never produced a pointer; the HID minidriver never even
   loaded because the touch device (TCH0) has no driver on the I2C1 bus.
   goyatouch became a **WDM mouse port driver** (like i8042prt) bound to the
   **I2C1 controller** (`ACPI\MRVL0004`) under mouclass: absolute mouse,
   first finger = pointer + left button. It drives TWSI1 itself (polled).
2. **Power.** The chip answered with NAK/bus errors (C0000185): its supply,
   **88PM822 LDO1**, was off (Samsung's kernel switches it on). The firmware now
   sets LDO1 to 2.8 V and enables it. Chip answers: code F400.
3. **Interrupt clear.** The Zinitix BT532 holds its interrupt line (GPIO94) low
   until it gets `CLEAR_INT`, which Samsung's driver sends after every read,
   even when the status is 0. goyatouch only sent it after a real touch, so the
   chip stopped reporting. Fixed, plus the GPIO94 pull-up, the vendor X/Y
   resolution (599/1023) and the 4 touch keys. **Touch works.**
4. **Freezes.** Touch sometimes froze until a key was pressed. Samsung's
   bt532 driver has an **ESD watchdog**: the chip sends a heartbeat interrupt
   every 100 scans (register 0x00F1) and is restarted after 3 silent seconds.
   goyatouch now does the same and releases a held click first (`5 7` counts
   restarts).

## 5. Hardware keys (goyakeys)

KMDF lower filter under mshidkmdf: Vol Up / Vol Down / Home from the GPIO
level register. Works. The power key has no driver yet (the PMIC's I2C2 bus is
hidden from Windows).

## 6. microSD card (goyasdhc)

goyasdhc is a Storport miniport with its own SD stack (polled PIO). It had been
taken off the card because the random stops were blamed on it; those also
happened without it. Getting it working, step by step (the Storport findings
come from disassembling build 8520's own `storport.sys`):

1. **Diagnostics** — on-screen stage, card info, request counters, last error,
   stage history, guarded callbacks; card init no longer retried on every
   request at DISPATCH_LEVEL (a dead card could have frozen Windows).
2. **Register range** — `StorPortGetDeviceBase` only maps an address whose
   resource entry has the same interface type as passed in; goyasdhc now tries
   the likely types, with its own mapping as a fallback.
3. **TaggedQueuing** — after FindAdapter, Storport fails the start
   (C0000182) unless NeedPhysicalAddresses, TaggedQueuing, ScatterGather and
   Master are all TRUE. TaggedQueuing was FALSE, so HwInitialize never ran.
4. **Interrupt** — the start also fails (C0000225) without an interrupt
   resource. The DSDT now lists the SD interrupt (GIC 71, shared by all three SD
   controllers); goyasdhc keeps every SD controller's signal enable at 0.
5. **DMA check (stop 0xE6 / 0x1F)** — as a DMA adapter, Storport runs DMA cache
   maintenance on every buffer and the kernel stops on buffers that aren't
   cache-line aligned. goyasdhc now reports **VirtualDevice**, which makes
   Storport skip DMA setup, the interrupt and those flushes. **The card works
   in WinPE with a drive letter**, and full Windows RT boots from it.

## 7. Crashes while idle (PatchGuard)

* A KMODE C000001D (illegal instruction) at an address in no module appeared
  once; the **WinRT firmware build now leaves DVFS off** (Windows has no CPU
  frequency or thermal control here, and 1066 MHz ran at the lowest allowed
  voltage on a supply that also feeds the RAM).
* WinPE then stopped after a few idle minutes with the same error. goyabug
  showed the instruction: `MRC p14, 0, r1, c0, c2, 2` (a debug register read).
  It is **PatchGuard**: its check routines run every few minutes from encrypted
  copies in memory and touch the debug registers when a "debug hardware
  present" flag is set. WinKernelFixupDxe gained a **third patch site** that
  keeps that flag clear when PatchGuard sets itself up at boot (before it takes
  its baseline). Checked in the emulator against the real kernel; WinPE has run
  idle without stopping since.

## 8. Full Windows RT install

* `install-winrt-sd.cmd` (on the PC, elevated): applies `install.wim` to the
  card, adds the Goya drivers and the test certificate, registers the HAL
  extension and goyabug, makes goyasdhc boot start, sets testsigning /
  nointegritychecks / onecpu, and adds an answer file that skips the setup
  screens (local account "Goya", no password).
* Fixed: goyapinmux is no longer added by default; the card is partitioned as
  **MBR** (FAT32 system + NTFS), because Windows refuses EFI/MSR partitions on
  removable card readers and diskpart didn't report the failure.
* First boot runs on a 16 GB card at full speed with no failed requests.

## 9. Firmware changes in this period

* WinKernelFixupDxe: three kernel patch sites (two CP14 debug-register setup
  calls, one PatchGuard flag) and the asynchronous-abort check.
* Pxa988SdhciDxe: LDO1 (touch supply) at 2.8 V.
* GoyaWifiAcpiDxe: CSRT subtype 0; SDH1 interrupt (GIC 71) in the DSDT.
* `tools/build-winrt.sh`: DVFS off for the Windows build (`GOYA_DVFS=1` keeps
  it); RTC starts at 2012-08-01 for the build 8520 timebomb.
* `tools/emu/fixup_test.py`: loads the whole kernel image and checks all three
  patch sites.

## 10. README

The README now has a full **Hardware status** section (firmware and Windows,
works / doesn't work / untested / not implemented) and an updated feature
table.

---

## Next

* **goyasdhc speed**: done, smoother on hardware: requests are finished
  from Storport's timer when the card stalls for more than 0.5 ms (instead of
  spinning at DISPATCH_LEVEL), and data goes through the data port directly.
  Later: DMA plus the real interrupt.
* **GPU (goyagpu, Vivante GC1000)**: steps 1–2 done — shared KMD/UMD ABI and the hardware
  layer (power, reset, identification, MMUv1, front-end ring, submission), checked against the
  Linux etnaviv sequences by `tools/gpusim/ringtest.c`. Next: the WDDM 1.2 KMD; see
  `WinRT/drivers/goyagpu/README.md` for verified facts and open questions.
* **Charger driver**: under Windows the tablet does not charge (the SMB328
  watchdog stops charging after the firmware hands over).
* Power key and screen brightness; the second CPU core (firmware support for
  Windows' core start-up); battery percentage; Wi-Fi as a long-term goal.
* Turn off hibernation / Fast Startup on the Windows RT card (goyasdhc's
  hibernate path is untested).
* Still to confirm: goyabug's test stop, the on-screen keyboard (goyaosk) with
  touch, goyapinmux with a trimmed mask, persistent UEFI variables.
