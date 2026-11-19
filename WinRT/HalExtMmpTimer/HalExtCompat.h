/*
Module Name:  HalExtCompat.h
Abstract:     Lets HalExtension.c build against both the Windows 8.0 WDK (the
              Windows RT 8.0 target) and the Windows 10 22H2 WDK (10.0.19041),
              and run on the matching HAL. The names below are the ones whose
              availability differs between the two nthalext.h versions; each is
              only given a fallback when the header did not already define it, so
              the newer WDK always uses its own definitions.

              Build with  /DHALEXT_TARGET_WIN8=1  for the 8.0 WDK (build.cmd w8),
              no define for the Windows 10 WDK (build.cmd w10).
*/
#ifndef _HALEXTCOMPAT_H
#define _HALEXTCOMPAT_H

#ifndef READ_REGISTER_NOFENCE_ULONG
#define READ_REGISTER_NOFENCE_ULONG(p)       READ_REGISTER_ULONG(p)
#endif
#ifndef WRITE_REGISTER_NOFENCE_ULONG
#define WRITE_REGISTER_NOFENCE_ULONG(p, v)   WRITE_REGISTER_ULONG((p), (v))
#endif

// Capability flags: if the header lacks one, the capability is simply not advertised.
#ifndef TIMER_ALWAYS_ON
#define TIMER_ALWAYS_ON                      0
#endif
#ifndef TIMER_GENERATES_LINE_BASED_INTERRUPTS
#define TIMER_GENERATES_LINE_BASED_INTERRUPTS 0
#endif

// The CSRT helper constants are the same in both, but guard against the 8.0 names missing.
#ifndef CSRT_RD_UID_ANY
#define CSRT_RD_UID_ANY                      0xFFFFFFFF
#endif

#ifdef HALEXT_TARGET_WIN8
// Windows 8.0 HAL: treat the descriptor data as the 8.0 timer block version.
#define HALEXT_TARGET_NAME "Windows 8.0 RT"
#else
#define HALEXT_TARGET_NAME "Windows 10"
#endif

#endif
