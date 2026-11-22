/*
 * gc1000_hw.c - Vivante GC1000 hardware layer (Marvell PXA986, Windows 8.0 RT ARM32).
 *
 * Ported from the goyawifi Linux 4.19 tree:
 *   power island      arch/arm/mach-mmp/pxa988-pm-domain.c  (pxa988_pd_power_on/off, GC desc)
 *   clocks            drivers/clk/mmp/clk-of-pxa988.c       (gc_mix, gc_axi_mix, gates; mix type V1)
 *   reset / identify  drivers/gpu/drm/etnaviv/etnaviv_gpu.c  (etnaviv_hw_reset, _hw_identify,
 *                     _hw_specs, _enable_mlcg, _setup_pulse_eater, _hw_init, _start_fe)
 *   ring / submit     drivers/gpu/drm/etnaviv/etnaviv_buffer.c (etnaviv_buffer_init/_queue)
 * The goya board runs the GC with FSCALE 64 (no internal frequency scaling; the APMU clock is
 * changed instead), as etnaviv's goya_scaling() does.
 *
 * No floating point, no NEON, no 64-bit or variable divisions (the Cortex-A9 has no SDIV/UDIV).
 */
#include <ntddk.h>
#include "gc1000_hw.h"

#if defined(_M_ARM)
#define GC_DSB()    __dsb(_ARM_BARRIER_SY)
#else
#define GC_DSB()    KeMemoryBarrier()
#endif

#define GC_RING_SIZE    (32UL * 1024)

/* clock setup (Linux assigned-clock-rates): core 156 MHz = pll1_624 / 4, AXI 208 MHz = pll1_416 / 2 */
#define GC_CORE_SRC     APMU_GC_SRC_PLL1_624
#define GC_CORE_DIV     4
#define GC_CORE_HZ      156000000UL
#define GC_AXI_SRC      APMU_GC_SRC_PLL1_416
#define GC_AXI_DIV      2

/* ---- register access ---------------------------------------------------------------------- */

static __inline ULONG GcRd(GC_HW *Hw, ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Hw->Regs + Off));
}

static __inline VOID GcWr(GC_HW *Hw, ULONG Off, ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Hw->Regs + Off), Val);
}

static __inline ULONG ApRd(GC_HW *Hw, ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Hw->Apmu + Off));
}

static __inline VOID ApWr(GC_HW *Hw, ULONG Off, ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Hw->Apmu + Off), Val);
}

/* ---- power island and clocks ---------------------------------------------------------------- */

NTSTATUS GcPowerOn(GC_HW *Hw)
{
    ULONG v, i;

    Hw->CoreSrc = GC_CORE_SRC; Hw->CoreDiv = GC_CORE_DIV;
    Hw->AxiSrc  = GC_AXI_SRC;  Hw->AxiDiv  = GC_AXI_DIV;

    /* clock sources and dividers first, gates closed (mix type V1: plain write, no FC handshake) */
    v = ApRd(Hw, APMU_GC_OFF);
    v &= ~((APMU_GC_MUX_MASK << APMU_GC_CORE_MUX_SHIFT) | (APMU_GC_DIV_MASK << APMU_GC_CORE_DIV_SHIFT) |
           (APMU_GC_DIV_MASK << APMU_GC_AXI_DIV_SHIFT)  | (APMU_GC_MUX_MASK << APMU_GC_AXI_MUX_SHIFT));
    v |= (Hw->CoreSrc << APMU_GC_CORE_MUX_SHIFT) | ((Hw->CoreDiv - 1) << APMU_GC_CORE_DIV_SHIFT) |
         (Hw->AxiSrc  << APMU_GC_AXI_MUX_SHIFT)  | ((Hw->AxiDiv - 1)  << APMU_GC_AXI_DIV_SHIFT);
    ApWr(Hw, APMU_GC_OFF, v);

    /* clk_bulk_prepare_enable(reg, bus, core) */
    v |= APMU_GC_HCLK_EN | APMU_GC_AXI_EN | APMU_GC_CORE_EN;
    ApWr(Hw, APMU_GC_OFF, v);
    KeStallExecutionProcessor(10);

    /* pxa988_pd_power_on */
    ApWr(Hw, APMU_PWR_BLK_TMR_OFF, APMU_PWR_BLK_TMR_VAL);
    v |= APMU_GC_HWMODE;
    ApWr(Hw, APMU_GC_OFF, v);
    ApWr(Hw, APMU_PWR_CTRL_OFF, ApRd(Hw, APMU_PWR_CTRL_OFF) | APMU_PWR_GC);
    for (i = 0; i < 1000 && !(ApRd(Hw, APMU_PWR_STATUS_OFF) & APMU_PWR_GC); i++)
        KeStallExecutionProcessor(10);
    if (!(ApRd(Hw, APMU_PWR_STATUS_OFF) & APMU_PWR_GC)) {
        ApWr(Hw, APMU_PWR_CTRL_OFF, ApRd(Hw, APMU_PWR_CTRL_OFF) & ~APMU_PWR_GC);
        ApWr(Hw, APMU_GC_OFF, v & ~(APMU_GC_HCLK_EN | APMU_GC_AXI_EN | APMU_GC_CORE_EN));
        return STATUS_IO_TIMEOUT;
    }

    /* release the resets */
    v = ApRd(Hw, APMU_GC_OFF) | APMU_GC_RST_MASK;
    ApWr(Hw, APMU_GC_OFF, v);
    KeStallExecutionProcessor(20);

    Hw->Chip.CoreClockHz = GC_CORE_HZ;
    return STATUS_SUCCESS;
}

