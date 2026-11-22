/*
 * gc1000_regs.h - Vivante GC1000 (Marvell PXA986/988) register map for the goyagpu driver.
 *
 * GPU register offsets, fields and command-stream encodings are taken from the Linux 4.19
 * etnaviv headers in the goyawifi kernel (state_hi.xml.h, state.xml.h, state_3d.xml.h,
 * cmdstream.xml.h, common.xml.h; generated from the etna_viv rnndb). PXA988 APMU bits come from
 * the goyawifi DT (gc_power, gpu) and drivers (pxa988-pm-domain.c, clk-of-pxa988.c).
 */
#pragma once

/* ---- SoC addresses ------------------------------------------------------------------------- */
#define GC_REGS_PHYS            0xC0400000UL    /* DT gpu@c0400000 */
#define GC_REGS_SIZE            0x00040000UL
#define GC_GSIV                 40              /* GIC SPI 8, level high */

#define PXA_APMU_PHYS           0xD4282800UL
#define APMU_GC_OFF             0x0CC           /* 0xD42828CC: GC clock / reset control */
#define APMU_PWR_CTRL_OFF       0x0D8           /* 0xD42828D8: power island control */
#define APMU_PWR_BLK_TMR_OFF    0x0DC           /* 0xD42828DC: power island timer */
#define APMU_PWR_STATUS_OFF     0x0F0           /* 0xD42828F0: power island status */
#define APMU_MAP_SIZE           0x100

/* APMU_GC (one register shared by the power island and the three GC clocks) */
#define APMU_GC_RST_MASK        0x00000007UL    /* resets released = 1 (pxa988_gc_pd_desc) */
#define APMU_GC_AXI_EN          0x00000008UL    /* gc_axi gate */
#define APMU_GC_CORE_EN         0x00000010UL    /* gc_clk gate */
#define APMU_GC_HCLK_EN         0x00000020UL    /* gc_hclk gate (register interface) */
#define APMU_GC_CORE_MUX_SHIFT  6               /* 2 bits */
#define APMU_GC_HWMODE          0x00000800UL    /* bit 11 */
#define APMU_GC_CORE_DIV_SHIFT  12              /* 3 bits, divisor - 1 */
#define APMU_GC_AXI_DIV_SHIFT   17              /* 3 bits, divisor - 1 */
#define APMU_GC_AXI_MUX_SHIFT   20              /* 2 bits */
#define APMU_GC_MUX_MASK        3UL
#define APMU_GC_DIV_MASK        7UL
/* clock parents (lcd_axi_parent_names) */
#define APMU_GC_SRC_PLL1_416    0
#define APMU_GC_SRC_PLL1_624    1
#define APMU_GC_SRC_PLL2        2
#define APMU_GC_SRC_PLL2P       3

#define APMU_PWR_GC             0x00000001UL    /* bit 0 in PWR_CTRL / PWR_STATUS */
#define APMU_PWR_BLK_TMR_VAL    0x20001FFFUL

