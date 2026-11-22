/* Host test of the goyagpu hardware layer: power-on/reset register sequence, ring layout and the
   submission sequence, compared with Linux etnaviv (etnaviv_buffer_init / etnaviv_buffer_queue).
   cc -I stub -I ../../WinRT/drivers/goyagpu -o ringtest ringtest.c && ./ringtest */
#include <stdio.h>
#include <assert.h>
#include "../../WinRT/drivers/goyagpu/gc1000_hw.c"
#include "../../WinRT/drivers/goyagpu/gc1000_mmu.c"

static ULONG gpu[GC_REGS_SIZE / 4], apmu[APMU_MAP_SIZE / 4];
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

/* fake memory: "physical" address = offset in a big arena + 0x01000000 */
static uint8_t arena[8 << 20]; static size_t arena_used;
PVOID MmAllocateContiguousMemorySpecifyCache(ULONG n, PHYSICAL_ADDRESS lo, PHYSICAL_ADDRESS hi, PHYSICAL_ADDRESS b, MEMORY_CACHING_TYPE t)
{ (void)lo; (void)hi; (void)b; (void)t; size_t a = (arena_used + 4095) & ~4095UL; if (a + n > sizeof arena) return NULL; arena_used = a + n; return arena + a; }
void MmFreeContiguousMemorySpecifyCache(PVOID p, ULONG n, MEMORY_CACHING_TYPE t) { (void)p; (void)n; (void)t; }
PHYSICAL_ADDRESS MmGetPhysicalAddress(PVOID p) { PHYSICAL_ADDRESS pa; pa.QuadPart = 0x01000000 + ((uint8_t *)p - arena); return pa; }

/* registers: APMU power status follows control; GC idle after reset; INTR read clears */
static ULONG intr_pending;
ULONG sim_rd(volatile ULONG *r)
{
    if (r >= apmu && r < apmu + APMU_MAP_SIZE / 4) {
        size_t o = (size_t)(r - apmu) * 4;
        if (o == APMU_PWR_STATUS_OFF) return apmu[APMU_PWR_CTRL_OFF / 4] & APMU_PWR_GC;
        return *r;
    }
    size_t o = (size_t)(r - gpu) * 4;
    switch (o) {
    case VIVS_HI_IDLE_STATE: return 0x7fffffff;
    case VIVS_HI_CLOCK_CONTROL: return gpu[0] | VIVS_HI_CLOCK_CONTROL_IDLE_3D | VIVS_HI_CLOCK_CONTROL_IDLE_2D;
    case VIVS_HI_CHIP_IDENTITY: return 0x10005037 & 0x00ffffff;   /* family != 1 */
    case VIVS_HI_CHIP_MODEL: return 0x1000;
    case VIVS_HI_CHIP_REV: return 0x5037;
    case VIVS_HI_CHIP_MINOR_FEATURE_0: return chipMinorFeatures0_MORE_MINOR_FEATURES;
    case VIVS_HI_INTR_ACKNOWLEDGE: { ULONG v = intr_pending; intr_pending = 0; return v; }
    default: return *r;
    }
}
void sim_wr(volatile ULONG *r, ULONG v) { *r = v; }