VOID GcPowerOff(GC_HW *Hw)
{
    ULONG v, i;

    /* pxa988_pd_power_off: resets, clocks, power island */
    v = ApRd(Hw, APMU_GC_OFF) & ~APMU_GC_RST_MASK;
    ApWr(Hw, APMU_GC_OFF, v);
    v &= ~(APMU_GC_HCLK_EN | APMU_GC_AXI_EN | APMU_GC_CORE_EN);
    ApWr(Hw, APMU_GC_OFF, v);
    ApWr(Hw, APMU_PWR_CTRL_OFF, ApRd(Hw, APMU_PWR_CTRL_OFF) & ~APMU_PWR_GC);
    for (i = 0; i < 1000 && (ApRd(Hw, APMU_PWR_STATUS_OFF) & APMU_PWR_GC); i++)
        KeStallExecutionProcessor(10);
}

/* ---- soft reset (etnaviv_hw_reset, goya: FSCALE 64) -------------------------------------------- */

static VOID GcLoadClock(GC_HW *Hw, ULONG Clock)
{
    GcWr(Hw, VIVS_HI_CLOCK_CONTROL, Clock | VIVS_HI_CLOCK_CONTROL_FSCALE_CMD_LOAD);
    GcWr(Hw, VIVS_HI_CLOCK_CONTROL, Clock);
}

NTSTATUS GcReset(GC_HW *Hw)
{
    ULONG control, idle, attempt;

    for (attempt = 0; attempt < 100; attempt++) {           /* Linux: up to one second */
        control = VIVS_HI_CLOCK_CONTROL_FSCALE_VAL(64);
        GcLoadClock(Hw, control);

        control |= VIVS_HI_CLOCK_CONTROL_ISOLATE_GPU;
        GcWr(Hw, VIVS_HI_CLOCK_CONTROL, control);
        control |= VIVS_HI_CLOCK_CONTROL_SOFT_RESET;
        GcWr(Hw, VIVS_HI_CLOCK_CONTROL, control);
        KeStallExecutionProcessor(20);
        control &= ~VIVS_HI_CLOCK_CONTROL_SOFT_RESET;
        GcWr(Hw, VIVS_HI_CLOCK_CONTROL, control);
        control &= ~VIVS_HI_CLOCK_CONTROL_ISOLATE_GPU;
        GcWr(Hw, VIVS_HI_CLOCK_CONTROL, control);

        idle = GcRd(Hw, VIVS_HI_IDLE_STATE);
        if (!(idle & VIVS_HI_IDLE_STATE_FE)) { KeStallExecutionProcessor(10000); continue; }

        control = GcRd(Hw, VIVS_HI_CLOCK_CONTROL);
        if (!(control & VIVS_HI_CLOCK_CONTROL_IDLE_3D) || !(control & VIVS_HI_CLOCK_CONTROL_IDLE_2D)) {
            KeStallExecutionProcessor(10000);
            continue;
        }

        control |= VIVS_HI_CLOCK_CONTROL_DISABLE_DEBUG_REGISTERS;
        GcWr(Hw, VIVS_HI_CLOCK_CONTROL, control);
        Hw->Resets++;
        return STATUS_SUCCESS;
    }
    return STATUS_IO_DEVICE_ERROR;
}

