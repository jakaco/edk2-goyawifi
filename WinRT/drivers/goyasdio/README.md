# goyasdio + goyawifi: Marvell 88W8777 Wi-Fi on Windows 8.0 RT

The SM-T110's Wi-Fi/Bluetooth module is a Marvell Avastar 88W8777 on SDH2 (SDIO, 4-bit). The Surface RT
used the 88W8797 from the same family, also on SDIO, with Marvell's Windows driver `mwls97w8arm.sys`.
That driver is built on the same Marvell code (MLAN) as the vendor Linux driver for our chip, so the plan is:
**the Surface RT driver, unchanged, on our own SDIO bus driver, with the 8777 firmware.**

```
ACPI\MRVL0009 (DSDT Device SDH2, 0xD4280800)
  └─ goyasdio.sys        SDIO bus driver (WDM): card init, CMD52/CMD53, card interrupt (polled)
       ├─ SD\VID_02DF&PID_9131&FN_1   → goyawifi.sys (= Surface RT mwls97w8arm.sys 14.62.17215.91)
       │                                 + mv14sxx97.bin (= SM-T110 sd8777_uapsta.bin 14.66.33.p46)
       └─ SD\VID_02DF&PID_9132&FN_2   (Bluetooth: no driver yet)
```

## Why our own SD bus

Windows 8.0 RT has no `sdport.sys`, and the inbox `sdbus.sys` cannot drive this controller: the PXAv3
base-clock capability is not usable (the clock comes from the APMU), there is no card-detect pin, and the
interrupt line is shared with the microSD controller, which goyasdhc runs polled. goyasdio talks to the
controller itself (`sdiocore.c`, same register handling as goyasdhc's `sdcore.c`) and gives the function
driver the interface it expects from the Windows SD bus.

## The SD bus contract (read from mwls97w8arm.sys)

The function driver links `sdbus.lib` statically. From its code (sys+0x71334 .. 0x71578):

| call | what it sends |
|---|---|
| `SdBusOpenInterface` | `IRP_MN_QUERY_INTERFACE` to the top of the PDO stack, `GUID_SDBUS_INTERFACE_STANDARD` {6bb24d81-e924-4825-af49-3acd33c1d820}, size 0x18, version 0x101 |
| `InitializeInterface` | parameters 0x14 bytes: target, DeviceGeneratesInterrupts = 1, CallbackAtDpcLevel = 0, callback, context |
| `SdBusSubmitRequest` | `IRP_MJ_INTERNAL_DEVICE_CONTROL`, IOCTL `0x43073`, the 0x48-byte request packet in `Type3InputBuffer`, sent to `Context->Target`; `sdbus.lib` first checks that the interface `Context` starts with `'SDIC'` (0x43494453) |

Requests the Marvell driver makes:
- **CMD52**: descriptor {52, standard, read/write, cmd only, R5}; the data byte comes back in `ResponseData[0]`.
- **CMD53**: block mode with 256-byte blocks (data port, fixed address) or byte mode. Its argument's count
  field is not consistent with the length, so goyasdio recomputes it from `Length` and the block length,
  like the Windows SD bus.
- **Properties** read: 3 function number, 4 function type, 5 bus driver version, 6 width, 7 clock, 8 bus
  interface control, 9 host block length, 10 function block length, 11 FN0 block length, 12 function
  interrupt enable. **Properties** set: 7 clock (if configured), 10 block length 256 (after writing FBR1
  0x110/0x111 itself with CMD52), 12 interrupt enable and 13 (1 at start-up, 0 on the D3 path together
  with 12: taken as "forward card interrupts").
- **Interrupt callback** at PASSIVE_LEVEL: it reads the card's interrupt status with synchronous requests,
  then calls `AcknowledgeInterrupt`. goyasdio polls the controller's card-interrupt status every 1 ms while
  a function waits for interrupts, masks it until the acknowledge, and calls the callback without the bus
  lock held.

The structure layouts are defined in `goyasdio.c` from these observations (with `C_ASSERT`s), not taken
from the current WDK headers.

## Firmware

The driver loads `mv14sxx97.bin` with `NdisOpenFile` (from `System32\drivers`) and uses the 8797 image
built into it only when that file is missing (sys+0x19808). `goyawifi.inf` installs the 8777 firmware
under that name. No binary patching.

The SDIO transport is the same for both chips (Linux mwifiex uses one register map, `mwifiex_reg_sd87xx`,
for 8786/8787/8797/8777), and the vendor Linux driver treats the 8777 as an 8787 with another firmware
file. Differences Linux had to work around for the 8777 firmware: 16-byte TxPD/RxPD (the 2012 MLAN uses
those too), no extended scan (the 2012 MLAN uses the legacy scan), and multi-port aggregation trouble.
The last one is the open risk.

## Package and signing

`tools/make-goyawifi-pkg.py` builds the package in CI from pinned inputs (checked by SHA-256):
- DriversStore/mwls97 `14.62.17215.91-arm` (commit 4d40cd6c): `mwls97w8arm.sys` → `goyawifi.sys` (unchanged)
- samsung-goya/android_vendor_samsung_goyawifi (commit be834799): `sd8777_uapsta.bin` → `mv14sxx97.bin`
- Marvell's INF with the 8777 hardware IDs, the new file names, no virtual Wi-Fi bus (`netvwifibus.inf`),
  Band 3 (2.4 GHz only), 1x1.

`sign-goya-drivers.ps1` adds the firmware file to `goyawifi.cat`. The Marvell binary keeps its own
signature and gets the Goya test signature added.

## On-screen trace (driver id 8)

- `8 xx` step: `00` added, `01` start, `10` card init, `11` card OK, `1F` card init failed, `02` started,
  `20`/`21` request (sync / work item).
- `8 0 vvvvpppp`: function 1 vendor/device after init (`02DF9131` expected), or `EEEE0sNN` on failure
  (s = SD_STATUS: 1 timeout, 2 command error, 4 card error, 5 no card; NN = init attempts).
- `8 1 CCCCEEII`: every ~1 s: CMD53 transfers (low 16 bits), errors (8 bits), card interrupts (8 bits).

DbgPrint lines (`goyasdio: ...`) go to the goyalog boot log: card IDs, clock, failed requests.

## Testing

`tools/sdiosim/sim.c` runs `sdiocore.c` against a model of the controller and an 88W8777-shaped card
(init, CMD52, block lengths, CMD53 byte/block mode, card interrupt gating, error paths); CI runs it.
