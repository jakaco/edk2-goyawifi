/*
 * gc1000_mmu.c - Vivante MMUv1 for the GC1000 (Linux 4.19 etnaviv_iommu.c).
 *
 * MMUv1 is one flat table of 32-bit entries, one per 4 KiB page, indexed by
 * (GPU address - 0x80000000) >> 12; an entry is the physical address of the page. Below 0x80000000
 * the GPU uses the linear window (physical address + memory base) without translation. Every unused
 * entry points at a "bad page" filled with 0xdead55aa, so a stray GPU access reads garbage instead
 * of random memory. The same table address goes into all five MC page-table registers.
 *
 * Linux allocates the full 2 MiB table (2 GiB aperture). On a 1 GiB tablet a 2 MiB physically
 * contiguous block may not be available once Windows is up, so smaller tables are tried in turn;
 * the aperture is then smaller (still >= 256 MiB). The GPU must never use addresses beyond it.
 *
 * The table lives in non-cached memory; GcSubmit issues a DSB and an MMU flush (GL_FLUSH_MMU) in
 * the command stream before the next submission runs, as etnaviv does.
 */
#include <ntddk.h>
#include "gc1000_hw.h"

static PVOID AllocNonCached(ULONG Bytes, PULONG Phys)
{
    PHYSICAL_ADDRESS lo, hi, bound, pa;
    PVOID p;

    lo.QuadPart = 0;
    hi.QuadPart = 0x7FFFFFFF;
    bound.QuadPart = 0;
    p = MmAllocateContiguousMemorySpecifyCache(Bytes, lo, hi, bound, MmNonCached);
    if (p == NULL) return NULL;
    pa = MmGetPhysicalAddress(p);
    *Phys = pa.LowPart;
    return p;
}

NTSTATUS GcMmuInit(GC_HW *Hw)
{
    static const ULONG sizes[] = { 2UL << 20, 1UL << 20, 512UL << 10, 256UL << 10 };
    ULONG i, n;

    Hw->BadPage = (PULONG)AllocNonCached(GOYAGPU_PAGE_SIZE, &Hw->BadPagePhys);
    if (Hw->BadPage == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    for (i = 0; i < GOYAGPU_PAGE_SIZE / 4; i++) Hw->BadPage[i] = 0xdead55aaUL;

    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]) && Hw->Pt == NULL; i++) {
        Hw->Pt = (PULONG)AllocNonCached(sizes[i], &Hw->PtPhys);
        if (Hw->Pt != NULL) Hw->PtEntries = sizes[i] / 4;
    }
    if (Hw->Pt == NULL) {
        MmFreeContiguousMemorySpecifyCache(Hw->BadPage, GOYAGPU_PAGE_SIZE, MmNonCached);
        Hw->BadPage = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    n = Hw->PtEntries;
    for (i = 0; i < n; i++) Hw->Pt[i] = Hw->BadPagePhys;
    Hw->Chip.ApertureSize = GcMmuApertureSize(Hw);
    Hw->MmuFlushPending = TRUE;
    return STATUS_SUCCESS;
}

VOID GcMmuFree(GC_HW *Hw)
{
    if (Hw->Pt) MmFreeContiguousMemorySpecifyCache(Hw->Pt, Hw->PtEntries * 4, MmNonCached);
    if (Hw->BadPage) MmFreeContiguousMemorySpecifyCache(Hw->BadPage, GOYAGPU_PAGE_SIZE, MmNonCached);
    Hw->Pt = NULL; Hw->BadPage = NULL; Hw->PtEntries = 0;
}

ULONG GcMmuApertureSize(GC_HW *Hw)
{
    return Hw->PtEntries * GOYAGPU_PAGE_SIZE;     /* <= 2 GiB, fits in 32 bits */
}

/* etnaviv_iommuv1_restore: memory bases (linear window) and the table address in the MC */
VOID GcMmuRestore(GC_HW *Hw)
{
    volatile UCHAR *r = Hw->Regs;
    WRITE_REGISTER_ULONG((volatile ULONG *)(r + VIVS_MC_MEMORY_BASE_ADDR_RA),  Hw->MemoryBase);
    WRITE_REGISTER_ULONG((volatile ULONG *)(r + VIVS_MC_MEMORY_BASE_ADDR_FE),  Hw->MemoryBase);
    WRITE_REGISTER_ULONG((volatile ULONG *)(r + VIVS_MC_MEMORY_BASE_ADDR_TX),  Hw->MemoryBase);
    WRITE_REGISTER_ULONG((volatile ULONG *)(r + VIVS_MC_MEMORY_BASE_ADDR_PEZ), Hw->MemoryBase);
    WRITE_REGISTER_ULONG((volatile ULONG *)(r + VIVS_MC_MEMORY_BASE_ADDR_PE),  Hw->MemoryBase);

    WRITE_REGISTER_ULONG((volatile ULONG *)(r + VIVS_MC_MMU_FE_PAGE_TABLE),  Hw->PtPhys);
    WRITE_REGISTER_ULONG((volatile ULONG *)(r + VIVS_MC_MMU_TX_PAGE_TABLE),  Hw->PtPhys);
    WRITE_REGISTER_ULONG((volatile ULONG *)(r + VIVS_MC_MMU_PE_PAGE_TABLE),  Hw->PtPhys);
    WRITE_REGISTER_ULONG((volatile ULONG *)(r + VIVS_MC_MMU_PEZ_PAGE_TABLE), Hw->PtPhys);
    WRITE_REGISTER_ULONG((volatile ULONG *)(r + VIVS_MC_MMU_RA_PAGE_TABLE),  Hw->PtPhys);
}

NTSTATUS GcMmuMap(GC_HW *Hw, ULONG GpuVa, PHYSICAL_ADDRESS Pa)
{
    ULONG index;

    if (GpuVa < GOYAGPU_APERTURE_BASE || (GpuVa & (GOYAGPU_PAGE_SIZE - 1)) || (Pa.LowPart & (GOYAGPU_PAGE_SIZE - 1)) ||
        Pa.HighPart != 0)
        return STATUS_INVALID_PARAMETER;
    index = (GpuVa - GOYAGPU_APERTURE_BASE) >> 12;
    if (index >= Hw->PtEntries) return STATUS_INVALID_PARAMETER;
    Hw->Pt[index] = Pa.LowPart;
    Hw->MmuFlushPending = TRUE;
    return STATUS_SUCCESS;
}

VOID GcMmuUnmap(GC_HW *Hw, ULONG GpuVa)
{
    ULONG index;

    if (GpuVa < GOYAGPU_APERTURE_BASE) return;
    index = (GpuVa - GOYAGPU_APERTURE_BASE) >> 12;
    if (index >= Hw->PtEntries) return;
    Hw->Pt[index] = Hw->BadPagePhys;
    Hw->MmuFlushPending = TRUE;
}