/* ---- identification (etnaviv_hw_identify + etnaviv_hw_specs) ----------------------------------- */

VOID GcIdentify(GC_HW *Hw)
{
    GOYAGPU_CHIP_INFO *c = &Hw->Chip;
    ULONG id = GcRd(Hw, VIVS_HI_CHIP_IDENTITY), s0, s1, s2, s3, n, clock = c->CoreClockHz, i;

    for (i = 0; i < sizeof(*c) / sizeof(ULONG); i++) ((PULONG)c)[i] = 0;
    c->Version = GOYAGPU_ABI_VERSION;
    c->Size = sizeof(*c);
    c->CoreClockHz = clock;

    if (VIVS_HI_CHIP_IDENTITY_FAMILY(id) == 0x01) {         /* older cores */
        c->Model = chipModel_GC500;
        c->Revision = VIVS_HI_CHIP_IDENTITY_REVISION(id);
    } else {
        c->Model = GcRd(Hw, VIVS_HI_CHIP_MODEL);
        c->Revision = GcRd(Hw, VIVS_HI_CHIP_REV);
    }

    /* GC1000 rev 0x5037 has no etnaviv HWDB entry: the registers are used, as on Linux */
    c->Features[0] = GcRd(Hw, VIVS_HI_CHIP_FEATURE);
    c->Features[1] = GcRd(Hw, VIVS_HI_CHIP_MINOR_FEATURE_0);
    if (c->Features[1] & chipMinorFeatures0_MORE_MINOR_FEATURES) {
        c->Features[2] = GcRd(Hw, VIVS_HI_CHIP_MINOR_FEATURE_1);
        c->Features[3] = GcRd(Hw, VIVS_HI_CHIP_MINOR_FEATURE_2);
        c->Features[4] = GcRd(Hw, VIVS_HI_CHIP_MINOR_FEATURE_3);
        c->Features[5] = GcRd(Hw, VIVS_HI_CHIP_MINOR_FEATURE_4);
        c->Features[6] = GcRd(Hw, VIVS_HI_CHIP_MINOR_FEATURE_5);

        s0 = GcRd(Hw, VIVS_HI_CHIP_SPECS);
        s1 = GcRd(Hw, VIVS_HI_CHIP_SPECS_2);
        s2 = GcRd(Hw, VIVS_HI_CHIP_SPECS_3);
        s3 = GcRd(Hw, VIVS_HI_CHIP_SPECS_4);
        c->StreamCount = SPECS_STREAM_COUNT(s0);
        c->RegisterMax = SPECS_REGISTER_MAX(s0);
        c->ThreadCount = SPECS_THREAD_COUNT(s0);
        c->VertexCacheSize = SPECS_VERTEX_CACHE_SIZE(s0);
        c->ShaderCoreCount = SPECS_SHADER_CORE_COUNT(s0);
        c->PixelPipes = SPECS_PIXEL_PIPES(s0);
        c->VertexOutputBufferSize = SPECS_VERTEX_OUTPUT_BUFFER_SIZE(s0);
        c->BufferSize = SPECS_2_BUFFER_SIZE(s1);
        c->InstructionCount = SPECS_2_INSTRUCTION_COUNT(s1);
        c->NumConstants = SPECS_2_NUM_CONSTANTS(s1);
        c->NumVaryings = SPECS_3_VARYINGS_COUNT(s2);
        n = SPECS_4_STREAM_COUNT(s3);
        if (n) c->StreamCount = n;
    }

    /* defaults and conversions, as etnaviv_hw_specs */
    if (c->StreamCount == 0) c->StreamCount = (c->Model >= 0x1000) ? 4 : 1;
    if (c->RegisterMax) c->RegisterMax = 1UL << c->RegisterMax;
    else c->RegisterMax = (c->Model == chipModel_GC400) ? 32 : 64;
    if (c->ThreadCount) c->ThreadCount = 1UL << c->ThreadCount;
    else c->ThreadCount = (c->Model == chipModel_GC400) ? 64 : 256;
    if (c->VertexCacheSize == 0) c->VertexCacheSize = 8;
    if (c->ShaderCoreCount == 0) c->ShaderCoreCount = (c->Model >= 0x1000) ? 2 : 1;
    if (c->PixelPipes == 0) c->PixelPipes = 1;
    if (c->VertexOutputBufferSize) c->VertexOutputBufferSize = 1UL << c->VertexOutputBufferSize;
    else c->VertexOutputBufferSize = 512;
    switch (c->InstructionCount) {
    case 1:  c->InstructionCount = 1024; break;
    case 2:  c->InstructionCount = 2048; break;
    default: c->InstructionCount = 256;  break;
    }
    if (c->NumConstants == 0) c->NumConstants = 168;
    if (c->NumVaryings == 0) c->NumVaryings = (c->Features[2] & chipMinorFeatures1_HALTI0) ? 12 : 8;

    c->MmuVersion = (c->Features[2] & chipMinorFeatures1_MMU_VERSION) ? 2 : 1;
    c->ApertureBase = GOYAGPU_APERTURE_BASE;
    c->ApertureSize = GcMmuApertureSize(Hw);

    Hw->IdleMask = ~VIVS_HI_IDLE_STATE_AXI_LP;
    Hw->MemoryBase = 0;                     /* PHYS_OFFSET = 0 on the PXA986 */
}

