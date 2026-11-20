/* Licensed under the MIT License.
   Derived from Microsoft imx-iotcore hals/halext/HalExtiMX6Timers.

Module Name:

    HalExtension.c

Abstract:

    HAL Extension Module for the Marvell PXA986/PXA988 APB timer 0.
    Registers two timers described by CSRT descriptors:
      - MMP_TIMER_TYPE_EVENT   (counter 0, GIC IRQ from the descriptor): the
        interrupt timer Windows uses for the clock tick.
      - MMP_TIMER_TYPE_COUNTER (counter 1): a free-running readable counter
        used as the performance counter.
    Cortex-A9 has no Generic Timer, so this module is what gives the Windows
    HAL its timers.

*/

#include <nthalext.h>
#include "HalExtCompat.h"
#include "MmpTimer.h"

//
// CSRT timer descriptor (must match GoyaWifiAcpiDxe.c)
//
#pragma pack(push, 1)
typedef struct _CSRT_RESOURCE_DESCRIPTOR_TIMER_INSTANCE {
    CSRT_RESOURCE_DESCRIPTOR_HEADER Header;
    UINT32 TimerType;
    UINT32 ClockSource;
    UINT32 Frequency;
    UINT32 FrequencyScale;
    UINT32 BaseAddress;
    UINT32 Interrupt;
} CSRT_RESOURCE_DESCRIPTOR_TIMER_INSTANCE, *PCSRT_RESOURCE_DESCRIPTOR_TIMER_INSTANCE;
#pragma pack(pop)

typedef struct _MMP_TIMER_INTERNAL_DATA {
    PULONG BaseAddressPtr;      // mapped timer block
    ULONG  PeriodInTicks;       // non-zero: periodic emulation
} MMP_TIMER_INTERNAL_DATA, *PMMP_TIMER_INTERNAL_DATA;

static FORCEINLINE ULONG ReadReg(PVOID Base, ULONG Reg)
{
    return READ_REGISTER_NOFENCE_ULONG((ULONG *)((PUCHAR)Base + Reg));
}

static FORCEINLINE VOID WriteReg(PVOID Base, ULONG Reg, ULONG Value)
{
    WRITE_REGISTER_NOFENCE_ULONG((ULONG *)((PUCHAR)Base + Reg), Value);
}

//
// ---- Counter 1: free-running readable counter -------------------------------
//

static NTSTATUS HalpMmpCounterInitialize(__in PVOID TimerDataPtr)
{
    PMMP_TIMER_INTERNAL_DATA T = (PMMP_TIMER_INTERNAL_DATA)TimerDataPtr;
    PULONG Base = T->BaseAddressPtr;
    ULONG Cer = ReadReg(Base, MMP_TMR_CER);

    //
    // Same sequence as the firmware's MmpTimerLib and the vendor kernel
    // (timer_config): fast clock, free running, no interrupt, enabled.
    // The functional/APB clock is left as the firmware configured it.
    //
    WriteReg(Base, MMP_TMR_CCR, ReadReg(Base, MMP_TMR_CCR) & ~(3UL << 2));
    WriteReg(Base, MMP_TMR_CMR, ReadReg(Base, MMP_TMR_CMR) | (1UL << MMP_FREE_COUNTER));
    WriteReg(Base, MMP_TMR_PLCR(MMP_FREE_COUNTER), 0);
    WriteReg(Base, MMP_TMR_ICR(MMP_FREE_COUNTER), 0x7);
    WriteReg(Base, MMP_TMR_IER(MMP_FREE_COUNTER), 0);
    WriteReg(Base, MMP_TMR_CER, Cer | (1UL << MMP_FREE_COUNTER));
    return STATUS_SUCCESS;
}

static ULONGLONG HalpMmpCounterQuery(__in PVOID TimerDataPtr)
{
    PMMP_TIMER_INTERNAL_DATA T = (PMMP_TIMER_INTERNAL_DATA)TimerDataPtr;
    ULONG Value = 0;
    ULONG I;

    //
    // The counter cannot be read directly: request a latch, then read the
    // latched value back after a few bus cycles. No stall function may be
    // used here (the HAL's stall depends on this counter).
    //
    WriteReg(T->BaseAddressPtr, MMP_TMR_CVWR(MMP_FREE_COUNTER), 1);
    for (I = 0; I < MMP_COUNTER_READ_SPINS; I++) {
        Value = ReadReg(T->BaseAddressPtr, MMP_TMR_CVWR(MMP_FREE_COUNTER));
    }

    return Value;
}

//
// ---- Counter 0: match interrupt timer ---------------------------------------
//