/* ---- HI: host interface ------------------------------------------------------------------- */
#define VIVS_HI_CLOCK_CONTROL                       0x0000
#define   VIVS_HI_CLOCK_CONTROL_CLK3D_DIS           0x00000001
#define   VIVS_HI_CLOCK_CONTROL_CLK2D_DIS           0x00000002
#define   VIVS_HI_CLOCK_CONTROL_FSCALE_VAL(x)       (((x) << 2) & 0x000001fc)
#define   VIVS_HI_CLOCK_CONTROL_FSCALE_VAL__MASK    0x000001fc
#define   VIVS_HI_CLOCK_CONTROL_FSCALE_CMD_LOAD     0x00000200
#define   VIVS_HI_CLOCK_CONTROL_DISABLE_RAM_CLK_GATING  0x00000400
#define   VIVS_HI_CLOCK_CONTROL_DISABLE_DEBUG_REGISTERS 0x00000800
#define   VIVS_HI_CLOCK_CONTROL_SOFT_RESET          0x00001000
#define   VIVS_HI_CLOCK_CONTROL_IDLE_3D             0x00010000
#define   VIVS_HI_CLOCK_CONTROL_IDLE_2D             0x00020000
#define   VIVS_HI_CLOCK_CONTROL_IDLE_VG             0x00040000
#define   VIVS_HI_CLOCK_CONTROL_ISOLATE_GPU         0x00080000
#define VIVS_HI_IDLE_STATE                          0x0004
#define   VIVS_HI_IDLE_STATE_FE                     0x00000001
#define   VIVS_HI_IDLE_STATE_AXI_LP                 0x80000000
#define VIVS_HI_AXI_CONFIG                          0x0008
#define   VIVS_HI_AXI_CONFIG_AWCACHE(x)             (((x) << 8) & 0x00000f00)
#define   VIVS_HI_AXI_CONFIG_ARCACHE(x)             (((x) << 12) & 0x0000f000)
#define VIVS_HI_AXI_STATUS                          0x000c
#define VIVS_HI_INTR_ACKNOWLEDGE                    0x0010  /* read clears */
#define   VIVS_HI_INTR_ACKNOWLEDGE_INTR_VEC__MASK   0x3fffffff
#define   VIVS_HI_INTR_ACKNOWLEDGE_MMU_EXCEPTION    0x40000000
#define   VIVS_HI_INTR_ACKNOWLEDGE_AXI_BUS_ERROR    0x80000000
#define VIVS_HI_INTR_ENBL                           0x0014
#define VIVS_HI_CHIP_IDENTITY                       0x0018
#define   VIVS_HI_CHIP_IDENTITY_FAMILY(v)           (((v) >> 24) & 0xff)
#define   VIVS_HI_CHIP_IDENTITY_REVISION(v)         (((v) >> 12) & 0xf)
#define VIVS_HI_CHIP_FEATURE                        0x001c
#define VIVS_HI_CHIP_MODEL                          0x0020
#define VIVS_HI_CHIP_REV                            0x0024
#define VIVS_HI_CHIP_DATE                           0x0028
#define VIVS_HI_CHIP_TIME                           0x002c
#define VIVS_HI_CHIP_MINOR_FEATURE_0                0x0034
#define VIVS_HI_CHIP_SPECS                          0x0048
#define VIVS_HI_CHIP_MINOR_FEATURE_1                0x0074
#define VIVS_HI_CHIP_SPECS_2                        0x0080
#define VIVS_HI_CHIP_MINOR_FEATURE_2                0x0084
#define VIVS_HI_CHIP_MINOR_FEATURE_3                0x0088
#define VIVS_HI_CHIP_SPECS_3                        0x008c
#define VIVS_HI_CHIP_MINOR_FEATURE_4                0x0094
#define VIVS_HI_CHIP_SPECS_4                        0x009c
#define VIVS_HI_CHIP_MINOR_FEATURE_5                0x00a0

/* CHIP_SPECS fields */
#define SPECS_STREAM_COUNT(v)               (((v) >> 0) & 0xf)
#define SPECS_REGISTER_MAX(v)               (((v) >> 4) & 0xf)
#define SPECS_THREAD_COUNT(v)               (((v) >> 8) & 0xf)
#define SPECS_VERTEX_CACHE_SIZE(v)          (((v) >> 12) & 0x1f)
#define SPECS_SHADER_CORE_COUNT(v)          (((v) >> 20) & 0x1f)
#define SPECS_PIXEL_PIPES(v)                (((v) >> 25) & 0x7)
#define SPECS_VERTEX_OUTPUT_BUFFER_SIZE(v)  (((v) >> 28) & 0xf)
#define SPECS_2_BUFFER_SIZE(v)              (((v) >> 0) & 0xff)
#define SPECS_2_INSTRUCTION_COUNT(v)        (((v) >> 8) & 0xff)
#define SPECS_2_NUM_CONSTANTS(v)            (((v) >> 16) & 0xffff)
#define SPECS_3_VARYINGS_COUNT(v)           (((v) >> 4) & 0x1f)
#define SPECS_4_STREAM_COUNT(v)             (((v) >> 12) & 0x1f)

