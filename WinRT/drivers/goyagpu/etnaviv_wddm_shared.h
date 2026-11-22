/*
 * etnaviv_wddm_shared.h - ABI between the goyagpu WDDM kernel driver (KMD) and its user-mode
 * driver (UMD, D3D9Ex DDI on top of Mesa etnaviv) for the Vivante GC1000 in the Marvell PXA986.
 *
 * Every structure here crosses the user/kernel boundary through the WDDM private-data buffers
 * (pPrivateDriverData of QueryAdapterInfo, CreateAllocation, CreateContext, Render/SubmitCommand
 * and Escape). Rules:
 *   - fixed-size types only (ULONG/LONG/ULONGLONG), natural 4/8-byte alignment, no pointers;
 *   - every structure starts with a Version field; the KMD rejects unknown versions;
 *   - the KMD validates every value (sizes, counts, offsets) before using it.
 *
 * Address model (MMUv1):
 *   GPU address <  0x80000000  : linear window, GPU address = physical address (memory base 0)
 *   GPU address >= 0x80000000  : aperture, translated by the MMUv1 table (4 KiB pages)
 * Allocations live in the aperture segment; DMA buffers are physically contiguous and are reached
 * through the linear window. The UMD never sees physical or GPU addresses: it writes a placeholder
 * and a patch location, and the KMD fills in the address (DxgkDdiPatch).
 *
 * Builds as C for the kernel (ntddk.h) and for user mode (windows.h); include one of them first.
 */
#pragma once

#define GOYAGPU_ABI_VERSION             1

/* Shared constants */
#define GOYAGPU_APERTURE_BASE           0x80000000UL    /* MMUv1 translated range starts here */
#define GOYAGPU_PAGE_SIZE               4096UL
#define GOYAGPU_DMA_BUFFER_SIZE         (64UL * 1024)   /* per-context DMA buffer handed to the UMD */
#define GOYAGPU_DMA_TAIL_BYTES          64UL            /* kept free at the end: KMD appends LINK */
#define GOYAGPU_PATCH_LIST_ENTRIES      1024UL          /* patch locations per DMA buffer */
#define GOYAGPU_ALLOCATION_LIST_ENTRIES 256UL           /* allocations referenced per DMA buffer */
#define GOYAGPU_MAX_EVENTS              30UL            /* Vivante events 0..29 (30/31 = errors) */

/* Pipes (Vivante GL_PIPE_SELECT values, same as Linux ETNA_PIPE_*) */
#define GOYAGPU_PIPE_3D                 0UL
#define GOYAGPU_PIPE_2D                 1UL

/* ---------------------------------------------------------------------------------------------
 * Chip information: returned by DxgkDdiQueryAdapterInfo(DXGKQAITYPE_UMDRIVERPRIVATE) and by the
 * GET_CHIP_INFO escape. Field meanings and units match Linux DRM_ETNAVIV_GET_PARAM so that Mesa's
 * etnaviv screen can be filled from it (features[i] = ETNAVIV_PARAM_GPU_FEATURES_0 + i).
 * ------------------------------------------------------------------------------------------- */
#define GOYAGPU_NUM_FEATURE_WORDS       13UL

typedef struct _GOYAGPU_CHIP_INFO {
    ULONG Version;                      /* GOYAGPU_ABI_VERSION */
    ULONG Size;                         /* sizeof(GOYAGPU_CHIP_INFO) */
    ULONG Model;                        /* 0x1000 for GC1000 */
    ULONG Revision;                     /* 0x5037 on the SM-T110 */
    ULONG Features[GOYAGPU_NUM_FEATURE_WORDS];  /* chipFeatures, chipMinorFeatures0..11 */
    ULONG StreamCount;
    ULONG RegisterMax;
    ULONG ThreadCount;
    ULONG VertexCacheSize;
    ULONG ShaderCoreCount;
    ULONG PixelPipes;
    ULONG VertexOutputBufferSize;
    ULONG BufferSize;
    ULONG InstructionCount;
    ULONG NumConstants;
    ULONG NumVaryings;
    ULONG MmuVersion;                   /* 1 */
    ULONG ApertureBase;                 /* GOYAGPU_APERTURE_BASE */
    ULONG ApertureSize;                 /* bytes covered by the MMUv1 table */
    ULONG CoreClockHz;
    ULONG Reserved[8];
} GOYAGPU_CHIP_INFO;

/* UMD adapter info = chip info + driver limits (DXGKQAITYPE_UMDRIVERPRIVATE) */
typedef struct _GOYAGPU_UMD_ADAPTER_INFO {
    ULONG Version;
    ULONG Size;
    GOYAGPU_CHIP_INFO Chip;
    ULONG DmaBufferSize;                /* GOYAGPU_DMA_BUFFER_SIZE */
    ULONG DmaTailBytes;                 /* GOYAGPU_DMA_TAIL_BYTES */
    ULONG PatchListEntries;
    ULONG AllocationListEntries;
    ULONG ApertureSegmentId;            /* WDDM segment id of the aperture segment */
    ULONG Reserved[7];
} GOYAGPU_UMD_ADAPTER_INFO;