/* ---- module clock gating, AXI, pulse eater ------------------------------------------------------ */

static VOID GcEnableMlcg(GC_HW *Hw)
{
    GOYAGPU_CHIP_INFO *c = &Hw->Chip;
    ULONG ppc, pmc;

    ppc = GcRd(Hw, VIVS_PM_POWER_CONTROLS) | VIVS_PM_POWER_CONTROLS_ENABLE_MODULE_CLOCK_GATING;
    GcWr(Hw, VIVS_PM_POWER_CONTROLS, ppc);

    pmc = GcRd(Hw, VIVS_PM_MODULE_CONTROLS);
    if (c->Model >= chipModel_GC400 && c->Model != chipModel_GC420 &&
        !(c->Features[4] & chipMinorFeatures3_BUG_FIXES12))
        pmc |= VIVS_PM_MODULE_CONTROLS_DISABLE_MODULE_CLOCK_GATING_PA;
    if (c->Revision < 0x5000 && (c->Features[1] & chipMinorFeatures0_HZ) &&
        !(c->Features[2] & chipMinorFeatures1_DISABLE_PE_GATING))
        pmc |= VIVS_PM_MODULE_CONTROLS_DISABLE_MODULE_CLOCK_GATING_PE;
    if (c->Revision < 0x5422)
        pmc |= 0x00008000;                  /* unknown bit 15, as etnaviv */
    pmc |= VIVS_PM_MODULE_CONTROLS_DISABLE_MODULE_CLOCK_GATING_RA_HZ |
           VIVS_PM_MODULE_CONTROLS_DISABLE_MODULE_CLOCK_GATING_RA_EZ;
    GcWr(Hw, VIVS_PM_MODULE_CONTROLS, pmc);
}