static NTSTATUS HalpMmpEventInitialize(__in PVOID TimerDataPtr)
{
    PMMP_TIMER_INTERNAL_DATA T = (PMMP_TIMER_INTERNAL_DATA)TimerDataPtr;
    PULONG Base = T->BaseAddressPtr;

    WriteReg(Base, MMP_TMR_CER, ReadReg(Base, MMP_TMR_CER) & ~(1UL << MMP_EVENT_COUNTER));
    WriteReg(Base, MMP_TMR_CCR, ReadReg(Base, MMP_TMR_CCR) & ~(3UL << 0));   // counter 0: fast clock
    WriteReg(Base, MMP_TMR_CMR, ReadReg(Base, MMP_TMR_CMR) | (1UL << MMP_EVENT_COUNTER)); // not used for counter 0 by the vendor kernel; harmless
    WriteReg(Base, MMP_TMR_PLCR(MMP_EVENT_COUNTER), 1);          // periodic reload from PLVR(0)=0, as the vendor kernel
    WriteReg(Base, MMP_TMR_ICR(MMP_EVENT_COUNTER), 0x7);
    WriteReg(Base, MMP_TMR_IER(MMP_EVENT_COUNTER), 0);
    return STATUS_SUCCESS;
}

static VOID HalpMmpEventStopCounter(PULONG Base)
{
    // 0x02 = only counter 1 stays enabled (vendor kernel writes the same)
    WriteReg(Base, MMP_TMR_CER, ReadReg(Base, MMP_TMR_CER) & ~(1UL << MMP_EVENT_COUNTER));
}

static VOID HalpMmpEventProgram(PULONG Base, ULONG Ticks)
{
    HalpMmpEventStopCounter(Base);
    WriteReg(Base, MMP_TMR_ICR(MMP_EVENT_COUNTER), 0x1);
    WriteReg(Base, MMP_TMR_IER(MMP_EVENT_COUNTER), 0x1);
    WriteReg(Base, MMP_TMR_TN_MM(MMP_EVENT_COUNTER, 0), Ticks - 1);
    WriteReg(Base, MMP_TMR_CER, ReadReg(Base, MMP_TMR_CER) | (1UL << MMP_EVENT_COUNTER));
}

static NTSTATUS HalpMmpEventArm(__in PVOID TimerDataPtr, __in TIMER_MODE Mode, __in ULONGLONG TickCount)
{
    PMMP_TIMER_INTERNAL_DATA T = (PMMP_TIMER_INTERNAL_DATA)TimerDataPtr;

    if (TickCount == 0 || TickCount >= MMP_RESET_MAX_TICKS) {
        return STATUS_INVALID_PARAMETER;
    }

    switch (Mode) {
    case TimerModePeriodic:
        T->PeriodInTicks = (ULONG)TickCount;   // re-armed in AcknowledgeInterrupt
        break;
    case TimerModeOneShot:
        T->PeriodInTicks = 0;
        break;
    default:
        return STATUS_INVALID_PARAMETER;
    }

    HalpMmpEventProgram(T->BaseAddressPtr, (ULONG)TickCount);
    return STATUS_SUCCESS;
}

static VOID HalpMmpEventAcknowledge(__in PVOID TimerDataPtr)
{
    PMMP_TIMER_INTERNAL_DATA T = (PMMP_TIMER_INTERNAL_DATA)TimerDataPtr;

    WriteReg(T->BaseAddressPtr, MMP_TMR_ICR(MMP_EVENT_COUNTER), 0x1);
    HalpMmpEventStopCounter(T->BaseAddressPtr);        // as the vendor kernel: stop after each match

    if (T->PeriodInTicks != 0) {
        HalpMmpEventProgram(T->BaseAddressPtr, T->PeriodInTicks);
    }
}

static VOID HalpMmpEventStop(__in PVOID TimerDataPtr)
{
    PMMP_TIMER_INTERNAL_DATA T = (PMMP_TIMER_INTERNAL_DATA)TimerDataPtr;

    WriteReg(T->BaseAddressPtr, MMP_TMR_IER(MMP_EVENT_COUNTER), 0);
    HalpMmpEventStopCounter(T->BaseAddressPtr);
    WriteReg(T->BaseAddressPtr, MMP_TMR_ICR(MMP_EVENT_COUNTER), 0x1);
    T->PeriodInTicks = 0;
}

//
// ---- Registration -----------------------------------------------------------
//

