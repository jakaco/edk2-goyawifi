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
    // Marvell MMP timer 0 HAL extension is matched by CSRT; this device lets the
    // INF install it (ACPI\MRVL0002)
    Device (TMR0) {
      Name (_HID, "MRVL0002")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0xD4014000, 0x1000)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 45 }
      })
    }
    // GPIO controller: 4 banks x 32 pins, one muxed interrupt (SPI 49 = GIC 81). GpioClx wants one
    // interrupt resource per bank, so the shared line is listed once per bank.
    Device (GPI0) {
      Name (_HID, "MRVL0003")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0xD4019000, 0x1000)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 81 }
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 81 }
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 81 }
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 81 }
      })
    }
    // TWSI1 (I2C bus 1): touchscreen (Zinitix BT532 @0x20) and display bridge. SpbCx controller driver
    // goyai2c; polled, so no interrupt. Second memory range = APBC_TWSI1 clock/reset register.
    // TWSI2 (PMIC) is deliberately not exposed: the firmware's runtime services use it.
    Device (I2C1) {
      Name (_HID, "MRVL0004")
      Name (_UID, 1)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0xD4010800, 0x1000)
        Memory32Fixed (ReadWrite, 0xD4015060, 0x4)
      })
    }
    // microSD host (SDH1). PNP0D40 = "SDA standard compliant SD host controller": the inbox
    // sdport/sdbus stack binds to it with no custom driver. Marvell quirks are untested.
    // The firmware has already powered the slot (88PM822 LDO4/LDO5) and enabled the clocks.
    Device (SDH1) {
      Name (_HID, "PNP0D40")
      Name (_UID, 1)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0xD4280000, 0x1000)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 71 }   // SPI 39 + 32
      })
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