static VOID GcSetupPulseEater(GC_HW *Hw)
{
    ULONG pe = 0x01590880;                  /* vivante driver base value */
    if (Hw->Chip.Model == chipModel_GC1000 && (Hw->Chip.Revision == 0x5039 || Hw->Chip.Revision == 0x5040)) {
        pe &= ~(1UL << 16);
        pe |= 1UL << 17;
    }
    if (Hw->Chip.Revision > 0x5420 && (Hw->Chip.Features[0] & chipFeatures_PIPE_3D)) {
        pe = GcRd(Hw, VIVS_PM_PULSE_EATER) | (1UL << 18);
    }
    GcWr(Hw, VIVS_PM_PULSE_EATER, pe);
}

/* ---- front-end ring ----------------------------------------------------------------------------- */

static __inline VOID RingAlign(GC_HW *Hw) { Hw->RingPos = (Hw->RingPos + 7) & ~7UL; }
static __inline VOID RingOut(GC_HW *Hw, ULONG V) { Hw->Ring[Hw->RingPos >> 2] = V; Hw->RingPos += 4; }

static VOID CmdLoadState(GC_HW *Hw, ULONG Reg, ULONG Val)
{
    RingAlign(Hw);
    RingOut(Hw, VIV_FE_LOAD_STATE_HEADER_OP_LOAD_STATE | VIV_FE_LOAD_STATE_HEADER_COUNT(1) |
                VIV_FE_LOAD_STATE_HEADER_OFFSET(Reg >> 2));
    RingOut(Hw, Val);
}

static VOID CmdWait(GC_HW *Hw)
{
    RingAlign(Hw);
    RingOut(Hw, VIV_FE_WAIT_HEADER_OP_WAIT | GC_WAIT_CYCLES);
}

static VOID CmdLink(GC_HW *Hw, ULONG Prefetch, ULONG Address)
{
    RingAlign(Hw);
    RingOut(Hw, VIV_FE_LINK_HEADER_OP_LINK | VIV_FE_LINK_HEADER_PREFETCH(Prefetch));
    RingOut(Hw, Address);
}

static VOID CmdStall(GC_HW *Hw, ULONG From, ULONG To)
{
    RingAlign(Hw);
    RingOut(Hw, VIV_FE_STALL_HEADER_OP_STALL);
    RingOut(Hw, VIV_FE_STALL_TOKEN_FROM(From) | VIV_FE_STALL_TOKEN_TO(To));
}

static VOID CmdSem(GC_HW *Hw, ULONG From, ULONG To)
{
    CmdLoadState(Hw, VIVS_GL_SEMAPHORE_TOKEN, VIVS_GL_SEMAPHORE_TOKEN_FROM(From) | VIVS_GL_SEMAPHORE_TOKEN_TO(To));
}

/* room for Dwords64 contiguous 64-bit commands, wrapping to the start if needed */
static ULONG RingReserve(GC_HW *Hw, ULONG Dwords64)
{
    if (Hw->RingPos + Dwords64 * 8 > Hw->RingSize) Hw->RingPos = 0;
    return Hw->RingGpu + Hw->RingPos;
}

/* Replace the WAIT of a WAIT/LINK pair while the front end may be executing it: argument first. */
static VOID RingReplaceWait(GC_HW *Hw, ULONG WaitOffset, ULONG Cmd, ULONG Arg)
{
    volatile ULONG *lw = (volatile ULONG *)(Hw->Ring + (WaitOffset >> 2));
    lw[1] = Arg;
    GC_DSB();
    lw[0] = Cmd;
    GC_DSB();
}

NTSTATUS GcRingInit(GC_HW *Hw)
{
    PHYSICAL_ADDRESS lo, hi, bound, pa;

    lo.QuadPart = 0;
    hi.QuadPart = 0x7FFFFFFF;               /* below the MMUv1 aperture: linear window */
    bound.QuadPart = 0;
    Hw->Ring = (PULONG)MmAllocateContiguousMemorySpecifyCache(GC_RING_SIZE, lo, hi, bound, MmNonCached);
    if (Hw->Ring == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Hw->Ring, GC_RING_SIZE);
    pa = MmGetPhysicalAddress(Hw->Ring);
    Hw->RingGpu = pa.LowPart - Hw->MemoryBase;
    Hw->RingSize = GC_RING_SIZE;
    Hw->RingPos = 0;
    return STATUS_SUCCESS;
}