/* ---- PM: power management (not offset on GC1000) ------------------------------------------ */
#define VIVS_PM_POWER_CONTROLS                      0x0100
#define   VIVS_PM_POWER_CONTROLS_ENABLE_MODULE_CLOCK_GATING 0x00000001
#define VIVS_PM_MODULE_CONTROLS                     0x0104
#define   VIVS_PM_MODULE_CONTROLS_DISABLE_MODULE_CLOCK_GATING_PE    0x00000004
#define   VIVS_PM_MODULE_CONTROLS_DISABLE_MODULE_CLOCK_GATING_PA    0x00000010
#define   VIVS_PM_MODULE_CONTROLS_DISABLE_MODULE_CLOCK_GATING_TX    0x00000080
#define   VIVS_PM_MODULE_CONTROLS_DISABLE_MODULE_CLOCK_GATING_RA_EZ 0x00010000
#define   VIVS_PM_MODULE_CONTROLS_DISABLE_MODULE_CLOCK_GATING_RA_HZ 0x00020000
#define VIVS_PM_PULSE_EATER                         0x010c

/* ---- MC: memory controller / MMUv1 -------------------------------------------------------- */
#define VIVS_MC_MMU_FE_PAGE_TABLE                   0x0400
#define VIVS_MC_MMU_TX_PAGE_TABLE                   0x0404
#define VIVS_MC_MMU_PE_PAGE_TABLE                   0x0408
#define VIVS_MC_MMU_PEZ_PAGE_TABLE                  0x040c
#define VIVS_MC_MMU_RA_PAGE_TABLE                   0x0410
#define VIVS_MC_DEBUG_MEMORY                        0x0414
#define VIVS_MC_MEMORY_BASE_ADDR_RA                 0x0418
#define VIVS_MC_MEMORY_BASE_ADDR_FE                 0x041c
#define VIVS_MC_MEMORY_BASE_ADDR_TX                 0x0420
#define VIVS_MC_MEMORY_BASE_ADDR_PEZ                0x0424
#define VIVS_MC_MEMORY_BASE_ADDR_PE                 0x0428
#define VIVS_MC_MEMORY_TIMING_CONTROL               0x042c
#define VIVS_MC_BUS_CONFIG                          0x0480

/* ---- FE: front end ------------------------------------------------------------------------ */
#define VIVS_FE_COMMAND_ADDRESS                     0x0654
#define VIVS_FE_COMMAND_CONTROL                     0x0658
#define   VIVS_FE_COMMAND_CONTROL_PREFETCH(x)       ((x) & 0x0000ffff)
#define   VIVS_FE_COMMAND_CONTROL_ENABLE            0x00010000
#define VIVS_FE_DMA_STATUS                          0x065c
#define VIVS_FE_DMA_DEBUG_STATE                     0x0660
#define VIVS_FE_DMA_ADDRESS                         0x0664
#define VIVS_FE_DMA_LOW                             0x0668
#define VIVS_FE_DMA_HIGH                            0x066c