/* ---------------------------------------------------------------------------------------------
 * Allocations (DxgkDdiCreateAllocation: DXGK_ALLOCATIONINFO.pPrivateDriverData)
 * ------------------------------------------------------------------------------------------- */
typedef enum _GOYAGPU_ALLOC_TYPE {
    GOYAGPU_ALLOC_BUFFER        = 1,    /* vertex / index / constant data, linear */
    GOYAGPU_ALLOC_TEXTURE       = 2,
    GOYAGPU_ALLOC_RENDER_TARGET = 3,
    GOYAGPU_ALLOC_DEPTH_STENCIL = 4,
    GOYAGPU_ALLOC_STAGING       = 5,    /* CPU readback / upload, linear */
    GOYAGPU_ALLOC_SHADER        = 6,    /* shader instructions uploaded through the cmd stream */
    GOYAGPU_ALLOC_TILE_STATUS   = 7     /* Vivante fast-clear (TS) buffer for an RT/DS */
} GOYAGPU_ALLOC_TYPE;

typedef enum _GOYAGPU_LAYOUT {
    GOYAGPU_LAYOUT_LINEAR       = 0,
    GOYAGPU_LAYOUT_TILED        = 1,    /* 4x4 tiles */
    GOYAGPU_LAYOUT_SUPERTILED   = 2     /* 64x64 super tiles of 4x4 tiles */
} GOYAGPU_LAYOUT;

/* Flags */
#define GOYAGPU_ALLOCF_CPU_VISIBLE      0x00000001UL    /* UMD will Lock it */
#define GOYAGPU_ALLOCF_RENDER_TARGET    0x00000002UL
#define GOYAGPU_ALLOCF_TEXTURE          0x00000004UL
#define GOYAGPU_ALLOCF_DEPTH            0x00000008UL
#define GOYAGPU_ALLOCF_SHARED_PRIMARY   0x00000010UL    /* copy of the desktop for Present/Blt */
#define GOYAGPU_ALLOCF_CACHED           0x00000020UL    /* CPU-cached mapping (readback); else WC */

typedef struct _GOYAGPU_ALLOCATION_INFO {
    ULONG Version;                      /* GOYAGPU_ABI_VERSION */
    ULONG Type;                         /* GOYAGPU_ALLOC_TYPE */
    ULONG Flags;                        /* GOYAGPU_ALLOCF_* */
    ULONG Layout;                       /* GOYAGPU_LAYOUT */
    ULONG Size;                         /* bytes, multiple of GOYAGPU_PAGE_SIZE after rounding */
    ULONG Alignment;                    /* bytes, power of two, >= 64 */
    ULONG Width;                        /* pixels (0 for buffers) */
    ULONG Height;
    ULONG Depth;                        /* 1 for 2D */
    ULONG MipLevels;
    ULONG Pitch;                        /* bytes per row of level 0 (after tiling padding) */
    ULONG D3dFormat;                    /* D3DDDIFORMAT from the runtime */
    ULONG HwFormat;                     /* Vivante PE / TE format code chosen by the UMD */
    ULONG BitsPerPixel;
    ULONG LevelOffset[14];              /* byte offset of each mip level */
    ULONG Reserved[4];
} GOYAGPU_ALLOCATION_INFO;

/* Resource-level private data (DXGKARG_CREATEALLOCATION.pPrivateDriverData) */
typedef struct _GOYAGPU_RESOURCE_INFO {
    ULONG Version;
    ULONG NumAllocations;
    ULONG Reserved[6];
} GOYAGPU_RESOURCE_INFO;

/* ---------------------------------------------------------------------------------------------
 * Contexts (DxgkDdiCreateContext: pPrivateDriverData)
 * ------------------------------------------------------------------------------------------- */
typedef struct _GOYAGPU_CONTEXT_INFO {
    ULONG Version;
    ULONG Pipe;                         /* GOYAGPU_PIPE_3D (2D pipe: later) */
    ULONG Reserved[6];
} GOYAGPU_CONTEXT_INFO;

/* ---------------------------------------------------------------------------------------------
 * DMA buffers (Render -> SubmitCommand: pDmaBufferPrivateData). The UMD's command buffer holds
 * Vivante front-end commands (LOAD_STATE, DRAW_PRIMITIVES, ...), 8-byte aligned, ending before
 * the last GOYAGPU_DMA_TAIL_BYTES. Commands that change the front end's flow (LINK, CALL, RETURN,
 * END, WAIT) are rejected by the KMD; the KMD appends the cache flush, event and return LINK.
 * ------------------------------------------------------------------------------------------- */
