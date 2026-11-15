/** @file
  goyawifi hardware keys as an EFI Simple Text Input device.

  Keys are active-low GPIOs: Volume Up = GPIO 1, Volume Down = GPIO 2,
  Home = GPIO 13 (see the goyawifi device tree).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Guid/ConsoleInDevice.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SimpleTextIn.h>
#include <Pxa988.h>

typedef struct {
  UINT32    Gpio;
  UINT16    ScanCode;
  CHAR16    Unicode;
} KEY_MAP;

STATIC CONST KEY_MAP  mKeys[] = {
  { 1,  SCAN_UP,   CHAR_NULL            },   // Volume Up
  { 2,  SCAN_DOWN, CHAR_NULL            },   // Volume Down
  { 13, SCAN_NULL, CHAR_CARRIAGE_RETURN },   // Home
};

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} KEYPAD_DEVICE_PATH;
#pragma pack()

STATIC KEYPAD_DEVICE_PATH  mDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH, HW_VENDOR_DP,
      { (UINT8)sizeof (VENDOR_DEVICE_PATH), (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8) }
    },
    { 0x2f1e7c40, 0x6b8d, 0x4a3e, { 0xb5, 0x91, 0x7c, 0x0d, 0x3e, 0x2a, 0x1f, 0x58 } }
  },
  {
    END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL  mTextIn;
STATIC EFI_HANDLE                      mHandle;
STATIC BOOLEAN                         mPressed[ARRAY_SIZE (mKeys)];
STATIC INTN                            mPending = -1;

STATIC
BOOLEAN
KeyIsDown (
  IN UINT32  Gpio
  )
{
  return (MmioRead32 (GPIO_BANK (Gpio) + GPIO_GPLR) & (1U << (Gpio % 32))) == 0;
}

STATIC
VOID
KeypadPoll (
  VOID
  )
{
  UINTN    Index;
  BOOLEAN  Down;

  for (Index = 0; Index < ARRAY_SIZE (mKeys); Index++) {
    Down = KeyIsDown (mKeys[Index].Gpio);
    if (Down && !mPressed[Index] && (mPending < 0)) {
      mPending = (INTN)Index;      // report on press edge
    }

    mPressed[Index] = Down;
  }
}

STATIC
EFI_STATUS
EFIAPI
KeypadReset (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  IN BOOLEAN                         ExtendedVerification
  )
{
  UINTN  Index;

  // Ignore keys already held, e.g. the one used to get here
  for (Index = 0; Index < ARRAY_SIZE (mKeys); Index++) {
    mPressed[Index] = KeyIsDown (mKeys[Index].Gpio);
  }

  mPending = -1;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
KeypadReadKeyStroke (
  IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  OUT EFI_INPUT_KEY                   *Key
  )
{
  if (Key == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  KeypadPoll ();
  if (mPending < 0) {
    return EFI_NOT_READY;
  }

  Key->ScanCode    = mKeys[mPending].ScanCode;
  Key->UnicodeChar = mKeys[mPending].Unicode;
  mPending         = -1;
  return EFI_SUCCESS;
}

STATIC
VOID
EFIAPI
KeypadWaitForKey (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  KeypadPoll ();
  if (mPending >= 0) {
    gBS->SignalEvent (Event);
  }
}

STATIC
VOID
EFIAPI
KeypadEndOfDxe (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  // Let ConSplitter pick us up (we carry the ConsoleInDevice tag)
  gBS->ConnectController (mHandle, NULL, NULL, TRUE);
  gBS->CloseEvent (Event);
}

EFI_STATUS
EFIAPI
KeypadInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   EndOfDxe;
  UINTN       Index;

  // Keys are GPIO inputs (S-Boot already muxes them, make sure of the dir)
  for (Index = 0; Index < ARRAY_SIZE (mKeys); Index++) {
    MmioWrite32 (GPIO_BANK (mKeys[Index].Gpio) + GPIO_GCDR, 1U << (mKeys[Index].Gpio % 32));
  }

  mTextIn.Reset         = KeypadReset;
  mTextIn.ReadKeyStroke = KeypadReadKeyStroke;

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_WAIT,
                  TPL_NOTIFY,
                  KeypadWaitForKey,
                  NULL,
                  &mTextIn.WaitForKey
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  KeypadReset (&mTextIn, FALSE);

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mHandle,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  &gEfiSimpleTextInProtocolGuid,
                  &mTextIn,
                  &gEfiConsoleInDeviceGuid,
                  NULL,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return gBS->CreateEventEx (
                EVT_NOTIFY_SIGNAL,
                TPL_CALLBACK,
                KeypadEndOfDxe,
                NULL,
                &gEfiEndOfDxeEventGroupGuid,
                &EndOfDxe
                );
}
