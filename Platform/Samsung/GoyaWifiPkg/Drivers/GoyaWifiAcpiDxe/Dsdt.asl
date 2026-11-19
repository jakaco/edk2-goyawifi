/* Minimal DSDT for Galaxy Tab 3 Lite (PXA986/988) - Windows RT bring-up */
DefinitionBlock ("Dsdt.aml", "DSDT", 5, "SAMSNG", "GOYAWIFI", 0x00000001)
{
  Scope (\_SB)
  {
    Device (CPU0) {
      Name (_HID, "ACPI0007")
      Name (_UID, 0)
    }
    Device (CPU1) {
      Name (_HID, "ACPI0007")
      Name (_UID, 1)
    }
    // UART1 (16550-compatible PXA UART), SPI 27 = GIC IRQ 59
    Device (UAR1) {
      Name (_HID, "MRVL0001")
      Name (_UID, 1)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0xD4017000, 0x1000)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 59 }
      })
    }
  }
}
