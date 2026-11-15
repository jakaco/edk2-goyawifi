/** @file
  GOP driver for a pre-initialised linear framebuffer (a8r8g8b8).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>

#define FB_BASE    FixedPcdGet32 (PcdFrameBufferBase)
#define FB_WIDTH   FixedPcdGet32 (PcdFrameBufferWidth)
#define FB_HEIGHT  FixedPcdGet32 (PcdFrameBufferHeight)
#define FB_STRIDE  FixedPcdGet32 (PcdFrameBufferStride)   // in pixels

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} SIMPLE_FB_DEVICE_PATH;
#pragma pack()

STATIC SIMPLE_FB_DEVICE_PATH  mDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH, HW_VENDOR_DP,
      { (UINT8)sizeof (VENDOR_DEVICE_PATH), (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8) }
    },
    { 0x6c4b7a1e, 0x2d3f, 0x4e8a, { 0x9b, 0x5c, 0x0a, 0x1f, 0x2e, 0x3d, 0x4c, 0x6b } }
  },
  {
    END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  mModeInfo;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE     mMode;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL          mGop;
STATIC FRAME_BUFFER_CONFIGURE                *mFbConfigure;

STATIC
EFI_STATUS
EFIAPI
SimpleFbQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  **Info
  )
{
  if ((ModeNumber != 0) || (SizeOfInfo == NULL) || (Info == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Info = AllocateCopyPool (sizeof (mModeInfo), &mModeInfo);
  if (*Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  *SizeOfInfo = sizeof (mModeInfo);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SimpleFbSetMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *This,
  IN UINT32                        ModeNumber
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Black;

  if (ModeNumber != 0) {
    return EFI_UNSUPPORTED;
  }

  ZeroMem (&Black, sizeof (Black));
  return FrameBufferBlt (
           mFbConfigure,
           &Black,
           EfiBltVideoFill,
           0,
           0,
           0,
           0,
           FB_WIDTH,
           FB_HEIGHT,
           0
           );
}

STATIC
EFI_STATUS
EFIAPI
SimpleFbBlt (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL       *This,
  IN  EFI_GRAPHICS_OUTPUT_BLT_PIXEL      *BltBuffer  OPTIONAL,
  IN  EFI_GRAPHICS_OUTPUT_BLT_OPERATION  BltOperation,
  IN  UINTN                              SourceX,
  IN  UINTN                              SourceY,
  IN  UINTN                              DestinationX,
  IN  UINTN                              DestinationY,
  IN  UINTN                              Width,
  IN  UINTN                              Height,
  IN  UINTN                              Delta         OPTIONAL
  )
{
  return FrameBufferBlt (
           mFbConfigure,
           BltBuffer,
           BltOperation,
           SourceX,
           SourceY,
           DestinationX,
           DestinationY,
           Width,
           Height,
           Delta
           );
}

EFI_STATUS
EFIAPI
SimpleFbDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINTN       ConfigureSize;
  EFI_HANDLE  Handle;

  mModeInfo.Version                       = 0;
  mModeInfo.HorizontalResolution          = FB_WIDTH;
  mModeInfo.VerticalResolution            = FB_HEIGHT;
  mModeInfo.PixelFormat                   = PixelBlueGreenRedReserved8BitPerColor;
  mModeInfo.PixelsPerScanLine             = FB_STRIDE;

  mMode.MaxMode         = 1;
  mMode.Mode            = 0;
  mMode.Info            = &mModeInfo;
  mMode.SizeOfInfo      = sizeof (mModeInfo);
  mMode.FrameBufferBase = FB_BASE;
  mMode.FrameBufferSize = (UINTN)FB_STRIDE * FB_HEIGHT * 4;

  mGop.QueryMode = SimpleFbQueryMode;
  mGop.SetMode   = SimpleFbSetMode;
  mGop.Blt       = SimpleFbBlt;
  mGop.Mode      = &mMode;

  ConfigureSize = 0;
  Status        = FrameBufferBltConfigure ((VOID *)(UINTN)FB_BASE, &mModeInfo, NULL, &ConfigureSize);
  if (Status != RETURN_BUFFER_TOO_SMALL) {
    return EFI_DEVICE_ERROR;
  }

  mFbConfigure = AllocatePool (ConfigureSize);
  if (mFbConfigure == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = FrameBufferBltConfigure ((VOID *)(UINTN)FB_BASE, &mModeInfo, mFbConfigure, &ConfigureSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  &gEfiGraphicsOutputProtocolGuid,
                  &mGop,
                  NULL
                  );
  DEBUG ((DEBUG_INFO, "SimpleFb: %ux%u @ 0x%x: %r\n", FB_WIDTH, FB_HEIGHT, FB_BASE, Status));

  return Status;
}
