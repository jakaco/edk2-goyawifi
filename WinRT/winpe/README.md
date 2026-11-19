# Windows 8.0 RT WinPE card for the Galaxy Tab 3 Lite (goyawifi)

Same idea as the `rossa-winpe` kit, adapted to ARM32 / Windows 8.0 RT and this firmware.

You provide: **your own Windows 8 ARM WinPE media** (the 8.0 ADK has no ARM WinPE and there is no `copype` for
ARM32; the media comes from your build, e.g. 8520). These scripts never fetch Windows files.

1. FAT32-format a microSD (<= 32 GB).
2. Build `HalExtMmpTimer.dll` (GitHub Actions artifact `HalExtMmpTimer-w10-arm`) and put it next to `make-goya-sd.cmd`.
   Put any Goya driver `.inf/.sys` (`goyagpio`, `goyai2c`, `goyatouch`, `goyakeys`, `goyasd`) there too, if you have them.
3. Elevated cmd on a Windows 10 PC: `make-goya-sd.cmd E: C:\winpe-arm-media`
4. Tablet: `fastboot boot goyawifi-uefi-winrt-release-boot.img` (from `goyawifi-edk2-test6.zip`), with the card inserted.
5. After the boot, `\goyape\diag.txt` on the card lists drivers, device error codes and the HAL extension key.

What to expect at first: a boot with only HalExtMmpTimer (no input drivers) is the first milestone. The screen comes from
the UEFI framebuffer (BasicDisplay); WinPE runs from RAM so it needs no storage driver. Without touch/keys you will only see it
run; `autorun.cmd` writing `diag.txt` to the card proves it got to the shell (needs the SD host driver, see the driver plan).

## Drivers from CI
`goya-drivers.yml` builds each driver under `WinRT/drivers/<name>/` and uploads `<name>-arm` (sys + inf).
Unzip an artifact so the files end up in `WinRT/drivers/<name>/out/` (or next to `make-goya-sd.cmd`); the script adds them
with DISM. Current drivers: `goyagpio` (GpioClx, ACPI `MRVL0003`). Planned: `goyai2c` (SpbCx), `goyatouch`, `goyakeys`.
