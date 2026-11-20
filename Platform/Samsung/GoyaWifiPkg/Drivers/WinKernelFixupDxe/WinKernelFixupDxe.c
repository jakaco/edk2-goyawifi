/** @file
  Windows 8 build 8520 (ARM) kernel fix-up for the PXA986.

  The Cortex-A9 in this SoC has its debug logic locked for software: every
  extended CP14 debug register (DBGOSLSR, DBGPRSR, DBGDSCRext, DBGVCR,
  DBGBCR/BVR/WCR) is UNDEFINED, and its memory-mapped view does not answer
  either (GoyaWifiDiag page 1). Samsung production parts fuse this off.

  The 8520 kernel initialises those registers unconditionally on every
  processor (it bugchecks 0x5D if ID_DFR0 reports no CP14 debug), so it takes
  an undefined instruction while starting and stops with
  DRIVER_VERIFIER_DETECTED_VIOLATION 0xC4 / 0x91.

  At ExitBootServices winload has loaded and relocated ntoskrnl.exe but not
  started it. This driver finds the two calls to the per-processor debug
  register initialisation in the loaded image and replaces them with NOPs.
  The kernel then records 0 breakpoints/watchpoints for the processor
  (PRCB fields stay zero) and never touches the debug registers again:
  hardware breakpoints are simply unavailable, which nothing in WinPE needs.

  Byte patterns are taken from ntoskrnl.exe 6.2.8520.0 (fbl_eeap.120722-1632).
  The code is position independent (relative branches, PC-relative
  literals), so the bytes are the same wherever the image was loaded. If the
  patterns are not found nothing is changed.

  A small square in the bottom-right corner of the screen reports the result
  (it stays visible behind the Windows logo and the stop screen):
    green  = both sites patched      yellow = only one site found
    red    = not found (other kernel build or not Windows)

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>

typedef struct {
  CONST CHAR8    *Name;
  UINT8          Pattern[24];
  UINTN          Length;
  UINTN          PatchOffset;     // offset of the 4-byte BL to replace
} KERNEL_SITE;

STATIC CONST KERNEL_SITE  mSites[] = {
  //  mrc p15,0,r3,c0,c1,2 (ID_DFR0); ands r3,#0xf; cmp r3,#4; blo.w <bugcheck 0x5D>;
  //  bl <KiInitializeDebugRegisters>; pop {r2,r3,fp,pc}
  {
    "KiInitializeProcessor debug init",
    { 0x10, 0xee, 0x51, 0x3f, 0x13, 0xf0, 0x0f, 0x03, 0x04, 0x2b, 0xd3, 0xf0,
      0xf9, 0xad, 0x00, 0xf0, 0xd1, 0xf9, 0xbd, 0xe8, 0x0c, 0x88 },
    22, 14
  },
  //  ldr r3,=<flag>; ldrb r3,[r3]; cbz r3,...; bl <KiInitializeDebugRegisters>
  {
    "resume path debug init",
    { 0x0f, 0x4b, 0x1b, 0x78, 0x9b, 0xb1, 0xff, 0xf7, 0x70, 0xfd },
    10, 6
  },
};

#define SITE_COUNT  (sizeof (mSites) / sizeof (mSites[0]))

STATIC CONST UINT8  mThumbNop2[4] = { 0x00, 0xbf, 0x00, 0xbf };   // nop; nop

STATIC EFI_EVENT              mExitBootServicesEvent;
STATIC EFI_MEMORY_DESCRIPTOR  *mMap;
STATIC UINTN                  mMapBufferSize;

STATIC
VOID
DrawMarker (
  IN UINT32  Color
  )
{
  UINT32  *Fb     = (UINT32 *)(UINTN)FixedPcdGet32 (PcdFrameBufferBase);
  UINT32  Stride  = FixedPcdGet32 (PcdFrameBufferStride);
  UINT32  Width   = FixedPcdGet32 (PcdFrameBufferWidth);
  UINT32  Height  = FixedPcdGet32 (PcdFrameBufferHeight);
  UINT32  X, Y;

  for (Y = Height - 20; Y < Height - 4; Y++) {
    for (X = Width - 20; X < Width - 4; X++) {
      Fb[Y * Stride + X] = Color;
    }
  }

  WriteBackDataCacheRange (Fb + (Height - 20) * Stride, 16 * Stride * 4);
}

/**
  Scan one region (2-byte aligned, Thumb code) for every site.
**/
STATIC
VOID
ScanRegion (
  IN     UINT8  *Base,
  IN     UINTN  Size,
  IN OUT UINTN  *Found
  )
{
  UINT16  *P;
  UINT16  *End;
  UINTN   S;
  UINT16  First[SITE_COUNT];

  for (S = 0; S < SITE_COUNT; S++) {
    First[S] = (UINT16)(mSites[S].Pattern[0] | (mSites[S].Pattern[1] << 8));
  }

  End = (UINT16 *)(Base + Size - 32);
  for (P = (UINT16 *)Base; P < End; P++) {
    for (S = 0; S < SITE_COUNT; S++) {
      if ((*P == First[S]) && (CompareMem (P, mSites[S].Pattern, mSites[S].Length) == 0)) {
        UINT8  *Bl = (UINT8 *)P + mSites[S].PatchOffset;

        CopyMem (Bl, mThumbNop2, sizeof (mThumbNop2));
        WriteBackInvalidateDataCacheRange (Bl, sizeof (mThumbNop2));
        InvalidateInstructionCacheRange (Bl, sizeof (mThumbNop2));
        Found[S]++;
        DEBUG ((DEBUG_INFO, "WinKernelFixup: patched %a at %p\n", mSites[S].Name, Bl));
      }
    }
  }
}

