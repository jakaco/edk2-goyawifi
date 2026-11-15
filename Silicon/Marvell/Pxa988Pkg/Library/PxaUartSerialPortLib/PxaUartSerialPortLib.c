/** @file
  SerialPortLib for the Marvell PXA/MMP UART.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Base.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/SerialPortLib.h>
#include <Pxa988.h>

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

  for (Count = 0; Count < NumberOfBytes; Count++) {
    while ((MmioRead32 (UART_REG (LSR)) & LSR_THRE) == 0) {
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
