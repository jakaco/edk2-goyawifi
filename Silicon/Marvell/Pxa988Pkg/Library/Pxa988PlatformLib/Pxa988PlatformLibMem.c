/** @file
  PXA988 virtual memory map.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Base.h>
#include <Library/ArmLib.h>
#include <Library/ArmPlatformLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>

#define MAX_REGIONS  6

#define FB_BASE  FixedPcdGet32 (PcdFrameBufferBase)
#define FB_SIZE  ALIGN_VALUE (FixedPcdGet32 (PcdFrameBufferStride) * 4 * \
                              FixedPcdGet32 (PcdFrameBufferHeight), SIZE_1MB)

/**
  DRAM is mapped write-back except for the bootloader framebuffer, which is
  mapped write-through so the LCD controller sees GOP writes without extra
  cache maintenance. Everything from 0x80000000 up is device memory.
**/
VOID
ArmPlatformGetVirtualMemoryMap (
  IN ARM_MEMORY_REGION_DESCRIPTOR  **VirtualMemoryMap
  )
{
  ARM_MEMORY_REGION_DESCRIPTOR  *Map;
  UINT64                        DramBase;
  UINT64                        DramSize;
  UINTN                         Index;

  DramBase = FixedPcdGet64 (PcdSystemMemoryBase);
  DramSize = FixedPcdGet64 (PcdSystemMemorySize);

  Map = AllocatePages (EFI_SIZE_TO_PAGES (sizeof (*Map) * MAX_REGIONS));
  ASSERT (Map != NULL);
  if (Map == NULL) {
    return;
  }

  Index = 0;

  // DRAM below the framebuffer
  Map[Index].PhysicalBase = DramBase;
  Map[Index].VirtualBase  = DramBase;
  Map[Index].Length       = FB_BASE - DramBase;
  Map[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
  Index++;

  // Framebuffer
  Map[Index].PhysicalBase = FB_BASE;
  Map[Index].VirtualBase  = FB_BASE;
  Map[Index].Length       = FB_SIZE;
  Map[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_THROUGH;
  Index++;

  // DRAM above the framebuffer
  Map[Index].PhysicalBase = FB_BASE + FB_SIZE;
  Map[Index].VirtualBase  = FB_BASE + FB_SIZE;
  Map[Index].Length       = DramBase + DramSize - (FB_BASE + FB_SIZE);
  Map[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
  Index++;

  // SoC: GPU/DDRC (0xC0xxxxxx), MPCore (0xD1xxxxxx), APB/AXI (0xD4xxxxxx)
  Map[Index].PhysicalBase = 0x80000000;
  Map[Index].VirtualBase  = 0x80000000;
  Map[Index].Length       = 0x80000000;
  Map[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;
  Index++;

  // End of table
  Map[Index].PhysicalBase = 0;
  Map[Index].VirtualBase  = 0;
  Map[Index].Length       = 0;
  Map[Index].Attributes   = 0;

  ASSERT (Index < MAX_REGIONS);
  *VirtualMemoryMap = Map;
}
