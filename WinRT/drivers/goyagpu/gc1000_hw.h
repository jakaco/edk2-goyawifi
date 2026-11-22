/*
 * gc1000_hw.h - Vivante GC1000 hardware layer for the goyagpu WDDM driver (Windows 8.0 RT, ARM32).
 *
 * Plain kernel C (ntddk.h), no WDDM types, so it can be tested on its own. The caller (the WDDM
 * KMD) maps the register windows (DxgkCbMapMemory or MmMapIoSpace) and provides the IRQL rules:
 *   GcPowerOn/GcPowerOff/GcReset/GcHwInit/GcMmuInit/GcRingInit  PASSIVE_LEVEL (StartDevice)
 *   GcMmuMap/GcMmuUnmap/GcSubmit                                 <= DISPATCH_LEVEL, serialised
 *   GcIsrAck                                                      device IRQL (ISR)
 * Start order: GcPowerOn, GcReset, GcIdentify, GcMmuInit, GcRingInit, GcHwInit (GcIdentify clears
 * the chip info, GcMmuInit then fills in the aperture size).
 * GcSubmit: the command buffer must already be visible to the GPU (non-cached or write-combined
 * mapping, or cleaned from the data cache with KeFlushIoBuffers): the GPU does not snoop the CPU.
 */
#pragma once
#include "gc1000_regs.h"
#include "etnaviv_wddm_shared.h"

typedef struct _GC_HW {
    /* register windows, mapped by the caller (non-cached device memory) */
    volatile UCHAR *Regs;               /* GC_REGS_PHYS, GC_REGS_SIZE */
    volatile UCHAR *Apmu;               /* PXA_APMU_PHYS, APMU_MAP_SIZE */

    GOYAGPU_CHIP_INFO Chip;             /* filled by GcIdentify */
    ULONG IdleMask;
    ULONG MemoryBase;                   /* linear window base: 0 (RAM starts at 0) */

    /* clock setup chosen at power-on */
    ULONG CoreSrc, CoreDiv, AxiSrc, AxiDiv;

    /* front-end ring (kernel command buffer): WAIT/LINK idle loop + submission glue */
    PULONG Ring;                        /* non-cached CPU mapping */
    ULONG  RingGpu;                     /* GPU address (linear window) */
    ULONG  RingSize;                    /* bytes */
    ULONG  RingPos;                     /* bytes written (Linux user_size) */
    ULONG  ExecState;                   /* GOYAGPU_PIPE_3D / _2D, or GC_PIPE_NONE */
    BOOLEAN MmuFlushPending;            /* table changed since the last submission */

    /* MMUv1 */
    PULONG Pt;                          /* non-cached CPU mapping of the page table */
    ULONG  PtPhys;
    ULONG  PtEntries;                   /* 4 KiB pages covered from GOYAGPU_APERTURE_BASE */
    PULONG BadPage;                     /* unmapped entries point here (filled 0xdead55aa) */
    ULONG  BadPagePhys;

    /* statistics */
    ULONG Resets, MmuExceptions, AxiBusErrors;
} GC_HW;

#define GC_PIPE_NONE    0xFFFFFFFFUL

/* power, reset, identification, init */
NTSTATUS GcPowerOn(GC_HW *Hw);
VOID     GcPowerOff(GC_HW *Hw);
NTSTATUS GcReset(GC_HW *Hw);
VOID     GcIdentify(GC_HW *Hw);
NTSTATUS GcRingInit(GC_HW *Hw);         /* allocates the ring (contiguous, non-cached) */
VOID     GcRingFree(GC_HW *Hw);
NTSTATUS GcHwInit(GC_HW *Hw);           /* clock gating, AXI, pulse eater, MMU, ring, FE start */

/* MMUv1 (gc1000_mmu.c) */
NTSTATUS GcMmuInit(GC_HW *Hw);          /* allocates the table: up to 2 MiB (2 GiB aperture) */
VOID     GcMmuFree(GC_HW *Hw);
VOID     GcMmuRestore(GC_HW *Hw);       /* writes memory bases + table address to the MC */
NTSTATUS GcMmuMap(GC_HW *Hw, ULONG GpuVa, PHYSICAL_ADDRESS Pa);   /* one 4 KiB page */
VOID     GcMmuUnmap(GC_HW *Hw, ULONG GpuVa);
ULONG    GcMmuApertureSize(GC_HW *Hw);

/* Submission: queue the command buffer at CmdGpu/CmdCpu (CmdBytes of commands, 8-byte aligned,
   with GOYAGPU_DMA_TAIL_BYTES free behind them) on Pipe; Event (0..29) fires when the pixel engine
   has finished it. Same sequence as Linux etnaviv_buffer_queue. */
VOID     GcSubmit(GC_HW *Hw, PULONG CmdCpu, ULONG CmdGpu, ULONG CmdBytes, ULONG Pipe, ULONG Event);

/* Interrupts: returns HI_INTR_ACKNOWLEDGE (reading it acknowledges). Event bits 0..29, bit 30 MMU
   exception, bit 31 AXI bus error. */
ULONG    GcIsrAck(GC_HW *Hw);

/* Diagnostics */
VOID     GcGetStatus(GC_HW *Hw, GOYAGPU_GPU_STATUS *St);
BOOLEAN  GcIsIdle(GC_HW *Hw);
