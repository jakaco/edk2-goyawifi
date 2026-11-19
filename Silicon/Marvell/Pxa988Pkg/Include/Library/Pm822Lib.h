/** @file
  Polled access to the Marvell 88PM822 PMIC on PXA988 TWSI2.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef PM822_LIB_H_
#define PM822_LIB_H_

#include <Uefi.h>

/**
  Read one register. Page is the 7-bit I2C address: PM822_I2C_ADDR (base
  page) or PM822_POWER_I2C_ADDR (regulators).
**/
EFI_STATUS
EFIAPI
Pm822Read (
  IN  UINT8  Page,
  IN  UINT8  Reg,
  OUT UINT8  *Val
  );

EFI_STATUS
EFIAPI
Pm822Write (
  IN UINT8  Page,
  IN UINT8  Reg,
  IN UINT8  Val
  );

/**
  A runtime driver calls this from its virtual address change notification
  with the converted bases of the TWSI2 (PXA988_TWSI2_BASE) and APBCP
  (PXA988_APBCP_BASE) register blocks.
**/
VOID
EFIAPI
Pm822SetRuntimeBases (
  IN UINTN  Twsi2Base,
  IN UINTN  ApbcpBase
  );

#endif // PM822_LIB_H_