#define GOYAGPU_DMA_SIGNATURE           0x55504747UL    /* 'GGPU' */

#define GOYAGPU_DMAF_FLUSH_TEXTURE      0x00000001UL    /* add a texture cache flush before */
#define GOYAGPU_DMAF_PRESENT            0x00000002UL    /* contains the Present/Blt resolve */

typedef struct _GOYAGPU_DMA_PRIVATE {
    ULONG Signature;                    /* GOYAGPU_DMA_SIGNATURE */
    ULONG Version;
    ULONG Pipe;                         /* GOYAGPU_PIPE_3D */
    ULONG Flags;                        /* GOYAGPU_DMAF_* */
    ULONG CommandBytes;                 /* bytes of commands written by the UMD */
    ULONG Event;                        /* KMD: Vivante event used for this submission */
    ULONG Reserved[2];
} GOYAGPU_DMA_PRIVATE;

/* Patch locations (D3DDDI_PATCHLOCATIONLIST.DriverId) */
#define GOYAGPU_PATCH_GPU_ADDRESS       0UL     /* dword = GPU address of allocation + offset */

/* ---------------------------------------------------------------------------------------------
 * Escapes (DxgkDdiEscape: pPrivateDriverData). Diagnostics and Mesa screen initialisation.
 * ------------------------------------------------------------------------------------------- */
typedef enum _GOYAGPU_ESCAPE_CODE {
    GOYAGPU_ESCAPE_GET_CHIP_INFO   = 1,         /* out: GOYAGPU_CHIP_INFO */
    GOYAGPU_ESCAPE_GET_PARAM       = 2,         /* in: Param (ETNAVIV_PARAM_*), out: Value */
    GOYAGPU_ESCAPE_GET_STATUS      = 3          /* out: GOYAGPU_GPU_STATUS */
} GOYAGPU_ESCAPE_CODE;

/* Linux ETNAVIV_PARAM_* numbers, so Mesa's etnaviv_gpu_get_param maps 1:1 */
#define GOYAGPU_PARAM_GPU_MODEL                     0x01
#define GOYAGPU_PARAM_GPU_REVISION                  0x02
#define GOYAGPU_PARAM_GPU_FEATURES_0                0x03    /* ..0x0f = FEATURES_12 */
#define GOYAGPU_PARAM_GPU_STREAM_COUNT              0x10
#define GOYAGPU_PARAM_GPU_REGISTER_MAX              0x11
#define GOYAGPU_PARAM_GPU_THREAD_COUNT              0x12
#define GOYAGPU_PARAM_GPU_VERTEX_CACHE_SIZE         0x13
#define GOYAGPU_PARAM_GPU_SHADER_CORE_COUNT         0x14
#define GOYAGPU_PARAM_GPU_PIXEL_PIPES               0x15
#define GOYAGPU_PARAM_GPU_VERTEX_OUTPUT_BUFFER_SIZE 0x16
#define GOYAGPU_PARAM_GPU_BUFFER_SIZE               0x17
#define GOYAGPU_PARAM_GPU_INSTRUCTION_COUNT         0x18
#define GOYAGPU_PARAM_GPU_NUM_CONSTANTS             0x19
#define GOYAGPU_PARAM_GPU_NUM_VARYINGS              0x1a

typedef struct _GOYAGPU_GPU_STATUS {
    ULONG IdleState;                    /* HI_IDLE_STATE */
    ULONG ClockControl;                 /* HI_CLOCK_CONTROL */
    ULONG FeDmaAddress;                 /* FE_DMA_ADDRESS (where the front end is) */
    ULONG FeDmaStatus;
    ULONG FeDmaDebugState;
    ULONG AxiStatus;
    ULONG LastFenceSubmitted;
    ULONG LastFenceCompleted;
    ULONG Interrupts;                   /* count */
    ULONG MmuExceptions;
    ULONG AxiBusErrors;
    ULONG Resets;
    ULONG Reserved[4];
} GOYAGPU_GPU_STATUS;

typedef struct _GOYAGPU_ESCAPE {
    ULONG Version;                      /* GOYAGPU_ABI_VERSION */
    ULONG Code;                         /* GOYAGPU_ESCAPE_CODE */
    LONG  Status;                       /* NTSTATUS written by the KMD */
    ULONG Param;                        /* GET_PARAM: in */
    ULONGLONG Value;                    /* GET_PARAM: out */
    union {
        GOYAGPU_CHIP_INFO  ChipInfo;
        GOYAGPU_GPU_STATUS GpuStatus;
    } u;
} GOYAGPU_ESCAPE;
