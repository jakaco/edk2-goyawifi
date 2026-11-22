/* host stand-in for the few kernel interfaces gc1000_hw.c / gc1000_mmu.c use */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t ULONG, *PULONG; typedef int32_t LONG, NTSTATUS; typedef uint8_t UCHAR, BOOLEAN;
typedef uint64_t ULONGLONG; typedef void VOID, *PVOID;
typedef union { struct { ULONG LowPart; LONG HighPart; }; int64_t QuadPart; } PHYSICAL_ADDRESS;
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0
#define STATUS_IO_TIMEOUT ((NTSTATUS)0xC00000B5)
#define STATUS_IO_DEVICE_ERROR ((NTSTATUS)0xC0000185)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009A)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000D)
#define STATUS_INVALID_DEVICE_STATE ((NTSTATUS)0xC0000184)
#define NT_SUCCESS(s) ((s) >= 0)
typedef enum { MmNonCached } MEMORY_CACHING_TYPE;
ULONG sim_rd(volatile ULONG *r); void sim_wr(volatile ULONG *r, ULONG v);
#define READ_REGISTER_ULONG(r)  sim_rd(r)
#define WRITE_REGISTER_ULONG(r, v) sim_wr(r, v)
#define KeStallExecutionProcessor(us) ((void)(us))
#define KeMemoryBarrier() ((void)0)
#define RtlZeroMemory(p, n) memset((p), 0, (n))
PVOID MmAllocateContiguousMemorySpecifyCache(ULONG n, PHYSICAL_ADDRESS lo, PHYSICAL_ADDRESS hi, PHYSICAL_ADDRESS b, MEMORY_CACHING_TYPE t);
void MmFreeContiguousMemorySpecifyCache(PVOID p, ULONG n, MEMORY_CACHING_TYPE t);
PHYSICAL_ADDRESS MmGetPhysicalAddress(PVOID p);