STATIC
VOID
EFIAPI
OnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINTN                  MapSize = mMapBufferSize;
  UINTN                  MapKey;
  UINTN                  DescSize;
  UINT32                 DescVer;
  UINT8                  *D;
  UINTN                  Found[SITE_COUNT];
  UINTN                  S;
  UINTN                  Sites = 0;

  ZeroMem (Found, sizeof (Found));
  if ((mMap == NULL) || EFI_ERROR (gBS->GetMemoryMap (&MapSize, mMap, &MapKey, &DescSize, &DescVer))) {
    DrawMarker (0x00FF00FF);    // magenta: could not read the memory map
    return;
  }

  for (D = (UINT8 *)mMap; D < (UINT8 *)mMap + MapSize; D += DescSize) {
    EFI_MEMORY_DESCRIPTOR  *Desc = (EFI_MEMORY_DESCRIPTOR *)D;

    // winload loads the kernel into loader memory; boot services data is scanned too in case
    // a loader allocates its image buffers there
    if (((Desc->Type == EfiLoaderCode) || (Desc->Type == EfiLoaderData) || (Desc->Type == EfiBootServicesData)) &&
        (Desc->NumberOfPages > 0))
    {
      ScanRegion ((UINT8 *)(UINTN)Desc->PhysicalStart, EFI_PAGES_TO_SIZE ((UINTN)Desc->NumberOfPages), Found);
    }
  }

  for (S = 0; S < SITE_COUNT; S++) {
    Sites += (Found[S] != 0) ? 1 : 0;
  }

  DrawMarker ((Sites == SITE_COUNT) ? 0x0000FF00 : (Sites != 0) ? 0x00FFFF00 : 0x00FF0000);
}

EFI_STATUS
EFIAPI
WinKernelFixupDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINTN   MapSize = 0;
  UINTN   MapKey;
  UINTN   DescSize;
  UINT32  DescVer;

  // The map grows while the OS loader runs; leave plenty of room (no allocation is allowed at EBS)
  gBS->GetMemoryMap (&MapSize, NULL, &MapKey, &DescSize, &DescVer);
  mMapBufferSize = MapSize + 256 * DescSize;
  mMap           = AllocateRuntimePool (mMapBufferSize);

  return gBS->CreateEvent (
                EVT_SIGNAL_EXIT_BOOT_SERVICES,
                TPL_NOTIFY,
                OnExitBootServices,
                NULL,
                &mExitBootServicesEvent
                );
}