int main(void)
{
    GC_HW hw; memset(&hw, 0, sizeof hw);
    hw.Regs = (volatile UCHAR *)gpu; hw.Apmu = (volatile UCHAR *)apmu;

    /* power on: clocks, island, resets */
    CHECK(GcPowerOn(&hw) == STATUS_SUCCESS, "power on");
    ULONG g = apmu[APMU_GC_OFF / 4];
    CHECK((g & 7) == 7, "resets released (APMU_GC=%08x)", g);
    CHECK(g & APMU_GC_HCLK_EN && g & APMU_GC_AXI_EN && g & APMU_GC_CORE_EN, "clock gates");
    CHECK(g & APMU_GC_HWMODE, "hw mode");
    CHECK(((g >> 6) & 3) == 1 && ((g >> 12) & 7) == 3, "core = pll1_624 / 4");
    CHECK(((g >> 20) & 3) == 0 && ((g >> 17) & 7) == 1, "axi = pll1_416 / 2");
    CHECK(apmu[APMU_PWR_BLK_TMR_OFF / 4] == 0x20001FFF, "power timer");

    CHECK(GcReset(&hw) == STATUS_SUCCESS, "reset");
    CHECK((gpu[0] & VIVS_HI_CLOCK_CONTROL_FSCALE_VAL__MASK) == VIVS_HI_CLOCK_CONTROL_FSCALE_VAL(64), "fscale 64");
    CHECK(gpu[0] & VIVS_HI_CLOCK_CONTROL_DISABLE_DEBUG_REGISTERS, "debug regs off after reset");
    GcIdentify(&hw);
    CHECK(hw.Chip.Model == 0x1000 && hw.Chip.Revision == 0x5037, "identity %x/%x", hw.Chip.Model, hw.Chip.Revision);
    CHECK(hw.Chip.StreamCount == 4 && hw.Chip.ShaderCoreCount == 2 && hw.Chip.NumConstants == 168, "spec defaults");
    CHECK(GcMmuInit(&hw) == STATUS_SUCCESS && hw.PtEntries == 512 * 1024, "mmu 2 MiB table");
    CHECK(hw.Chip.ApertureSize == 0x80000000UL, "aperture 2 GiB");
    CHECK(hw.Pt[12345] == hw.BadPagePhys, "unmapped -> bad page");
    CHECK(GcRingInit(&hw) == STATUS_SUCCESS, "ring");
    CHECK(GcHwInit(&hw) == STATUS_SUCCESS, "hw init");

    /* registers after init */
    CHECK(gpu[VIVS_HI_AXI_CONFIG / 4] == 0x2200, "axi config");
    CHECK(gpu[VIVS_PM_PULSE_EATER / 4] == 0x01590880, "pulse eater");
    CHECK(gpu[VIVS_PM_POWER_CONTROLS / 4] & 1, "module clock gating on");
    CHECK((gpu[VIVS_PM_MODULE_CONTROLS / 4] & 0x00038010) == 0x00038010, "PA/bit15/RA_HZ/RA_EZ gating off (%08x)", gpu[VIVS_PM_MODULE_CONTROLS / 4]);
    for (ULONG r = VIVS_MC_MMU_FE_PAGE_TABLE; r <= VIVS_MC_MMU_RA_PAGE_TABLE; r += 4) CHECK(gpu[r / 4] == hw.PtPhys, "page table reg %x", r);
    for (ULONG r = VIVS_MC_MEMORY_BASE_ADDR_RA; r <= VIVS_MC_MEMORY_BASE_ADDR_PE; r += 4) CHECK(gpu[r / 4] == 0, "memory base reg %x", r);
    CHECK(gpu[VIVS_FE_COMMAND_ADDRESS / 4] == hw.RingGpu, "FE address");
    CHECK(gpu[VIVS_FE_COMMAND_CONTROL / 4] == (VIVS_FE_COMMAND_CONTROL_ENABLE | 2), "FE control prefetch 2");
    CHECK(gpu[VIVS_HI_INTR_ENBL / 4] == 0xFFFFFFFF, "interrupts enabled");

    /* ring idle loop: WAIT at 0, LINK(2) back to it at 8 */
    CHECK(hw.Ring[0] == (VIV_FE_WAIT_HEADER_OP_WAIT | 200), "WAIT");
    CHECK(hw.Ring[2] == (VIV_FE_LINK_HEADER_OP_LINK | 2) && hw.Ring[3] == hw.RingGpu, "LINK back to WAIT");
    CHECK(hw.RingPos == 16, "ring pos 16");

    /* map a page, then submit a 3D command buffer of 3 LOAD_STATEs (24 bytes) with event 5 */
    PHYSICAL_ADDRESS pa; pa.QuadPart = 0x02345000;
    CHECK(GcMmuMap(&hw, 0x80001000, pa) == STATUS_SUCCESS && hw.Pt[1] == 0x02345000, "map");
    CHECK(GcMmuMap(&hw, 0x7FFFF000, pa) != STATUS_SUCCESS, "map below aperture rejected");
    ULONG *cmd = MmAllocateContiguousMemorySpecifyCache(4096, pa, pa, pa, MmNonCached);
    ULONG cmdGpu = MmGetPhysicalAddress(cmd).LowPart;
    for (int i = 0; i < 6; i++) cmd[i] = 0x11110000 + i;
    GcSubmit(&hw, cmd, cmdGpu, 24, GOYAGPU_PIPE_3D, 5);

    /* the old WAIT is now a LINK to the pipe-select + MMU flush block (extra = 1 + 3 + 4 = 8) */
    ULONG blk = hw.RingGpu + 16;
    CHECK(hw.Ring[0] == (VIV_FE_LINK_HEADER_OP_LINK | 8) && hw.Ring[1] == blk, "kick: LINK(8) to block (%08x %08x)", hw.Ring[0], hw.Ring[1]);
    ULONG *b = hw.Ring + 4;
    CHECK(b[0] == (0x08010000 | (VIVS_GL_FLUSH_CACHE >> 2)) && b[1] == 0, "select: flush (none yet)");
    CHECK(b[2] == (0x08010000 | (VIVS_GL_SEMAPHORE_TOKEN >> 2)) && b[3] == 0x0701, "select: sem FE->PE");
    CHECK(b[4] == VIV_FE_STALL_HEADER_OP_STALL && b[5] == 0x0701, "select: stall");
    CHECK(b[6] == (0x08010000 | (VIVS_GL_PIPE_SELECT >> 2)) && b[7] == 0, "select: 3D");
    CHECK(b[8] == (0x08010000 | (VIVS_GL_FLUSH_MMU >> 2)) && b[9] == 0x1f, "mmu flush");
    CHECK(b[10] == (0x08010000 | (VIVS_GL_SEMAPHORE_TOKEN >> 2)) && b[12] == VIV_FE_STALL_HEADER_OP_STALL, "mmu flush sem/stall");
    CHECK(b[14] == (VIV_FE_LINK_HEADER_OP_LINK | 4) && b[15] == cmdGpu, "LINK(4) into the command buffer (24 B + LINK)");
    ULONG ret = blk + 64;
    CHECK(cmd[6] == (VIV_FE_LINK_HEADER_OP_LINK | 7) && cmd[7] == ret, "return LINK(7) appended (%08x %08x)", cmd[6], cmd[7]);
    ULONG *t = hw.Ring + (ret - hw.RingGpu) / 4;
    CHECK(t[0] == (0x08010000 | (VIVS_GL_FLUSH_CACHE >> 2)) && t[1] == 3, "tail: flush depth|color");
    CHECK(t[2] == (0x08010000 | (VIVS_TS_FLUSH_CACHE >> 2)) && t[3] == 1, "tail: TS flush");
    CHECK(t[4] == (0x08010000 | (VIVS_GL_SEMAPHORE_TOKEN >> 2)) && t[6] == VIV_FE_STALL_HEADER_OP_STALL, "tail: sem/stall");
    CHECK(t[8] == (0x08010000 | (VIVS_GL_EVENT >> 2)) && t[9] == (5 | VIVS_GL_EVENT_FROM_PE), "tail: event 5 from PE");
    CHECK(t[10] == (VIV_FE_WAIT_HEADER_OP_WAIT | 200), "tail: WAIT");
    CHECK(t[12] == (VIV_FE_LINK_HEADER_OP_LINK | 2) && t[13] == ret + 40, "tail: LINK back to the new WAIT");
    CHECK(!hw.MmuFlushPending && hw.ExecState == GOYAGPU_PIPE_3D, "state after submit");

    /* second submission: no pipe switch, no flush -> the WAIT is replaced by a LINK straight into cmd */
    ULONG w2 = (ret + 40) - hw.RingGpu;
    GcSubmit(&hw, cmd, cmdGpu, 24, GOYAGPU_PIPE_3D, 6);
    CHECK(hw.Ring[w2 / 4] == (VIV_FE_LINK_HEADER_OP_LINK | 4) && hw.Ring[w2 / 4 + 1] == cmdGpu, "second kick goes straight to cmd");

    /* interrupts */
    intr_pending = (1u << 5) | VIVS_HI_INTR_ACKNOWLEDGE_MMU_EXCEPTION;
    ULONG i = GcIsrAck(&hw);
    CHECK(i == ((1u << 5) | VIVS_HI_INTR_ACKNOWLEDGE_MMU_EXCEPTION) && hw.MmuExceptions == 1 && GcIsrAck(&hw) == 0, "isr ack");

    printf(fails ? "%d FAILED\n" : "ALL GPU LAYER TESTS PASSED\n", fails);
    return fails != 0;
}