VOID GcRingFree(GC_HW *Hw)
{
    if (Hw->Ring) MmFreeContiguousMemorySpecifyCache(Hw->Ring, GC_RING_SIZE, MmNonCached);
    Hw->Ring = NULL;
}

/* etnaviv_buffer_init: a WAIT followed by a LINK back to it; returns the prefetch (64-bit words) */
static ULONG RingStartIdleLoop(GC_HW *Hw)
{
    ULONG wait;
    Hw->RingPos = 0;
    CmdWait(Hw);
    wait = Hw->RingGpu + Hw->RingPos - 4;
    CmdLink(Hw, 2, wait);
    return Hw->RingPos / 8;
}

NTSTATUS GcHwInit(GC_HW *Hw)
{
    ULONG prefetch;

    if (Hw->Ring == NULL || Hw->Pt == NULL) return STATUS_INVALID_DEVICE_STATE;

    GcEnableMlcg(Hw);
    /* "cacheable, no allocate" AXI attributes, as etnaviv (needed on i.MX6, harmless here) */
    GcWr(Hw, VIVS_HI_AXI_CONFIG, VIVS_HI_AXI_CONFIG_AWCACHE(2) | VIVS_HI_AXI_CONFIG_ARCACHE(2));
    GcSetupPulseEater(Hw);
    GcMmuRestore(Hw);

    prefetch = RingStartIdleLoop(Hw);
    Hw->ExecState = GC_PIPE_NONE;
    Hw->MmuFlushPending = FALSE;
    GC_DSB();

    GcWr(Hw, VIVS_HI_INTR_ENBL, 0xFFFFFFFFUL);
    GcWr(Hw, VIVS_FE_COMMAND_ADDRESS, Hw->RingGpu);
    GcWr(Hw, VIVS_FE_COMMAND_CONTROL, VIVS_FE_COMMAND_CONTROL_ENABLE | VIVS_FE_COMMAND_CONTROL_PREFETCH(prefetch));
    return STATUS_SUCCESS;
}

/* ---- submission (etnaviv_buffer_queue) ------------------------------------------------------------ */

