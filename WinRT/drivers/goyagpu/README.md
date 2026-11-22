# goyagpu — Vivante GC1000 3D driver for Windows 8.0 RT (build 8520, ARM32)

Status: **Steps 1–2 written** (shared KMD/UMD ABI, GC1000 hardware + MMUv1 layer).
Nothing here has run on the tablet yet. The WDDM KMD, the D3D9 UMD and the INF follow.

| File | Step | Contents |
|---|---|---|
| `etnaviv_wddm_shared.h` | 1 | KMD ↔ UMD ABI: adapter info, allocation / DMA-buffer / context private data, patch types, escapes |
| `gc1000_regs.h` | 2 | GC1000 and PXA988 APMU register map (from Linux 4.19 etnaviv + goyawifi DT) |
| `gc1000_hw.h`, `gc1000_hw.c` | 2 | Power/clock/reset, identification, clock gating, front-end ring (WAIT/LINK), submission, interrupt decode |
| `gc1000_mmu.c` | 2 | MMUv1 flat page table (GPU aperture 0x80000000+) |

Host test: `tools/gpusim/ringtest.c` runs power-on, reset, init, the ring and two submissions against
fake registers and checks every command word against Linux etnaviv (`cc -I stub -o ringtest ringtest.c`).

Build check: `build-gpu-check.cmd` compiles these files with the same ARM / NT 6.2 / no-NEON
flags as the other drivers and fails on VFP/NEON or SDIV/UDIV in the objects (CI: `goyagpu`).

## Hardware facts (verified against the goyawifi Linux 4.19 tree)

Sources: `arch/arm/boot/dts/pxa988.dtsi` (`gpu@c0400000`, `gc_power`),
`arch/arm/mach-mmp/pxa988-pm-domain.c` (power island), `drivers/clk/mmp/clk-of-pxa988.c` (GC
clocks), `drivers/gpu/drm/etnaviv/*` (reset, init, MMUv1, ring, IRQ), and the port notes
(`phase-13-gpu-etnaviv.md`: etnaviv + Mesa 26 etnaviv render kmscube at 60 fps on this tablet).

| Item | Value |
|---|---|
| GPU registers | **0xC0400000, size 0x40000** (not 0xD420A000 / 0x4000) |
| Interrupt | GIC SPI 8 = GSIV 40, level high (the ICU masking in the firmware does not affect GIC SPIs) |
| Identity | GC1000, revision **0x5037**; no HWDB entry, features come from the ID registers |
| MMU | MMUv1: one flat table of 4 KiB entries for GPU addresses 0x80000000–0xFFFFFFFF; below 0x80000000 the GPU addresses physical memory directly (`memory_base` = 0, RAM starts at 0) |
| APMU_GC | 0xD42828CC: resets bits 2:0, AXI clock bit 3, core clock bit 4, HCLK bit 5, core mux 7:6 + divider 14:12, hardware mode bit 11, AXI divider 19:17 + mux 21:20 (divider field = divisor − 1; parents 0 pll1_416, 1 pll1_624, 2 pll2, 3 pll2p) |
| Power island | PWR_CTRL 0xD42828D8 bit 0, PWR_BLK_TMR 0xD42828DC = 0x20001FFF, PWR_STATUS 0xD42828F0 bit 0 |
| Clocks | core 156 MHz (pll1_624 / 4), AXI 208 MHz (pll1_416 / 2); goya devfreq goes up to 624 MHz |
| Supply | buck1 (also CPU and DDR), already on |

Power-on order (Linux `pxa988_pd_power_on`): clocks on → PWR_BLK_TMR → APMU_GC hardware mode →
PWR_CTRL bit 0 → wait for PWR_STATUS bit 0 → release resets → 10–20 µs. Then etnaviv: soft reset
loop, identify, module clock gating, AXI cache attributes (2/2), pulse eater 0x01590880, MMU
bases + page table, WAIT/LINK ring, interrupt enable, front end start.

## Design notes for the next steps

* **Memory model (WDDM 1.x):** one *aperture segment* backed by the MMUv1 table (GPU VA
  0x80000000 + offset); `DxgkDdiBuildPagingBuffer` MAP/UNMAP_APERTURE_SEGMENT write the table
  entries, the next submission flushes the GPU MMU. DMA buffers come from physically contiguous
  system memory (DmaBufferSegmentSet = 0) and are reached through the linear window (GPU address =
  physical address). The Windows memory manager only hands out RAM from the UEFI memory map, so
  the firmware's reserved regions (CP carveout, ramoops, framebuffer) are never used.
* **Submission:** the UMD writes Vivante front-end command streams; allocation addresses are
  patched by the KMD (`DxgkDdiPatch`, patch type `GOYAGPU_PATCH_GPU_ADDRESS`). The KMD appends the
  return LINK, so every DMA buffer keeps `GOYAGPU_DMA_TAIL_BYTES` free at the end.
* **Fences:** one Vivante event (0–29) per submission; the ISR reads INTR_ACKNOWLEDGE (read
  clears), the DPC reports `DXGK_INTERRUPT_DMA_COMPLETED` with the fence of the newest finished
  submission (the front end executes in order). Bits 30/31 = MMU exception / AXI bus error.
* **Caches:** the PL310 is off and CPU1 is off. Ring and page table are non-cached; a DSB follows
  every table/ring update before the GPU can see it.

## Open questions (to settle before / while writing the KMD)

1. **DXGKDDI interface version accepted by build 8520's dxgkrnl.sys.** The final Windows 8 WDK
   defines `DXGKDDI_INTERFACE_VERSION_WIN8 = 0x300E`; 0x3004 is not a published value. The value
   must be read from the 8520 `dxgkrnl.sys` (`DxgkInitialize` version check) — copy
   `Windows\System32\drivers\dxgkrnl.sys` (and `dxgkrnl`-related `d3d10level9.dll`,
   `d3d11.dll`) off the card for disassembly.
2. **Who composes the desktop.** WinPE has no DWM and no Direct3D applications, so in WinPE the
   GPU is only reachable by a test tool. On full Windows RT, DWM composes on the adapter that owns
   the display. With `BasicDisplay.sys` keeping the display, a render-only GC1000 serves Direct3D
   applications, but desktop composition most likely stays on WARP. Accelerated DWM probably
   needs goyagpu to own the display as well (fixed 1024x600 VidPN, scanout at 0x17000000) —
   possible later, once rendering works.
3. **UMD size.** Gallium Nine + etnaviv for ARMNT is a large port (Nine's Linux winsys and
   presentation, Mesa's Windows build for 32-bit ARM, NEON off). Feature level 9_1 is the
   realistic target for a GLES2-class GC1000; 9_3 needs capabilities to be checked against the
   GC1000 feature bits. A minimal hand-written UMD (clears, blits, fixed shaders) is the quickest
   way to prove the KMD path first.
