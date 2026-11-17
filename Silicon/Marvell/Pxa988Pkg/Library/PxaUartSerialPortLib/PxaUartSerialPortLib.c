/** @file
  SerialPortLib for the Marvell PXA/MMP UART.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Base.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/SerialPortLib.h>
#include <Pxa988.h>
#include "FbFont8x19.h"

#define UART_BASE   FixedPcdGet32 (PcdDebugUartBase)
#define UART_REG(r) (UART_BASE + ((r) << 2))

#define RBR  0
#define THR  0
#define DLL  0
#define IER  1
#define DLH  1
#define FCR  2
#define LCR  3
#define MCR  4
#define LSR  5
#define MSR  6

#define IER_UUE   BIT6        // UART unit enable (PXA specific)
#define FCR_FIFO  (BIT0 | BIT1 | BIT2)
#define LCR_DLAB  BIT7
#define LCR_8N1   0x03
#define LSR_DR    BIT0
#define LSR_THRE  BIT5
#define LSR_TEMT  BIT6
#define MCR_DTR   BIT0
#define MCR_RTS   BIT1

STATIC BOOLEAN  mUartInitialised;

//
// Framebuffer log. The cursor is kept in the page right after the
// framebuffer (reserved by the platform), so SEC, DXE core and every DXE
// driver share one position even though each links its own copy of this
// library. SEC zeroes the magic to start a fresh screen.
//
#define FB_BASE     FixedPcdGet32 (PcdFrameBufferBase)
#define FB_WIDTH    FixedPcdGet32 (PcdFrameBufferWidth)
#define FB_HEIGHT   FixedPcdGet32 (PcdFrameBufferHeight)
#define FB_STRIDE   FixedPcdGet32 (PcdFrameBufferStride)
#define FB_SCALE    FixedPcdGet32 (PcdFbLogScale)
#define FB_CELL_W   (FB_FONT_W * FB_SCALE)
#define FB_CELL_H   (FB_FONT_H * FB_SCALE)
#define FB_COLS     (FB_WIDTH / FB_CELL_W)
#define FB_ROWS     (FB_HEIGHT / FB_CELL_H)
#define FB_LOG_MAGIC  0x464C4F47    // "FLOG"
#define FB_FG       0xFFFFFFFF
#define FB_BG       0xFF000000

typedef struct {
  UINT32    Magic;
  UINT32    X;
  UINT32    Y;
} FB_LOG_STATE;

#define FB_STATE  ((volatile FB_LOG_STATE *)(UINTN)(FB_BASE + ALIGN_VALUE (FB_STRIDE * 4 * FB_HEIGHT, 0x1000)))

STATIC
VOID
FbFillRow (
  IN UINT32  Row
  )
{
  volatile UINT32  *Line;
  UINT32           Y;
  UINT32           X;

  for (Y = Row * FB_CELL_H; Y < (Row + 1) * FB_CELL_H; Y++) {
    Line = (volatile UINT32 *)(UINTN)(FB_BASE + Y * FB_STRIDE * 4);
    for (X = 0; X < FB_WIDTH; X++) {
      Line[X] = FB_BG;
    }
  }
}

STATIC
VOID
FbDrawChar (
  IN UINT8   Ch,
  IN UINT32  Col,
  IN UINT32  Row
  )
{
  CONST UINT8      *Glyph;
  volatile UINT32  *Line;
  UINT32           Gy;
  UINT32           Gx;
  UINT32           Sy;
  UINT32           Sx;
  UINT32           Color;

  if ((Ch < FB_FONT_FIRST) || (Ch > FB_FONT_LAST)) {
    Ch = '?';
  }

  Glyph = mFbFont[Ch - FB_FONT_FIRST];
  for (Gy = 0; Gy < FB_FONT_H; Gy++) {
    for (Sy = 0; Sy < FB_SCALE; Sy++) {
      Line = (volatile UINT32 *)(UINTN)(FB_BASE +
                                        ((Row * FB_CELL_H) + (Gy * FB_SCALE) + Sy) * FB_STRIDE * 4) +
             Col * FB_CELL_W;
      for (Gx = 0; Gx < FB_FONT_W; Gx++) {
        Color = ((Glyph[Gy] >> (7 - Gx)) & 1) ? FB_FG : FB_BG;
        for (Sx = 0; Sx < FB_SCALE; Sx++) {
          Line[(Gx * FB_SCALE) + Sx] = Color;
        }
      }
    }
  }
}

STATIC
VOID
FbNewLine (
  VOID
  )
{
  FB_STATE->X = 0;
  FB_STATE->Y++;
  if (FB_STATE->Y >= FB_ROWS) {
    FB_STATE->Y = 0;          // wrap around instead of slow scrolling
  }

  FbFillRow (FB_STATE->Y);    // blank the line we are about to use
  if (FB_STATE->Y + 1 < FB_ROWS) {
    FbFillRow (FB_STATE->Y + 1);  // and the next one, to mark the head
  }
}

STATIC
VOID
FbLogInit (
  VOID
  )
{
  volatile UINT32  *Fb;
  UINTN            Index;

  if (FB_STATE->Magic == FB_LOG_MAGIC) {
    return;
  }

  // Clear the whole splash, including the strip below the last text row
  Fb = (volatile UINT32 *)(UINTN)FB_BASE;
  for (Index = 0; Index < FB_STRIDE * FB_HEIGHT; Index++) {
    Fb[Index] = FB_BG;
  }

  FB_STATE->X     = 0;
  FB_STATE->Y     = 0;
  FB_STATE->Magic = FB_LOG_MAGIC;
}

STATIC
VOID
FbLogWrite (
  IN UINT8  *Buffer,
  IN UINTN  NumberOfBytes
  )
{
  UINTN  Index;

  FbLogInit ();

  for (Index = 0; Index < NumberOfBytes; Index++) {
    switch (Buffer[Index]) {
      case '\n':
        FbNewLine ();
        break;
      case '\r':
        FB_STATE->X = 0;
        break;
      default:
        if (FB_STATE->X >= FB_COLS) {
          FbNewLine ();
        }

        FbDrawChar (Buffer[Index], FB_STATE->X, FB_STATE->Y);
        FB_STATE->X++;
        break;
    }
  }
}

RETURN_STATUS
EFIAPI
SerialPortInitialize (
  VOID
  )
{
  UINT32  Divisor;

  if (mUartInitialised) {
    return RETURN_SUCCESS;
  }

  //
  // uart_pll = pll1_4 * 1536 / (8125 * 2) = 14.7456 MHz, FNCLKSEL 1.
  // Only the debug UART (UART1 @ 0xD4017000) has its clock set up here.
  //
  if (UART_BASE == PXA988_UART1_BASE) {
    MmioWrite32 (MPMU_UART_PLL, (8125 << 16) | 1536);
    MmioWrite32 (APBC_UART1, APBC_APBCLK | APBC_FNCLK | APBC_FNCLKSEL (1));
  }

  Divisor = PXA988_UART_CLOCK / (16 * FixedPcdGet32 (PcdDebugUartBaudRate));

  MmioWrite32 (UART_REG (IER), 0);
  MmioWrite32 (UART_REG (LCR), LCR_DLAB | LCR_8N1);
  MmioWrite32 (UART_REG (DLL), Divisor & 0xFF);
  MmioWrite32 (UART_REG (DLH), (Divisor >> 8) & 0xFF);
  MmioWrite32 (UART_REG (LCR), LCR_8N1);
  MmioWrite32 (UART_REG (FCR), FCR_FIFO);
  MmioWrite32 (UART_REG (MCR), MCR_DTR | MCR_RTS);
  MmioWrite32 (UART_REG (IER), IER_UUE);

  mUartInitialised = TRUE;
  return RETURN_SUCCESS;
}

UINTN
EFIAPI
SerialPortWrite (
  IN UINT8  *Buffer,
  IN UINTN  NumberOfBytes
  )
{
  UINTN  Count;

  if (Buffer == NULL) {
    return 0;
  }

  if (FixedPcdGetBool (PcdFbLogEnable)) {
    FbLogWrite (Buffer, NumberOfBytes);
  }

  for (Count = 0; Count < NumberOfBytes; Count++) {
    UINTN  Spin;

    // Bounded wait: never hang the firmware on a stuck/unclocked UART
    for (Spin = 0; Spin < 100000; Spin++) {
      if ((MmioRead32 (UART_REG (LSR)) & LSR_THRE) != 0) {
        break;
      }
    }

    MmioWrite32 (UART_REG (THR), Buffer[Count]);
  }

  return NumberOfBytes;
}

UINTN
EFIAPI
SerialPortRead (
  OUT UINT8  *Buffer,
  IN  UINTN  NumberOfBytes
  )
{
  UINTN  Count;

  if (Buffer == NULL) {
    return 0;
  }

  for (Count = 0; Count < NumberOfBytes; Count++) {
    while ((MmioRead32 (UART_REG (LSR)) & LSR_DR) == 0) {
    }

    Buffer[Count] = (UINT8)MmioRead32 (UART_REG (RBR));
  }

  return NumberOfBytes;
}

BOOLEAN
EFIAPI
SerialPortPoll (
  VOID
  )
{
  return (MmioRead32 (UART_REG (LSR)) & LSR_DR) != 0;
}

RETURN_STATUS
EFIAPI
SerialPortSetControl (
  IN UINT32  Control
  )
{
  return RETURN_UNSUPPORTED;
}

RETURN_STATUS
EFIAPI
SerialPortGetControl (
  OUT UINT32  *Control
  )
{
  *Control = 0;
  if (!SerialPortPoll ()) {
    *Control |= EFI_SERIAL_INPUT_BUFFER_EMPTY;
  }

  if ((MmioRead32 (UART_REG (LSR)) & LSR_TEMT) != 0) {
    *Control |= EFI_SERIAL_OUTPUT_BUFFER_EMPTY;
  }

  return RETURN_SUCCESS;
}

RETURN_STATUS
EFIAPI
SerialPortSetAttributes (
  IN OUT UINT64              *BaudRate,
  IN OUT UINT32              *ReceiveFifoDepth,
  IN OUT UINT32              *Timeout,
  IN OUT EFI_PARITY_TYPE     *Parity,
  IN OUT UINT8               *DataBits,
  IN OUT EFI_STOP_BITS_TYPE  *StopBits
  )
{
  // Fixed at 115200 8N1
  return RETURN_SUCCESS;
}