VOID GcSubmit(GC_HW *Hw, PULONG CmdCpu, ULONG CmdGpu, ULONG CmdBytes, ULONG Pipe, ULONG Event)
{
    ULONG waitlink = Hw->RingPos - 16;      /* the WAIT/LINK the front end is parked on */
    ULONG linkTarget = CmdGpu, linkDwords, returnTarget, wait;
    BOOLEAN switchPipe = (BOOLEAN)(Hw->ExecState != Pipe), flushMmu = Hw->MmuFlushPending;

    CmdBytes = (CmdBytes + 7) & ~7UL;
    linkDwords = (CmdBytes + 8) / 8;        /* the user commands plus the LINK appended below */

    if (flushMmu || switchPipe) {
        ULONG extra = 1, target;
        if (flushMmu) extra += 3;
        if (switchPipe) extra += 4;
        target = RingReserve(Hw, extra);
        if (switchPipe) {                   /* etnaviv_cmd_select_pipe */
            ULONG flush = 0;
            if (Hw->ExecState == GOYAGPU_PIPE_2D) flush = VIVS_GL_FLUSH_CACHE_PE2D;
            else if (Hw->ExecState == GOYAGPU_PIPE_3D) flush = VIVS_GL_FLUSH_CACHE_DEPTH | VIVS_GL_FLUSH_CACHE_COLOR;
            CmdLoadState(Hw, VIVS_GL_FLUSH_CACHE, flush);
            CmdSem(Hw, SYNC_RECIPIENT_FE, SYNC_RECIPIENT_PE);
            CmdStall(Hw, SYNC_RECIPIENT_FE, SYNC_RECIPIENT_PE);
            CmdLoadState(Hw, VIVS_GL_PIPE_SELECT, VIVS_GL_PIPE_SELECT_PIPE(Pipe));
            Hw->ExecState = Pipe;
        }
        if (flushMmu) {
            CmdLoadState(Hw, VIVS_GL_FLUSH_MMU, VIVS_GL_FLUSH_MMU_V1_ALL);
            CmdSem(Hw, SYNC_RECIPIENT_FE, SYNC_RECIPIENT_PE);
            CmdStall(Hw, SYNC_RECIPIENT_FE, SYNC_RECIPIENT_PE);
            Hw->MmuFlushPending = FALSE;
        }
        CmdLink(Hw, linkDwords, linkTarget);
        linkTarget = target;
        linkDwords = extra;
    }

    /* return path: LINK at the end of the user commands back into the ring */
    returnTarget = RingReserve(Hw, 7);
    CmdCpu[CmdBytes >> 2]       = VIV_FE_LINK_HEADER_OP_LINK | VIV_FE_LINK_HEADER_PREFETCH(7);
    CmdCpu[(CmdBytes >> 2) + 1] = returnTarget;

    /* cache flush, stall, event, then a new WAIT/LINK idle loop */
    if (Hw->ExecState == GOYAGPU_PIPE_2D) {
        CmdLoadState(Hw, VIVS_GL_FLUSH_CACHE, VIVS_GL_FLUSH_CACHE_PE2D);
    } else {
        CmdLoadState(Hw, VIVS_GL_FLUSH_CACHE, VIVS_GL_FLUSH_CACHE_DEPTH | VIVS_GL_FLUSH_CACHE_COLOR);
        CmdLoadState(Hw, VIVS_TS_FLUSH_CACHE, VIVS_TS_FLUSH_CACHE_FLUSH);
    }
    CmdSem(Hw, SYNC_RECIPIENT_FE, SYNC_RECIPIENT_PE);
    CmdStall(Hw, SYNC_RECIPIENT_FE, SYNC_RECIPIENT_PE);
    CmdLoadState(Hw, VIVS_GL_EVENT, VIVS_GL_EVENT_EVENT_ID(Event) | VIVS_GL_EVENT_FROM_PE);
    CmdWait(Hw);
    wait = Hw->RingGpu + Hw->RingPos - 4;
    CmdLink(Hw, 2, wait);

    /* everything above must be in memory before the front end can branch to it */
    GC_DSB();
    RingReplaceWait(Hw, waitlink, VIV_FE_LINK_HEADER_OP_LINK | VIV_FE_LINK_HEADER_PREFETCH(linkDwords), linkTarget);
}

/* ---- interrupts and status ------------------------------------------------------------------------ */

ULONG GcIsrAck(GC_HW *Hw)
{
    ULONG intr = GcRd(Hw, VIVS_HI_INTR_ACKNOWLEDGE);       /* reading acknowledges */
    if (intr & VIVS_HI_INTR_ACKNOWLEDGE_MMU_EXCEPTION) Hw->MmuExceptions++;
    if (intr & VIVS_HI_INTR_ACKNOWLEDGE_AXI_BUS_ERROR) Hw->AxiBusErrors++;
    return intr;
}

BOOLEAN GcIsIdle(GC_HW *Hw)
{
    return (BOOLEAN)((GcRd(Hw, VIVS_HI_IDLE_STATE) & Hw->IdleMask) == Hw->IdleMask);
}

VOID GcGetStatus(GC_HW *Hw, GOYAGPU_GPU_STATUS *St)
{
    RtlZeroMemory(St, sizeof(*St));
    St->IdleState       = GcRd(Hw, VIVS_HI_IDLE_STATE);
    St->ClockControl    = GcRd(Hw, VIVS_HI_CLOCK_CONTROL);
    St->FeDmaAddress    = GcRd(Hw, VIVS_FE_DMA_ADDRESS);
    St->FeDmaStatus     = GcRd(Hw, VIVS_FE_DMA_STATUS);
    St->FeDmaDebugState = GcRd(Hw, VIVS_FE_DMA_DEBUG_STATE);
    St->AxiStatus       = GcRd(Hw, VIVS_HI_AXI_STATUS);
    St->MmuExceptions   = Hw->MmuExceptions;
    St->AxiBusErrors    = Hw->AxiBusErrors;
    St->Resets          = Hw->Resets;
}
