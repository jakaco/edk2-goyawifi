/* goyalog - persistent boot / crash log for Windows RT on goyawifi.

   The firmware reserves 0x08100000-0x0817FFFF (the Android ramoops area, kept out of the UEFI memory
   map handed to Windows). DRAM keeps its contents across a watchdog reset, so Windows writes its log
   there and the firmware saves it on the next start:

     goyabug (boot start)  header + boot log: boot number, loaded drivers, started programs, heartbeat,
                           shutdown; at a stop the same text it draws on the stop screen (crash area),
                           then a watchdog reset after a few seconds.
     goyatrace.h           every Goya driver records its last callback site, a call count and the
                           first exception its guard caught in its own slot (plain stores, no locks).
     firmware (BDS)        if the header is valid and the log has not been saved yet: writes
                           \goya\logs\boot-NNNN-<state>.txt on the card's FAT32 system partition,
                           marks it saved, and restarts into recovery (TWRP) if Windows did not shut
                           down cleanly, so the log can be pulled over adb.

   Plain 32-bit fields only: the same layout is used by the drivers (ntddk) and the firmware
   (Platform/Samsung/GoyaWifiPkg/Include/GoyaLog.h, keep both in sync). */
#pragma once

#define GOYALOG_PHYS        0x08100000UL
#define GOYALOG_SIZE        0x00080000UL
#define GOYALOG_MAGIC0      0x41594F47UL      /* "GOYA" */
#define GOYALOG_MAGIC1      0x31474F4CUL      /* "LOG1" */

#define GOYALOG_TEXT_OFF    0x00000100UL      /* boot log text */
#define GOYALOG_CRASH_OFF   0x00070000UL      /* stop details text */
#define GOYALOG_TEXT_MAX    (GOYALOG_CRASH_OFF - GOYALOG_TEXT_OFF)
#define GOYALOG_CRASH_MAX   (GOYALOG_SIZE - GOYALOG_CRASH_OFF)

#define GOYALOG_SAVED       0                 /* firmware wrote it to the card */
#define GOYALOG_RUNNING     1                 /* Windows running (or hung / reset by hand) */
#define GOYALOG_CRASHED     2                 /* stop screen, goyabug reset the tablet */
#define GOYALOG_SHUTDOWN    3                 /* clean shutdown or restart */

#define GOYALOG_SLOTS       8                 /* GOYA_TRACE_ID 1..7 (0 unused) */

typedef struct _GOYALOG_SLOT {
    unsigned int Site;                        /* last GoyaTraceStep site */
    unsigned int Count;                       /* GoyaTraceStep calls */
    unsigned int ExcCode;                     /* first exception caught by GT_GUARD (0 = none) */
    unsigned int ExcInfo;                     /* site << 24 | offset in the driver image (low 24 bits) */
} GOYALOG_SLOT;

typedef struct _GOYALOG_HDR {
    unsigned int Magic0, Magic1;
    unsigned int State;                       /* GOYALOG_* above */
    unsigned int Boot;                        /* boot number, +1 every Windows start */
    unsigned int Used;                        /* bytes of boot log text */
    unsigned int Uptime;                      /* seconds, heartbeat from goyabug */
    unsigned int CrashUsed;                   /* bytes of stop text */
    unsigned int StopCode;                    /* bugcheck code (when State = CRASHED) */
    GOYALOG_SLOT Slot[GOYALOG_SLOTS];
} GOYALOG_HDR;                                /* 32 + 128 = 160 bytes, below GOYALOG_TEXT_OFF */
