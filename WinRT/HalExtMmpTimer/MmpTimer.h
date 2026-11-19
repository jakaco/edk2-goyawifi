/*
Module Name:  MmpTimer.h
Abstract:     Marvell MMP/PXA988 APB timer block (Timer0 @ 0xD4014000) definitions.
              Register layout from arch/arm/mach-mmp/regs-timers.h and time.c of the
              Samsung SM-T110 (goyawifi) kernels.
License:      MIT (derived from Microsoft imx-iotcore HalExtiMX6Timers)
*/
#ifndef _MMPTIMER_H
#define _MMPTIMER_H

// Register offsets from the timer block base
#define MMP_TMR_CCR          0x00            // counter clock control
#define MMP_TMR_TN_MM(n, m)  (0x04 + ((n) << 3) + (((n) + (m)) << 2))   // match register
#define MMP_TMR_SR(n)        (0x34 + ((n) << 2))
#define MMP_TMR_IER(n)       (0x40 + ((n) << 2))
#define MMP_TMR_PLCR(n)      (0x58 + ((n) << 2))   // preload control (0 = free running)
#define MMP_TMR_ICR(n)       (0x74 + ((n) << 2))   // interrupt clear
#define MMP_TMR_CER          0x84            // counter enable
#define MMP_TMR_CMR          0x88            // counter mode
#define MMP_TMR_CVWR(n)      (0xA4 + ((n) << 2))   // write 1 to latch, read back value

#define MMP_EVENT_COUNTER    0               // counter 0: match interrupt (clock event)
#define MMP_FREE_COUNTER     1               // counter 1: free running (performance counter)

//
// CSRT timer descriptor TimerType values used by the firmware
// (Platform/Samsung/GoyaWifiPkg/Drivers/GoyaWifiAcpiDxe)
//
#define MMP_TIMER_TYPE_EVENT    1            // counter 0, generates the interrupt
#define MMP_TIMER_TYPE_COUNTER  2            // counter 1, readable free-running counter

#define MMP_COUNTER_READ_SPINS  3            // CVWR needs a few bus cycles to latch
#define MMP_RESET_MAX_TICKS     0xFFFFFFFFULL

#endif
