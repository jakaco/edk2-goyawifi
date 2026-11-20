/** @file
  PXA988 ChipIdea USB device controller, polled, with a CDC-ACM serial function exposed as
  EFI_SERIAL_IO_PROTOCOL (consumed by TerminalDxe -> console on a PC's serial terminal).

  The register layout and the reset/start/setup sequence follow U-Boot's drivers/usb/gadget/ci_udc.c
  (non-HOSTPC layout), which is known to enumerate fastboot on this very tablet:
    - the port is forced to full speed (PORTSC.PFSC): the PHY PLL-ready bit never sets, so high speed
      is not trusted; bulk packets are 64 bytes
    - no "advance" bit for SET_ADDRESS: the address is written after the status stage completed
    - queue-head list 4 KiB aligned, setup data read from the queue head
  Transfers are polled from a timer event (and from Read/Write), no interrupts.

  The queue heads, transfer descriptors and bounce buffers live in one page-aligned arena that is
  mapped uncached, so no cache maintenance is needed.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SerialIo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Pxa988.h>
#include <Guid/Pxa988UsbSerial.h>
#include "UsbGadget.h"

//
// Registers (offsets from PXA988_USB_BASE)
//
#define UDC_USBCMD      0x140
#define UDC_USBSTS      0x144
#define UDC_DEVADDR     0x154
#define UDC_EPLIST      0x158
#define UDC_PORTSC      0x184
#define UDC_USBMODE     0x1A8
#define UDC_EPSETUPSTAT 0x1AC
#define UDC_EPPRIME     0x1B0
#define UDC_EPFLUSH     0x1B4
#define UDC_EPSTAT      0x1B8
#define UDC_EPCOMP      0x1BC
#define UDC_EPCTRL(n)   (0x1C0 + 4 * (n))

#define UDC_R32(o)      MmioRead32 (PXA988_USB_BASE + (o))
#define UDC_W32(o, v)   MmioWrite32 (PXA988_USB_BASE + (o), (v))

#define CMD_RUN         BIT0
#define CMD_RST         BIT1
#define CMD_SUTW        BIT13
#define CMD_ITC8        (8U << 16)
#define STS_URI         BIT6
#define STS_PCI         BIT2
#define PORTSC_PFSC     BIT24
#define USBMODE_DEVICE  2

#define EPT_RX(n)       (1U << (n))
#define EPT_TX(n)       (1U << ((n) + 16))
#define CTRL_RXS        BIT0
#define CTRL_RXT(t)     ((UINT32)(t) << 2)
#define CTRL_RXR        BIT6
#define CTRL_RXE        BIT7
#define CTRL_TXS        BIT16
#define CTRL_TXT(t)     ((UINT32)(t) << 18)
#define CTRL_TXR        BIT22
#define CTRL_TXE        BIT23

#define CFG_MAX_PKT(n)  ((UINT32)(n) << 16)
#define CFG_ZLT         BIT29
#define CFG_IOS         BIT15

#define DTD_ACTIVE      0x80
#define DTD_IOC         BIT15

//
// Endpoint numbers of the CDC-ACM function
//
#define EP_BULK         1          // bulk OUT (0x01) and bulk IN (0x81)
#define EP_NOTIFY       2          // interrupt IN (0x82), never used for data
#define MAX_PKT_FS      64
#define RING_SIZE       2048

#pragma pack (1)
typedef struct {
  UINT32    Config;
  UINT32    Current;
  UINT32    Next;
  UINT32    Info;
  UINT32    Page[5];
  UINT32    Reserved0;
  UINT8     Setup[8];
  UINT32    Reserved1[4];
} QH;                            // 64 bytes

typedef struct {
  UINT32    Next;
  UINT32    Info;
  UINT32    Page[5];
  UINT32    Reserved;
} DTD;                           // 32 bytes

typedef struct {
  UINT8     RequestType;
  UINT8     Request;
  UINT16    Value;
  UINT16    Index;
  UINT16    Length;
} SETUP_PACKET;
#pragma pack ()

typedef struct {
  QH       Qh[8];                // 0x000: 8 queue heads (4 endpoint numbers x IN/OUT), 512 bytes
  UINT8    Pad0[0x800 - sizeof (QH) * 8];
  DTD      Dtd[8];               // 0x800
  UINT8    Pad1[0x1000 - 0x800 - sizeof (DTD) * 8];
  UINT8    Ep0Buf[256];          // 0x1000
  UINT8    BulkOut[MAX_PKT_FS * 2];
  UINT8    BulkIn[MAX_PKT_FS * 4];
} ARENA;

STATIC ARENA   *mArena;
STATIC BOOLEAN mConfigured;
STATIC BOOLEAN mDtr;
STATIC UINT8   mConfigValue;
STATIC UINT8   mLineCoding[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };      // 115200 8N1
STATIC UINT8   mRing[RING_SIZE];
STATIC UINTN   mRingHead, mRingTail;
STATIC UINTN   mTxFailures;
STATIC EFI_EVENT mTimer;

//
// ---------------------------------------------------------------------------------------------
//
STATIC
UINTN
QhIndex (
  IN UINTN    Ep,
  IN BOOLEAN  In
  )
{
  return Ep * 2 + (In ? 1 : 0);
}

STATIC
VOID
Prime (
  IN UINTN    Ep,
  IN BOOLEAN  In,
  IN VOID     *Buf,
  IN UINTN    Len
  )
{
  UINTN  I   = QhIndex (Ep, In);
  QH     *Q  = &mArena->Qh[I];
  DTD    *D  = &mArena->Dtd[I];
  UINT32 Pg  = (UINT32)(UINTN)Buf;
  UINTN  K;

  ZeroMem (D, sizeof (*D));
  D->Next  = 1;
  D->Info  = ((UINT32)Len << 16) | DTD_IOC | DTD_ACTIVE;
  D->Page[0] = Pg;
  for (K = 1; K < 5; K++) {
    D->Page[K] = (Pg & ~0xFFFU) + 0x1000 * K;
  }

  Q->Next = (UINT32)(UINTN)D;
  Q->Info = 0;
  MemoryFence ();
  UDC_W32 (UDC_EPPRIME, In ? EPT_TX (Ep) : EPT_RX (Ep));
}

STATIC
BOOLEAN
Done (
  IN UINTN    Ep,
  IN BOOLEAN  In
  )
{
  return (mArena->Dtd[QhIndex (Ep, In)].Info & DTD_ACTIVE) == 0;
}

STATIC
UINTN
Residual (
  IN UINTN    Ep,
  IN BOOLEAN  In
  )
{
  return (mArena->Dtd[QhIndex (Ep, In)].Info >> 16) & 0x7FFF;
}

STATIC
BOOLEAN
WaitDone (
  IN UINTN    Ep,
  IN BOOLEAN  In,
  IN UINTN    TimeoutMs
  )
{
  UINTN  T;

  for (T = 0; T < TimeoutMs * 10; T++) {
    if (Done (Ep, In)) {
      return TRUE;
    }

    MicroSecondDelay (100);
  }

  return Done (Ep, In);
}

STATIC
VOID
FlushAll (
  VOID
  )
{
  UINTN  T;

  UDC_W32 (UDC_EPFLUSH, 0xFFFFFFFF);
  for (T = 0; T < 1000 && UDC_R32 (UDC_EPFLUSH) != 0; T++) {
    MicroSecondDelay (10);
  }
}

STATIC
VOID
ConfigureEp0 (
  VOID
  )
{
  mArena->Qh[QhIndex (0, FALSE)].Config = CFG_MAX_PKT (64) | CFG_ZLT | CFG_IOS;
  mArena->Qh[QhIndex (0, TRUE)].Config  = CFG_MAX_PKT (64) | CFG_ZLT;
  UDC_W32 (UDC_EPCTRL (0), CTRL_RXE | CTRL_TXE);
}

STATIC
VOID
EnableDataEndpoints (
  VOID
  )
{
  mArena->Qh[QhIndex (EP_BULK, FALSE)].Config   = CFG_MAX_PKT (MAX_PKT_FS) | CFG_ZLT;
  mArena->Qh[QhIndex (EP_BULK, TRUE)].Config    = CFG_MAX_PKT (MAX_PKT_FS) | CFG_ZLT;
  mArena->Qh[QhIndex (EP_NOTIFY, TRUE)].Config  = CFG_MAX_PKT (16) | CFG_ZLT;
  UDC_W32 (UDC_EPCTRL (EP_BULK), CTRL_RXE | CTRL_RXR | CTRL_RXT (2) | CTRL_TXE | CTRL_TXR | CTRL_TXT (2));
  UDC_W32 (UDC_EPCTRL (EP_NOTIFY), CTRL_TXE | CTRL_TXR | CTRL_TXT (3));
  Prime (EP_BULK, FALSE, mArena->BulkOut, MAX_PKT_FS);
}

STATIC
VOID
DisableDataEndpoints (
  VOID
  )
{
  UDC_W32 (UDC_EPCTRL (EP_BULK), 0);
  UDC_W32 (UDC_EPCTRL (EP_NOTIFY), 0);
}

STATIC
VOID
BusReset (
  VOID
  )
{
  UDC_W32 (UDC_EPSETUPSTAT, UDC_R32 (UDC_EPSETUPSTAT));
  UDC_W32 (UDC_EPCOMP, UDC_R32 (UDC_EPCOMP));
  FlushAll ();
  UDC_W32 (UDC_DEVADDR, 0);
  DisableDataEndpoints ();
  mConfigured = FALSE;
  mDtr        = FALSE;
  mTxFailures = 0;
  ConfigureEp0 ();
}

//
// ---------------------------------------------------------------------------------------------
// Descriptors
//
STATIC CONST UINT8  mDeviceDesc[18] = {
  18, 1, 0x00, 0x02,             // bcdUSB 2.00
  0x02, 0x00, 0x00,              // CDC, no subclass/protocol at device level
  64,                            // EP0 packet size
  0x09, 0x12, 0x01, 0x00,        // VID 0x1209 / PID 0x0001 (pid.codes test PID)
  0x00, 0x01,                    // bcdDevice
  1, 2, 3,                       // strings: manufacturer, product, serial
  1
};

STATIC CONST UINT8  mConfigDesc[67] = {
  9, 2, 67, 0, 2, 1, 0, 0x80, 50,                       // configuration, 2 interfaces, bus powered 100 mA
  9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,                   // interface 0: CDC communications, ACM, AT commands
  5, 0x24, 0x00, 0x10, 0x01,                            // header functional descriptor
  5, 0x24, 0x01, 0x00, 0x01,                            // call management (data over interface 1)
  4, 0x24, 0x02, 0x02,                                  // ACM: line coding + control line state
  5, 0x24, 0x06, 0x00, 0x01,                            // union: control 0, subordinate 1
  7, 5, 0x82, 0x03, 16, 0, 0xFF,                        // interrupt IN 0x82
  9, 4, 1, 0, 2, 0x0A, 0x00, 0x00, 0,                   // interface 1: CDC data
  7, 5, 0x01, 0x02, MAX_PKT_FS, 0, 0,                   // bulk OUT 0x01
  7, 5, 0x81, 0x02, MAX_PKT_FS, 0, 0                    // bulk IN 0x81
};

STATIC
UINTN
BuildString (
  IN  UINTN        Index,
  OUT UINT8        *Out
  )
{
  CONST CHAR8  *S;
  UINTN        I, N;

  if (Index == 0) {
    Out[0] = 4; Out[1] = 3; Out[2] = 0x09; Out[3] = 0x04;
    return 4;
  }

  S = (Index == 1) ? "Goya" : (Index == 2) ? "goyawifi UEFI serial console" : "0001";
  N = AsciiStrLen (S);
  Out[0] = (UINT8)(2 + 2 * N);
  Out[1] = 3;
  for (I = 0; I < N; I++) {
    Out[2 + 2 * I] = (UINT8)S[I];
    Out[3 + 2 * I] = 0;
  }

  return 2 + 2 * N;
}

//
// ---------------------------------------------------------------------------------------------
// Control endpoint
//
STATIC
VOID
StallEp0 (
  VOID
  )
{
  UDC_W32 (UDC_EPCTRL (0), UDC_R32 (UDC_EPCTRL (0)) | CTRL_RXS | CTRL_TXS);
}

STATIC
VOID
Ep0DataIn (
  IN CONST VOID  *Data,
  IN UINTN       Len,
  IN UINTN       WLength
  )
{
  if (Len > WLength) {
    Len = WLength;
  }

  CopyMem (mArena->Ep0Buf, Data, Len);
  Prime (0, TRUE, mArena->Ep0Buf, Len);
  WaitDone (0, TRUE, 50);
  Prime (0, FALSE, mArena->Ep0Buf, 0);                       // status stage (host sends ZLP OUT)
}

STATIC
VOID
Ep0StatusIn (
  VOID
  )
{
  Prime (0, TRUE, mArena->Ep0Buf, 0);
}

STATIC
VOID
HandleSetup (
  VOID
  )
{
  SETUP_PACKET  S;
  UINTN         T;
  UINT8         Tmp[64];

  // read the 8 setup bytes, with the "setup tripwire" so a new SETUP cannot tear them
  do {
    UDC_W32 (UDC_USBCMD, UDC_R32 (UDC_USBCMD) | CMD_SUTW);
    CopyMem (&S, mArena->Qh[QhIndex (0, FALSE)].Setup, sizeof (S));
  } while ((UDC_R32 (UDC_USBCMD) & CMD_SUTW) == 0);

  UDC_W32 (UDC_USBCMD, UDC_R32 (UDC_USBCMD) & ~CMD_SUTW);
  UDC_W32 (UDC_EPSETUPSTAT, EPT_RX (0));
  UDC_W32 (UDC_EPFLUSH, EPT_RX (0) | EPT_TX (0));
  for (T = 0; T < 1000 && (UDC_R32 (UDC_EPFLUSH) & (EPT_RX (0) | EPT_TX (0))) != 0; T++) {
    MicroSecondDelay (10);
  }

  // un-stall EP0 for the new control transfer
  UDC_W32 (UDC_EPCTRL (0), CTRL_RXE | CTRL_TXE);

  switch ((S.RequestType << 8) | S.Request) {
    case 0x8006:                                       // GET_DESCRIPTOR (device)
      switch (S.Value >> 8) {
        case 1:
          Ep0DataIn (mDeviceDesc, sizeof (mDeviceDesc), S.Length);
          return;
        case 2:
          Ep0DataIn (mConfigDesc, sizeof (mConfigDesc), S.Length);
          return;
        case 3:
          Ep0DataIn (Tmp, BuildString (S.Value & 0xFF, Tmp), S.Length);
          return;
        default:                                       // qualifier / other speed: full speed device, stall
          StallEp0 ();
          return;
      }

    case 0x0005:                                       // SET_ADDRESS
      Ep0StatusIn ();
      WaitDone (0, TRUE, 50);
      UDC_W32 (UDC_DEVADDR, (UINT32)(S.Value & 0x7F) << 25);   // after the status stage: no advance bit
      return;

    case 0x0009:                                       // SET_CONFIGURATION
      if ((S.Value & 0xFF) > 1) {
        StallEp0 ();
        return;
      }

      mConfigValue = (UINT8)S.Value;
      if (mConfigValue != 0) {
        EnableDataEndpoints ();
        mConfigured = TRUE;
      } else {
        DisableDataEndpoints ();
        mConfigured = FALSE;
        mDtr        = FALSE;
      }

      Ep0StatusIn ();
      return;

    case 0x8008:                                       // GET_CONFIGURATION
      Tmp[0] = mConfigValue;
      Ep0DataIn (Tmp, 1, S.Length);
      return;

    case 0x8000:                                       // GET_STATUS (device)
    case 0x8100:                                       // (interface)
    case 0x8200:                                       // (endpoint)
      Tmp[0] = Tmp[1] = 0;
      Ep0DataIn (Tmp, 2, S.Length);
      return;

    case 0x0101:                                       // SET_INTERFACE
    case 0x0001:                                       // CLEAR_FEATURE (device)
    case 0x0201:                                       // CLEAR_FEATURE (endpoint)
    case 0x0003:                                       // SET_FEATURE (device)
      Ep0StatusIn ();
      return;

    case 0x810A:                                       // GET_INTERFACE
      Tmp[0] = 0;
      Ep0DataIn (Tmp, 1, S.Length);
      return;

    case 0x2120:                                       // CDC SET_LINE_CODING (7 bytes OUT)
      Prime (0, FALSE, mArena->Ep0Buf, 7);
      if (WaitDone (0, FALSE, 50)) {
        CopyMem (mLineCoding, mArena->Ep0Buf, sizeof (mLineCoding));
        Ep0StatusIn ();
      } else {
        StallEp0 ();
      }

      return;

    case 0xA121:                                       // CDC GET_LINE_CODING
      Ep0DataIn (mLineCoding, sizeof (mLineCoding), S.Length);
      return;

    case 0x2122:                                       // CDC SET_CONTROL_LINE_STATE (bit0 = DTR)
      mDtr        = (S.Value & 1) != 0;
      mTxFailures = 0;
      Ep0StatusIn ();
      return;

    case 0x2123:                                       // CDC SEND_BREAK
      Ep0StatusIn ();
      return;

    default:
      StallEp0 ();
      return;
  }
}

//
// ---------------------------------------------------------------------------------------------
// Polling
//
STATIC
VOID
RingPush (
  IN CONST UINT8  *Data,
  IN UINTN        Len
  )
{
  while (Len-- > 0) {
    UINTN  Next = (mRingHead + 1) % RING_SIZE;

    if (Next == mRingTail) {
      return;                                          // full: drop
    }

    mRing[mRingHead] = *Data++;
    mRingHead        = Next;
  }
}

STATIC
VOID
UsbPoll (
  VOID
  )
{
  UINT32  Sts = UDC_R32 (UDC_USBSTS);

  if (Sts != 0) {
    UDC_W32 (UDC_USBSTS, Sts);
  }

  if ((Sts & STS_URI) != 0) {
    BusReset ();
  }

  if ((UDC_R32 (UDC_EPSETUPSTAT) & EPT_RX (0)) != 0) {
    HandleSetup ();
  }

  {
    UINT32  Comp = UDC_R32 (UDC_EPCOMP);

    if (Comp != 0) {
      UDC_W32 (UDC_EPCOMP, Comp);
    }
  }

  if (mConfigured && Done (EP_BULK, FALSE)) {
    UINTN  Got = MAX_PKT_FS - Residual (EP_BULK, FALSE);

    if (Got > 0) {
      RingPush (mArena->BulkOut, Got);
    }

    Prime (EP_BULK, FALSE, mArena->BulkOut, MAX_PKT_FS);
  }
}

STATIC
VOID
EFIAPI
OnTimer (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UsbPoll ();
}

//
// ---------------------------------------------------------------------------------------------
// EFI_SERIAL_IO_PROTOCOL
//
STATIC EFI_SERIAL_IO_MODE  mMode = {
  0, 0, 115200, 16, 8, NoParity, OneStopBit
};

STATIC
EFI_STATUS
EFIAPI
SerialReset (
  IN EFI_SERIAL_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SerialSetAttributes (
  IN EFI_SERIAL_IO_PROTOCOL  *This,
  IN UINT64                  BaudRate,
  IN UINT32                  ReceiveFifoDepth,
  IN UINT32                  Timeout,
  IN EFI_PARITY_TYPE         Parity,
  IN UINT8                   DataBits,
  IN EFI_STOP_BITS_TYPE      StopBits
  )
{
  return EFI_SUCCESS;             // a USB CDC link has no real baud rate
}

STATIC
EFI_STATUS
EFIAPI
SerialSetControl (
  IN EFI_SERIAL_IO_PROTOCOL  *This,
  IN UINT32                  Control
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SerialGetControl (
  IN  EFI_SERIAL_IO_PROTOCOL  *This,
  OUT UINT32                  *Control
  )
{
  UINT32  C = 0;
  EFI_TPL Tpl = gBS->RaiseTPL (TPL_NOTIFY);

  UsbPoll ();
  if (mConfigured && mDtr) {
    C |= EFI_SERIAL_DATA_SET_READY | EFI_SERIAL_CLEAR_TO_SEND | EFI_SERIAL_CARRIER_DETECT;
  }

  if (mRingHead == mRingTail) {
    C |= EFI_SERIAL_INPUT_BUFFER_EMPTY;
  }

  gBS->RestoreTPL (Tpl);
  *Control = C;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SerialWrite (
  IN     EFI_SERIAL_IO_PROTOCOL  *This,
  IN OUT UINTN                   *BufferSize,
  IN     VOID                    *Buffer
  )
{
  CONST UINT8  *P   = Buffer;
  UINTN        Left = *BufferSize;
  EFI_TPL      Tpl;

  Tpl = gBS->RaiseTPL (TPL_NOTIFY);
  UsbPoll ();

  // Nobody listening (no host, port not open, or a host that stopped reading): swallow the data so the
  // console never blocks the firmware.
  while (Left > 0 && mConfigured && mDtr && mTxFailures < 3) {
    UINTN  N = (Left > MAX_PKT_FS * 4) ? MAX_PKT_FS * 4 : Left;

    CopyMem (mArena->BulkIn, P, N);
    Prime (EP_BULK, TRUE, mArena->BulkIn, N);
    if (!WaitDone (EP_BULK, TRUE, 20)) {
      mTxFailures++;
      UDC_W32 (UDC_EPFLUSH, EPT_TX (EP_BULK));
      break;
    }

    mTxFailures = 0;
    P          += N;
    Left       -= N;
    UsbPoll ();
  }

  gBS->RestoreTPL (Tpl);
  return EFI_SUCCESS;             // all bytes "written", dropped ones included
}

STATIC
EFI_STATUS
EFIAPI
SerialRead (
  IN     EFI_SERIAL_IO_PROTOCOL  *This,
  IN OUT UINTN                   *BufferSize,
  OUT    VOID                    *Buffer
  )
{
  UINT8   *P = Buffer;
  UINTN   N  = 0;
  EFI_TPL Tpl;

  Tpl = gBS->RaiseTPL (TPL_NOTIFY);
  UsbPoll ();
  while (N < *BufferSize && mRingTail != mRingHead) {
    P[N++]    = mRing[mRingTail];
    mRingTail = (mRingTail + 1) % RING_SIZE;
  }

  gBS->RestoreTPL (Tpl);
  *BufferSize = N;
  return (N == 0) ? EFI_TIMEOUT : EFI_SUCCESS;
}

STATIC EFI_SERIAL_IO_PROTOCOL  mSerial = {
  SERIAL_IO_INTERFACE_REVISION,
  SerialReset,
  SerialSetAttributes,
  SerialSetControl,
  SerialGetControl,
  SerialWrite,
  SerialRead,
  &mMode
};

#pragma pack (1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  UART_DEVICE_PATH            Uart;
  EFI_DEVICE_PATH_PROTOCOL    End;
} USB_SERIAL_DEVICE_PATH;
#pragma pack ()

STATIC USB_SERIAL_DEVICE_PATH  mSerialPath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    PXA988_USB_SERIAL_GUID
  },
  {
    { MESSAGING_DEVICE_PATH, MSG_UART_DP, { sizeof (UART_DEVICE_PATH), 0 } },
    0, 115200, 8, 1, 1
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

//
// ---------------------------------------------------------------------------------------------
//
EFI_STATUS
UsbGadgetStart (
  OUT CHAR8  *Report,
  IN  UINTN  ReportSize
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Mem;
  UINTN                 I;
  EFI_HANDLE            Handle = NULL;

  Status = gBS->AllocatePages (AllocateAnyPages, EfiBootServicesData, EFI_SIZE_TO_PAGES (sizeof (ARENA)), &Mem);
  if (EFI_ERROR (Status)) {
    AsciiSPrint (Report, ReportSize, "gadget: no memory for the arena (%r)\n", Status);
    return Status;
  }

  Status = gDS->SetMemorySpaceAttributes (Mem, EFI_PAGES_TO_SIZE (EFI_SIZE_TO_PAGES (sizeof (ARENA))), EFI_MEMORY_UC);
  if (EFI_ERROR (Status)) {
    AsciiSPrint (Report, ReportSize, "gadget: cannot map the arena uncached (%r)\n", Status);
    gBS->FreePages (Mem, EFI_SIZE_TO_PAGES (sizeof (ARENA)));
    return Status;
  }

  mArena = (ARENA *)(UINTN)Mem;
  ZeroMem (mArena, sizeof (ARENA));

  // ci_pullup(on): reset, list address, device mode, force full speed, flush, run
  UDC_W32 (UDC_USBCMD, CMD_ITC8 | CMD_RST);
  for (I = 0; I < 100 && (UDC_R32 (UDC_USBCMD) & CMD_RST) != 0; I++) {
    MicroSecondDelay (1000);
  }

  UDC_W32 (UDC_EPLIST, (UINT32)(UINTN)mArena->Qh);
  UDC_W32 (UDC_USBMODE, USBMODE_DEVICE);
  UDC_W32 (UDC_PORTSC, UDC_R32 (UDC_PORTSC) | PORTSC_PFSC);
  ConfigureEp0 ();
  FlushAll ();
  UDC_W32 (UDC_USBCMD, UDC_R32 (UDC_USBCMD) | CMD_ITC8 | CMD_RUN);        // D+ pull-up on: the host sees us now

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnTimer, NULL, &mTimer);
  if (!EFI_ERROR (Status)) {
    Status = gBS->SetTimer (mTimer, TimerPeriodic, 10000);                 // as fast as the tick allows
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mSerialPath,
                  &gEfiSerialIoProtocolGuid,
                  &mSerial,
                  NULL
                  );
  AsciiSPrint (
    Report,
    ReportSize,
    "gadget: CDC-ACM serial up (USBMODE=0x%x PORTSC=0x%08x), serial handle: %r\n",
    UDC_R32 (UDC_USBMODE),
    UDC_R32 (UDC_PORTSC),
    Status
    );
  return Status;
}
