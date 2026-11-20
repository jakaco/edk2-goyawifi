/*
 * memcpy / memmove / memset for the Goya drivers, compiled into every driver.
 *
 * The 19041 WDK's libcntpr.lib versions pick an implementation from the CPU's MIDR: on a
 * Cortex-A9 they use NEON (vld1/vst1). The Windows 8 (8520) ARM kernel does not enable VFP/NEON
 * for kernel-mode code, so the first copy made by a Goya driver raised an undefined instruction
 * exception: KMODE_EXCEPTION_NOT_HANDLED during boot. These plain integer versions take
 * precedence over the library (object files are linked before libraries).
 *
 * SPDX-License-Identifier: MIT
 */
#include <ntddk.h>

#pragma function(memcpy, memmove, memset)

void *__cdecl memmove(void *Dst, const void *Src, size_t Size);

void *__cdecl memcpy(void *Dst, const void *Src, size_t Size)
{
    return memmove(Dst, Src, Size);
}

void *__cdecl memmove(void *Dst, const void *Src, size_t Size)
{
    unsigned char *D = (unsigned char *)Dst;
    const unsigned char *S = (const unsigned char *)Src;

    if (D == S || Size == 0) {
        return Dst;
    }

    if (D < S || D >= S + Size) {
        // forward; words when both are 4-byte aligned
        if ((((ULONG_PTR)D | (ULONG_PTR)S) & 3) == 0) {
#pragma loop(no_vector)
            for (; Size >= 4; Size -= 4, D += 4, S += 4) {
                *(ULONG *)D = *(const ULONG *)S;
            }
        }
#pragma loop(no_vector)
        for (; Size != 0; Size--) {
            *D++ = *S++;
        }
    } else {
        // overlapping, destination above source: backward
        D += Size;
        S += Size;
        if ((((ULONG_PTR)D | (ULONG_PTR)S) & 3) == 0) {
#pragma loop(no_vector)
            for (; Size >= 4; Size -= 4) {
                D -= 4;
                S -= 4;
                *(ULONG *)D = *(const ULONG *)S;
            }
        }
#pragma loop(no_vector)
        for (; Size != 0; Size--) {
            *--D = *--S;
        }
    }

    return Dst;
}

void *__cdecl memset(void *Dst, int Value, size_t Size)
{
    volatile unsigned char *D = (volatile unsigned char *)Dst;   // volatile: never turned back into memset
    ULONG Word = (unsigned char)Value;

    Word |= Word << 8;
    Word |= Word << 16;
#pragma loop(no_vector)
    while (Size != 0 && ((ULONG_PTR)D & 3) != 0) {
        *D++ = (unsigned char)Value;
        Size--;
    }
#pragma loop(no_vector)
    for (; Size >= 4; Size -= 4, D += 4) {
        *(volatile ULONG *)D = Word;
    }
#pragma loop(no_vector)
    for (; Size != 0; Size--) {
        *D++ = (unsigned char)Value;
    }

    return Dst;
}
