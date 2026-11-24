/* goyacmp.exe <file1> <file2> - byte compare for WinPE on the Goya tablet (no fc.exe, comp.exe or
 * script engine in the image). Both files are read with FILE_FLAG_NO_BUFFERING, so every byte comes
 * from the disk and not from the file cache; the read time is printed as well (an uncached read
 * benchmark). For the first 20 differing 1 MiB pieces the first differing offset is printed.
 * Exit code: 0 identical, 1 different, 2 cannot open / read.
 * ARM32 Thumb-2, no C runtime, no division (the Cortex-A9 has no hardware divide). */
#include <windows.h>

#define CHUNK (1u << 20)

static HANDLE mOut;

static void Say(const char *Fmt, ...)
{
  char Buf[512];
  DWORD Len, Written;
  va_list Ap;
  va_start(Ap, Fmt);
  Len = (DWORD)wvsprintfA(Buf, Fmt, Ap);
  va_end(Ap);
  WriteFile(mOut, Buf, Len, &Written, NULL);
}

/* next argument of the command line (quotes allowed); returns NULL at the end */
static WCHAR *NextArg(WCHAR **P)
{
  WCHAR *S = *P, *A;
  while (*S == L' ' || *S == L'\t') S++;
  if (*S == 0) return NULL;
  if (*S == L'"') {
    A = ++S;
    while (*S && *S != L'"') S++;
  } else {
    A = S;
    while (*S && *S != L' ' && *S != L'\t') S++;
  }
  if (*S) *S++ = 0;
  *P = S;
  return A;
}

/* reads up to CHUNK bytes; returns bytes read, ~0 on error. NO_BUFFERING needs sector-aligned
   offsets and sizes, so a short read (end of file: the last piece is not a sector multiple) ends it
   instead of reading on at an unaligned position (that failed with error 87 on the tablet). */
static DWORD ReadPiece(HANDLE H, BYTE *Buf)
{
  DWORD Got = 0, N;
  while (Got < CHUNK) {
    if (!ReadFile(H, Buf + Got, CHUNK - Got, &N, NULL)) return ~0u;
    Got += N;
    if (N == 0 || (N & 511)) break;
  }
  return Got;
}

void mainCRTStartup(void)
{
  WCHAR *Cmd = GetCommandLineW(), *Name1, *Name2;
  HANDLE H1, H2;
  BYTE *B1, *B2;
  DWORD Off = 0, N1, N2, I, First, Pieces = 0, Bytes = 0, T0, Ms1 = 0, Ms2 = 0, T;
  LARGE_INTEGER S1, S2;

  mOut = GetStdHandle(STD_OUTPUT_HANDLE);
  NextArg(&Cmd);                                     /* program name */
  Name1 = NextArg(&Cmd);
  Name2 = NextArg(&Cmd);
  if (!Name1 || !Name2) { Say("usage: goyacmp file1 file2\r\n"); ExitProcess(2); }

  H1 = CreateFileW(Name1, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
  H2 = CreateFileW(Name2, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
  if (H1 == INVALID_HANDLE_VALUE || H2 == INVALID_HANDLE_VALUE) {
    Say("goyacmp: cannot open %s (error %lu)\r\n", H1 == INVALID_HANDLE_VALUE ? "file 1" : "file 2", GetLastError());
    ExitProcess(2);
  }
  GetFileSizeEx(H1, &S1);
  GetFileSizeEx(H2, &S2);
  if (S1.QuadPart != S2.QuadPart)
    Say("goyacmp: sizes differ: %lu / %lu bytes\r\n", (DWORD)S1.QuadPart, (DWORD)S2.QuadPart);

  /* page-aligned buffers (NO_BUFFERING needs sector alignment) */
  B1 = VirtualAlloc(NULL, CHUNK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  B2 = VirtualAlloc(NULL, CHUNK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!B1 || !B2) { Say("goyacmp: out of memory\r\n"); ExitProcess(2); }

  for (;;) {
    T0 = GetTickCount(); N1 = ReadPiece(H1, B1); T = GetTickCount(); Ms1 += T - T0;
    N2 = ReadPiece(H2, B2); Ms2 += GetTickCount() - T;
    if (N1 == ~0u || N2 == ~0u) {
      Say("goyacmp: read error at offset %lu (error %lu)\r\n", Off, GetLastError());
      ExitProcess(2);
    }
    if (N1 == 0 && N2 == 0) break;
    if (N1 != N2) N1 = N1 < N2 ? N1 : N2;            /* sizes differ: compare the common part */
    for (I = 0, First = ~0u; I < N1; I++) {
      if (B1[I] != B2[I]) { Bytes++; if (First == ~0u) First = I; }
    }
    if (First != ~0u) {
      if (++Pieces <= 20)
        Say("goyacmp: MiB %lu differs, first at offset %lu (0x%08lX): %02X / %02X\r\n",
            Off >> 20, Off + First, Off + First, B1[First], B2[First]);
    }
    Off += N1;
    if (N1 < CHUNK) break;
  }
  Say("goyacmp: compared %lu bytes, %lu bytes in %lu MiB pieces differ\r\n", Off, Bytes, Pieces);
  Say("goyacmp: uncached read time: file 1 %lu ms, file 2 %lu ms\r\n", Ms1, Ms2);
  ExitProcess(Bytes == 0 && Pieces == 0 && S1.QuadPart == S2.QuadPart ? 0 : 1);
}
