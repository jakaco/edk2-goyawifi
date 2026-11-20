#ifndef USB_GADGET_H_
#define USB_GADGET_H_

#include <Uefi.h>

/**
  Start the ChipIdea controller as a USB device (full speed, forced like U-Boot's ci_udc does) with one
  CDC-ACM function and publish it as an EFI_SERIAL_IO_PROTOCOL handle.

  @param[out] Report  short text for the UsbStatus variable
**/
EFI_STATUS
UsbGadgetStart (
  OUT CHAR8  *Report,
  IN  UINTN  ReportSize
  );

#endif