/* ---- GL states used by the kernel ring (written through LOAD_STATE) ------------------------- */
#define VIVS_GL_PIPE_SELECT                         0x3800
#define   VIVS_GL_PIPE_SELECT_PIPE(x)               ((x) & 0x1)
#define VIVS_GL_EVENT                               0x3804
#define   VIVS_GL_EVENT_EVENT_ID(x)                 ((x) & 0x1f)
#define   VIVS_GL_EVENT_FROM_FE                     0x00000020
#define   VIVS_GL_EVENT_FROM_PE                     0x00000040
#define VIVS_GL_SEMAPHORE_TOKEN                     0x3808
#define   VIVS_GL_SEMAPHORE_TOKEN_FROM(x)           ((x) & 0x1f)
#define   VIVS_GL_SEMAPHORE_TOKEN_TO(x)             (((x) << 8) & 0x1f00)
#define VIVS_GL_FLUSH_CACHE                         0x380c
#define   VIVS_GL_FLUSH_CACHE_DEPTH                 0x00000001
#define   VIVS_GL_FLUSH_CACHE_COLOR                 0x00000002
#define   VIVS_GL_FLUSH_CACHE_TEXTURE               0x00000004
#define   VIVS_GL_FLUSH_CACHE_PE2D                  0x00000008
#define VIVS_GL_FLUSH_MMU                           0x3810
#define   VIVS_GL_FLUSH_MMU_V1_ALL                  0x0000001f  /* FEMMU|UNK1|UNK2|PEMMU|UNK4 */
#define VIVS_TS_FLUSH_CACHE                         0x1650
#define   VIVS_TS_FLUSH_CACHE_FLUSH                 0x00000001

#define SYNC_RECIPIENT_FE                           0x01
#define SYNC_RECIPIENT_RA                           0x05
#define SYNC_RECIPIENT_PE                           0x07

/* ---- Front-end command stream (64-bit aligned commands) ------------------------------------- */
#define VIV_FE_LOAD_STATE_HEADER_OP_LOAD_STATE      0x08000000
#define VIV_FE_LOAD_STATE_HEADER_FIXP               0x04000000
#define VIV_FE_LOAD_STATE_HEADER_COUNT(x)           (((x) << 16) & 0x03ff0000)
#define VIV_FE_LOAD_STATE_HEADER_OFFSET(x)          ((x) & 0x0000ffff)  /* state address >> 2 */
#define VIV_FE_END_HEADER_OP_END                    0x10000000
#define VIV_FE_NOP_HEADER_OP_NOP                    0x18000000
#define VIV_FE_DRAW_2D_HEADER_OP_DRAW_2D            0x20000000
#define VIV_FE_DRAW_PRIMITIVES_HEADER_OP_DRAW_PRIMITIVES 0x28000000
#define VIV_FE_DRAW_INDEXED_PRIMITIVES_HEADER_OP    0x30000000
#define VIV_FE_WAIT_HEADER_OP_WAIT                  0x38000000
#define VIV_FE_LINK_HEADER_OP_LINK                  0x40000000
#define VIV_FE_LINK_HEADER_PREFETCH(x)              ((x) & 0x0000ffff)  /* in 64-bit words */
#define VIV_FE_STALL_HEADER_OP_STALL                0x48000000
#define VIV_FE_STALL_TOKEN_FROM(x)                  ((x) & 0x1f)
#define VIV_FE_STALL_TOKEN_TO(x)                    (((x) << 8) & 0x1f00)
#define VIV_FE_CALL_HEADER_OP_CALL                  0x50000000
#define VIV_FE_RETURN_HEADER_OP_RETURN              0x58000000
#define VIV_FE_HEADER_OP_MASK                       0xf8000000

#define GC_WAIT_CYCLES                              200     /* as Linux CMD_WAIT */

/* ---- Identity / feature bits used by the driver (common.xml.h) ------------------------------ */
#define chipModel_GC300                             0x0300
#define chipModel_GC400                             0x0400
#define chipModel_GC420                             0x0420
#define chipModel_GC500                             0x0500
#define chipModel_GC1000                            0x1000
#define chipFeatures_FAST_CLEAR                     0x00000001
#define chipFeatures_PIPE_3D                        0x00000004
#define chipFeatures_PIPE_2D                        0x00000200
#define chipMinorFeatures0_MORE_MINOR_FEATURES      0x00200000
#define chipMinorFeatures0_HZ                       0x08000000
#define chipMinorFeatures1_DISABLE_PE_GATING        0x00000200
#define chipMinorFeatures1_HALTI0                   0x00800000
#define chipMinorFeatures1_MMU_VERSION              0x10000000  /* set: MMUv2 */
#define chipMinorFeatures3_BUG_FIXES12              0x00008000
