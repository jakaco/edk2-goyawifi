/** @file
  Persistent Windows log in the reserved RAM at 0x08100000 (written by goyabug, saved by BDS).
  Same layout as WinRT/drivers/common/goyalog.h - keep both in sync.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef GOYA_LOG_H_
#define GOYA_LOG_H_

#define GOYALOG_PHYS       0x08100000
#define GOYALOG_SIZE       0x00080000
#define GOYALOG_MAGIC0     0x41594F47      // "GOYA"
#define GOYALOG_MAGIC1     0x31474F4C      // "LOG1"

#define GOYALOG_TEXT_OFF   0x00000100
#define GOYALOG_CRASH_OFF  0x00070000
#define GOYALOG_TEXT_MAX   (GOYALOG_CRASH_OFF - GOYALOG_TEXT_OFF)
#define GOYALOG_CRASH_MAX  (GOYALOG_SIZE - GOYALOG_CRASH_OFF)

#define GOYALOG_SAVED      0
#define GOYALOG_RUNNING    1
#define GOYALOG_CRASHED    2
#define GOYALOG_SHUTDOWN   3

#define GOYALOG_SLOTS      8

typedef struct {
  UINT32    Site;
  UINT32    Count;
  UINT32    ExcCode;
  UINT32    ExcInfo;
} GOYALOG_SLOT;

typedef struct {
  UINT32          Magic0;
  UINT32          Magic1;
  UINT32          State;
  UINT32          Boot;
  UINT32          Used;
  UINT32          Uptime;
  UINT32          CrashUsed;
  UINT32          StopCode;
  GOYALOG_SLOT    Slot[GOYALOG_SLOTS];
} GOYALOG_HDR;

/**
  If Windows left an unsaved log, write it to \goya\logs on the card's system
  partition and, unless Windows shut down cleanly, restart into recovery.
**/
VOID
GoyaLogSave (
  VOID
  );

#endif