static NTSTATUS
HalpMmpTimerRegister(
    ULONG Handle,
    PCSRT_RESOURCE_GROUP_HEADER ResourceGroup,
    PCSRT_RESOURCE_DESCRIPTOR_TIMER_INSTANCE Csrt,
    BOOLEAN IsEvent
    )
{
    NTSTATUS Status;
    PHYSICAL_ADDRESS PhysicalAddress;
    MMP_TIMER_INTERNAL_DATA InternalData;
    TIMER_INITIALIZATION_BLOCK NewTimer;

    RtlZeroMemory(&InternalData, sizeof(InternalData));

    PhysicalAddress.QuadPart = (LONGLONG)Csrt->BaseAddress;

    // Both descriptors share the block; the HAL tolerates the repeat registration
    // on the first call only, so ignore "already registered" on the second.
    Status = HalRegisterPermanentAddressUsage(PhysicalAddress, 0x1000);
    if (!NT_SUCCESS(Status) && Status != STATUS_CONFLICTING_ADDRESSES) {
        return Status;
    }

    InternalData.BaseAddressPtr = HalMapIoSpace(PhysicalAddress, 0x1000, MmNonCached);
    if (InternalData.BaseAddressPtr == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(&NewTimer, sizeof(NewTimer));
    INITIALIZE_TIMER_HEADER(&NewTimer);

    NewTimer.InternalData = &InternalData;
    NewTimer.InternalDataSize = sizeof(InternalData);
    NewTimer.CounterBitWidth = 32;
    NewTimer.CounterFrequency = Csrt->Frequency / Csrt->FrequencyScale;
    NewTimer.MaxDivisor = 1;
    NewTimer.KnownType = TimerUnknown;
    NewTimer.Identifier = Csrt->Header.Uid;

    if (IsEvent) {
        NewTimer.FunctionTable.Initialize = HalpMmpEventInitialize;
        NewTimer.FunctionTable.AcknowledgeInterrupt = HalpMmpEventAcknowledge;
        NewTimer.FunctionTable.ArmTimer = HalpMmpEventArm;
        NewTimer.FunctionTable.Stop = HalpMmpEventStop;
        //
        // Only flags the Windows 8 (build 8520) HAL is known to accept: Microsoft's own 8520 timer
        // extensions (HalExtSP804) register the same TIMER_INITIALIZATION_BLOCK version/size (1/0x68)
        // and never set TIMER_ALWAYS_ON (0x8000, Windows 10 header).
        //
        NewTimer.Capabilities = TIMER_PERIODIC_CAPABLE |
                                TIMER_ONE_SHOT_CAPABLE |
                                TIMER_GENERATES_LINE_BASED_INTERRUPTS;
        NewTimer.Interrupt.Mode = LevelSensitive;
        NewTimer.Interrupt.Polarity = InterruptActiveHigh;
        NewTimer.Interrupt.Gsi = Csrt->Interrupt;
    } else {
        NewTimer.FunctionTable.Initialize = HalpMmpCounterInitialize;
        NewTimer.FunctionTable.QueryCounter = HalpMmpCounterQuery;
        NewTimer.Capabilities = TIMER_COUNTER_READABLE;
    }

    return RegisterResourceDescriptor(Handle, ResourceGroup, &Csrt->Header, &NewTimer);
}

NTSTATUS
AddResourceGroup (
    __in ULONG Handle,
    __in PCSRT_RESOURCE_GROUP_HEADER ResourceGroup
    )
{
    PCSRT_RESOURCE_DESCRIPTOR_HEADER Header = NULL;
    PCSRT_RESOURCE_DESCRIPTOR_TIMER_INSTANCE Timer;
    NTSTATUS Status;

    for (;;) {
        Header = GetNextResourceDescriptor(Handle, ResourceGroup, Header,
                                           CSRT_RD_TYPE_TIMER,
                                           CSRT_RD_SUBTYPE_TIMER,
                                           CSRT_RD_UID_ANY);
        if (Header == NULL) {
            break;
        }

        if (Header->Length != sizeof(CSRT_RESOURCE_DESCRIPTOR_TIMER_INSTANCE)) {
            return STATUS_INVALID_PARAMETER;
        }

        Timer = (PCSRT_RESOURCE_DESCRIPTOR_TIMER_INSTANCE)Header;
        switch (Timer->TimerType) {
        case MMP_TIMER_TYPE_EVENT:
            Status = HalpMmpTimerRegister(Handle, ResourceGroup, Timer, TRUE);
            break;
        case MMP_TIMER_TYPE_COUNTER:
            Status = HalpMmpTimerRegister(Handle, ResourceGroup, Timer, FALSE);
            break;
        default:
            return STATUS_INVALID_PARAMETER;
        }

        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    return STATUS_SUCCESS;
}
