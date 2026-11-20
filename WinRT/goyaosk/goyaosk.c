/*
 * goyaosk.exe - touch on-screen keyboard and shell for WinPE (Windows 8 / RT ARM32) on the
 * Galaxy Tab 3 Lite 7.0 Wi-Fi (goyawifi, PXA986, 1024x600) running edk2-goyawifi.
 * Same idea as rossaosk (https://github.com/notnoelchannel2/rossaosk), written for ARM32 / Win8.
 *
 * WinPE has no osk.exe and no TabTip (needs TabletInputService). This window docks at the
 * bottom of the screen, never takes focus (WS_EX_NOACTIVATE) and turns taps into key presses
 * with SendInput into the window in front (cmd, notepad, regedit, ...).
 *
 *  - "goyaosk.exe /shell" is the WinPE shell (winpeshl.ini): starts the keyboard, then
 *    "cmd /k startnet.cmd" sized to the area above it, and never exits (WinPE reboots when
 *    the shell exits). Without /shell it is just a keyboard for whatever is in front.
 *  - Touch: handles WM_POINTERDOWN/UP (Windows 8 touch input, from goyatouch) directly and
 *    still accepts mouse clicks; left or right button, a tap is a tap.
 *  - Printable keys go out as Unicode characters (no layout or Shift-state dependence);
 *    Ctrl/Esc/Tab/Enter/arrows/Backspace/Del as real key presses.
 *  - Shift and Ctrl are sticky (one key); double-tap Shift = Caps lock.
 *  - Backspace, Del and the arrows repeat while held.
 *  - "Hide" folds the keyboard into a small tab in the bottom-right corner; tap it to bring
 *    the keyboard back. The console is resized to match.
 *  - QuickEdit off in the console (taps would start selections / paste).
 *  - goyaosk.log next to \sources\boot.wim on the boot media, else X:\Windows\Temp.
 *  - "/probe" exits with code 0x5050 (checks that the binary loads at all).
 *
 * ARM32 Thumb-2, no C runtime. The Cortex-A9 has no divide instruction: variable divisions go
 * through MulDiv (kernel32) so no __rt_sdiv helper is needed. Built with clang/lld (build.sh).
 */
#include <windows.h>

#ifndef WM_POINTERDOWN
#define WM_POINTERDOWN     0x0246
#define WM_POINTERUP       0x0247
#define WM_POINTERACTIVATE 0x024B
#endif
#ifndef PA_NOACTIVATE
#define PA_NOACTIVATE 3
#endif

typedef struct {
  const WCHAR *Label;
  const WCHAR *Shifted; /* NULL: same label, not a character key */
  WORD         Vk;
  BYTE         Width;   /* in quarter keys */
} KEY;

#define VK_SHIFT_STICKY 0xFFF0
#define VK_CTRL_STICKY  0xFFF1
#define VK_HIDE_OSK     0xFFF2

static const KEY mRow1[] = {
  {L"Esc", NULL, VK_ESCAPE, 5},
  {L"1", L"!", '1', 4}, {L"2", L"@", '2', 4}, {L"3", L"#", '3', 4}, {L"4", L"$", '4', 4},
  {L"5", L"%", '5', 4}, {L"6", L"^", '6', 4}, {L"7", L"&", '7', 4}, {L"8", L"*", '8', 4},
  {L"9", L"(", '9', 4}, {L"0", L")", '0', 4}, {L"-", L"_", VK_OEM_MINUS, 4}, {L"=", L"+", VK_OEM_PLUS, 4},
  {L"Bksp", NULL, VK_BACK, 7},
};
static const KEY mRow2[] = {
  {L"Tab", NULL, VK_TAB, 6},
  {L"q", L"Q", 'Q', 4}, {L"w", L"W", 'W', 4}, {L"e", L"E", 'E', 4}, {L"r", L"R", 'R', 4},
  {L"t", L"T", 'T', 4}, {L"y", L"Y", 'Y', 4}, {L"u", L"U", 'U', 4}, {L"i", L"I", 'I', 4},
  {L"o", L"O", 'O', 4}, {L"p", L"P", 'P', 4}, {L"[", L"{", VK_OEM_4, 4}, {L"]", L"}", VK_OEM_6, 4},
  {L"\\", L"|", VK_OEM_5, 6},
};
static const KEY mRow3[] = {
  {L"Ctrl", NULL, VK_CTRL_STICKY, 7},
  {L"a", L"A", 'A', 4}, {L"s", L"S", 'S', 4}, {L"d", L"D", 'D', 4}, {L"f", L"F", 'F', 4},
  {L"g", L"G", 'G', 4}, {L"h", L"H", 'H', 4}, {L"j", L"J", 'J', 4}, {L"k", L"K", 'K', 4},
  {L"l", L"L", 'L', 4}, {L";", L":", VK_OEM_1, 4}, {L"'", L"\"", VK_OEM_7, 4},
  {L"Enter", NULL, VK_RETURN, 9},
};
static const KEY mRow4[] = {
  {L"Shift", NULL, VK_SHIFT_STICKY, 9},
  {L"z", L"Z", 'Z', 4}, {L"x", L"X", 'X', 4}, {L"c", L"C", 'C', 4}, {L"v", L"V", 'V', 4},
  {L"b", L"B", 'B', 4}, {L"n", L"N", 'N', 4}, {L"m", L"M", 'M', 4},
  {L",", L"<", VK_OEM_COMMA, 4}, {L".", L">", VK_OEM_PERIOD, 4}, {L"/", L"?", VK_OEM_2, 4},
  {L"Up", NULL, VK_UP, 5}, {L"Del", NULL, VK_DELETE, 5},
};
static const KEY mRow5[] = {
  {L"Hide", NULL, VK_HIDE_OSK, 6}, {L"`", L"~", VK_OEM_3, 4}, {L"Home", NULL, VK_HOME, 5},
  {L"End", NULL, VK_END, 5}, {L"Space", NULL, VK_SPACE, 20},
  {L"Left", NULL, VK_LEFT, 5}, {L"Down", NULL, VK_DOWN, 5}, {L"Right", NULL, VK_RIGHT, 5},
};

static const struct { const KEY *Keys; int Count; } mRows[] = {
  {mRow1, ARRAYSIZE(mRow1)}, {mRow2, ARRAYSIZE(mRow2)}, {mRow3, ARRAYSIZE(mRow3)},
  {mRow4, ARRAYSIZE(mRow4)}, {mRow5, ARRAYSIZE(mRow5)},
};
#define ROWS ((int)ARRAYSIZE(mRows))

#define TIMER_TOPMOST 1
#define TIMER_UNLIGHT 2
#define TIMER_CONSOLE 3
#define TIMER_REPEAT  4

#define TAB_W 140
#define TAB_H 44

static BOOL   mShift, mCaps, mCtrl, mHidden;
static DWORD  mLastShiftTick;
static HFONT  mFont;
static int    mLitRow = -1, mLitKey = -1;
static int    mRepRow = -1, mRepKey = -1;
static int    mScreenW, mScreenH, mKbdH;
static DWORD  mShellPid;
static HWND   mConsole;
static HWND   mWnd;
static HANDLE mLog = INVALID_HANDLE_VALUE;

/* --- log ---------------------------------------------------------------- */

static void LogOpen(void)
{
  WCHAR Path[64];
  WCHAR Drive;

  for (Drive = L'C'; Drive <= L'Z' && mLog == INVALID_HANDLE_VALUE; Drive++) {
    wsprintfW(Path, L"%c:\\sources\\boot.wim", Drive);
    if (GetFileAttributesW(Path) == INVALID_FILE_ATTRIBUTES) continue;
    wsprintfW(Path, L"%c:\\goyaosk.log", Drive);
    mLog = CreateFileW(Path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
  }
  if (mLog == INVALID_HANDLE_VALUE) {
    mLog = CreateFileW(L"X:\\Windows\\Temp\\goyaosk.log", GENERIC_WRITE, FILE_SHARE_READ, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  }
}

static void Log(const char *Fmt, ...)
{
  char    Buf[256];
  DWORD   Len, Written;
  va_list Args;

  if (mLog == INVALID_HANDLE_VALUE) return;
  va_start(Args, Fmt);
  Len = (DWORD)wvsprintfA(Buf, Fmt, Args);
  va_end(Args);
  if (Len > sizeof(Buf) - 3) Len = sizeof(Buf) - 3;
  Buf[Len++] = '\r';
  Buf[Len++] = '\n';
  WriteFile(mLog, Buf, Len, &Written, NULL);
}

/* --- geometry ------------------------------------------------------------- */

static void Measure(void)
{
  mScreenW = GetSystemMetrics(SM_CXSCREEN);
  mScreenH = GetSystemMetrics(SM_CYSCREEN);
  /* 1024x600 landscape: 40 % of the height keeps the keys ~45 px tall */
  mKbdH = (mScreenH > mScreenW) ? mScreenH * 2 / 5 : mScreenH * 2 / 5;
  if (mKbdH < 5 * 36) mKbdH = 5 * 36;
}

static int ConsoleHeight(void) { return mHidden ? mScreenH - TAB_H : mScreenH - mKbdH; }

static void PlaceKeyboard(void)
{
  if (mWnd == NULL) return;
  if (mHidden) {
    SetWindowPos(mWnd, HWND_TOPMOST, mScreenW - TAB_W, mScreenH - TAB_H, TAB_W, TAB_H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  } else {
    SetWindowPos(mWnd, HWND_TOPMOST, 0, mScreenH - mKbdH, mScreenW, mKbdH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  }
}

/* --- the console of /shell mode -------------------------------------------- */

static void DisableQuickEdit(void)
{
  HKEY  Key;
  DWORD Zero = 0, One = 1;

  if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Console", 0, NULL, 0, KEY_SET_VALUE, NULL, &Key, NULL) == ERROR_SUCCESS) {
    RegSetValueExW(Key, L"QuickEdit", 0, REG_DWORD, (const BYTE *)&Zero, sizeof(Zero));
    RegSetValueExW(Key, L"InsertMode", 0, REG_DWORD, (const BYTE *)&One, sizeof(One));
    RegCloseKey(Key);
    Log("QuickEdit off");
  }
}

static BOOL StartShell(void)
{
  STARTUPINFOW        Si;
  PROCESS_INFORMATION Pi;
  WCHAR               Cmd[] = L"X:\\Windows\\System32\\cmd.exe /k X:\\Windows\\System32\\startnet.cmd";

  ZeroMemory(&Si, sizeof(Si));
  ZeroMemory(&Pi, sizeof(Pi));
  Si.cb      = sizeof(Si);
  Si.dwFlags = STARTF_USEPOSITION | STARTF_USESIZE;
  Si.dwXSize = (DWORD)mScreenW;
  Si.dwYSize = (DWORD)ConsoleHeight();

  if (!CreateProcessW(NULL, Cmd, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, L"X:\\Windows\\System32", &Si, &Pi)) {
    Log("CreateProcess(cmd) failed: %lu", GetLastError());
    return FALSE;
  }
  mShellPid = Pi.dwProcessId;
  CloseHandle(Pi.hThread);
  CloseHandle(Pi.hProcess);
  Log("cmd started, pid %lu", mShellPid);
  return TRUE;
}

static BOOL CALLBACK FindConsoleProc(HWND Wnd, LPARAM Param)
{
  WCHAR Class[32];
  DWORD Pid = 0;

  if (!IsWindowVisible(Wnd) || !GetClassNameW(Wnd, Class, ARRAYSIZE(Class))) return TRUE;
  if (lstrcmpW(Class, L"ConsoleWindowClass") != 0) return TRUE;
  GetWindowThreadProcessId(Wnd, &Pid);
  if (mShellPid == 0 || Pid == mShellPid || *(HWND *)Param == NULL) {
    *(HWND *)Param = Wnd;
    if (mShellPid == 0 || Pid == mShellPid) return FALSE;
  }
  return TRUE;
}

static void PlaceConsole(void)
{
  if (mConsole != NULL && IsWindow(mConsole)) {
    SetWindowPos(mConsole, HWND_NOTOPMOST, 0, 0, mScreenW, ConsoleHeight(), SWP_NOACTIVATE);
  }
}

static HWND FindConsole(void)
{
  HWND Found = NULL;

  if (mConsole != NULL && IsWindow(mConsole)) return mConsole;
  EnumWindows(FindConsoleProc, (LPARAM)&Found);
  if (Found != NULL) {
    mConsole = Found;
    PlaceConsole();
    Log("console window found");
  }
  return Found;
}

/* keys go to the foreground window: make sure that is not the keyboard itself */
static void FocusTarget(void)
{
  HWND  Fg = GetForegroundWindow();
  HWND  Con;
  WCHAR Class[16];

  if (Fg != NULL && Fg != GetDesktopWindow() && Fg != mWnd &&
      GetClassNameW(Fg, Class, ARRAYSIZE(Class)) && lstrcmpW(Class, L"GoyaOsk") != 0) {
    return;
  }
  Con = FindConsole();
  if (Con != NULL) SetForegroundWindow(Con);
}

/* --- keyboard ----------------------------------------------------------- */

static int RowUnits(int Row)
{
  int U = 0, I;
  for (I = 0; I < mRows[Row].Count; I++) U += mRows[Row].Keys[I].Width;
  return U;
}

static void KeyRect(HWND Wnd, int Row, int Index, RECT *R)
{
  RECT C;
  int  Units = RowUnits(Row), Before = 0, I, W;

  GetClientRect(Wnd, &C);
  W = C.right - C.left;
  for (I = 0; I < Index; I++) Before += mRows[Row].Keys[I].Width;
  R->left   = C.left + MulDiv(W, Before, Units) + 2;
  R->right  = C.left + MulDiv(W, Before + mRows[Row].Keys[Index].Width, Units) - 2;
  R->top    = MulDiv(C.bottom - C.top, Row, ROWS) + 2;
  R->bottom = MulDiv(C.bottom - C.top, Row + 1, ROWS) - 2;
}

static BOOL HitKey(HWND Wnd, POINT P, int *Row, int *Index)
{
  RECT R;
  int  Ro, I;
  for (Ro = 0; Ro < ROWS; Ro++) {
    for (I = 0; I < mRows[Ro].Count; I++) {
      KeyRect(Wnd, Ro, I, &R);
      if (PtInRect(&R, P)) { *Row = Ro; *Index = I; return TRUE; }
    }
  }
  return FALSE;
}

static BOOL IsExtended(WORD Vk)
{
  switch (Vk) {
  case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
  case VK_HOME: case VK_END: case VK_DELETE: case VK_INSERT: case VK_PRIOR: case VK_NEXT:
    return TRUE;
  }
  return FALSE;
}

static BOOL IsRepeating(WORD Vk)
{
  return Vk == VK_BACK || Vk == VK_DELETE || Vk == VK_LEFT || Vk == VK_RIGHT || Vk == VK_UP || Vk == VK_DOWN;
}

static void SendVk(WORD Vk, BOOL Up)
{
  INPUT In;

  ZeroMemory(&In, sizeof(In));
  In.type       = INPUT_KEYBOARD;
  In.ki.wVk     = Vk;
  In.ki.wScan   = (WORD)MapVirtualKeyW(Vk, MAPVK_VK_TO_VSC);
  In.ki.dwFlags = (Up ? KEYEVENTF_KEYUP : 0) | (IsExtended(Vk) ? KEYEVENTF_EXTENDEDKEY : 0);
  SendInput(1, &In, sizeof(In));
}

static void SendChar(WCHAR Ch)
{
  INPUT In[2];

  ZeroMemory(In, sizeof(In));
  In[0].type       = INPUT_KEYBOARD;
  In[0].ki.wScan   = Ch;
  In[0].ki.dwFlags = KEYEVENTF_UNICODE;
  In[1].type       = INPUT_KEYBOARD;
  In[1].ki.wScan   = Ch;
  In[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
  SendInput(2, In, sizeof(INPUT));
}

static BOOL IsCharKey(const KEY *K)
{
  return K->Shifted != NULL && K->Label[0] != 0 && K->Label[1] == 0;
}

static BOOL UpperCase(const KEY *K)
{
  BOOL Letter = K->Label[0] >= L'a' && K->Label[0] <= L'z';
  return mShift ^ (mCaps && Letter);
}

/* send one key with the current modifiers (no state change) */
static void Emit(const KEY *K)
{
  FocusTarget();
  if (IsCharKey(K) && !mCtrl) {
    SendChar(UpperCase(K) ? K->Shifted[0] : K->Label[0]);
    return;
  }
  if (mCtrl)  SendVk(VK_CONTROL, FALSE);
  if (mShift) SendVk(VK_SHIFT, FALSE);
  SendVk(K->Vk, FALSE);
  SendVk(K->Vk, TRUE);
  if (mShift) SendVk(VK_SHIFT, TRUE);
  if (mCtrl)  SendVk(VK_CONTROL, TRUE);
}

static void SetHidden(BOOL Hide)
{
  mHidden = Hide;
  PlaceKeyboard();
  PlaceConsole();
  InvalidateRect(mWnd, NULL, TRUE);
  Log(Hide ? "keyboard hidden" : "keyboard shown");
}

static void Press(HWND Wnd, int Row, int Index)
{
  const KEY *K = &mRows[Row].Keys[Index];

  mLitRow = Row;
  mLitKey = Index;
  SetTimer(Wnd, TIMER_UNLIGHT, 150, NULL);

  if (K->Vk == VK_SHIFT_STICKY) {
    DWORD Now = GetTickCount();
    if (Now - mLastShiftTick < 400) { mCaps = !mCaps; mShift = FALSE; }   /* double tap */
    else mShift = !mShift;
    mLastShiftTick = Now;
  } else if (K->Vk == VK_CTRL_STICKY) {
    mCtrl = !mCtrl;
  } else if (K->Vk == VK_HIDE_OSK) {
    SetHidden(TRUE);
    return;
  } else {
    Emit(K);
    if (IsRepeating(K->Vk) && !mCtrl && !mShift) {
      mRepRow = Row;
      mRepKey = Index;
      SetTimer(Wnd, TIMER_REPEAT, 450, NULL);
    }
    mShift = FALSE;
    mCtrl  = FALSE;
  }
  InvalidateRect(Wnd, NULL, FALSE);
}

static void Release(HWND Wnd)
{
  KillTimer(Wnd, TIMER_REPEAT);
  mRepRow = mRepKey = -1;
}

static void Tap(HWND Wnd, POINT P)
{
  int Row, I;

  if (mHidden) { SetHidden(FALSE); return; }
  if (HitKey(Wnd, P, &Row, &I)) Press(Wnd, Row, I);
}

static void Paint(HWND Wnd)
{
  PAINTSTRUCT Ps;
  HDC         Dc = BeginPaint(Wnd, &Ps);
  RECT        C, R;
  HBRUSH      Back = CreateSolidBrush(RGB(28, 28, 30));
  HBRUSH      Key  = CreateSolidBrush(RGB(68, 68, 72));
  HBRUSH      Mod  = CreateSolidBrush(RGB(48, 48, 52));
  HBRUSH      On   = CreateSolidBrush(RGB(0, 120, 215));
  HBRUSH      Hit  = CreateSolidBrush(RGB(160, 160, 165));
  HGDIOBJ     OldFont = SelectObject(Dc, mFont);
  int         Row, I;

  GetClientRect(Wnd, &C);
  FillRect(Dc, &C, Back);
  SetBkMode(Dc, TRANSPARENT);
  SetTextColor(Dc, RGB(240, 240, 240));

  if (mHidden) {
    InflateRect(&C, -3, -3);
    FillRect(Dc, &C, On);
    DrawTextW(Dc, L"Keyboard", -1, &C, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  } else {
    for (Row = 0; Row < ROWS; Row++) {
      for (I = 0; I < mRows[Row].Count; I++) {
        const KEY *K = &mRows[Row].Keys[I];
        BOOL Sticky  = (K->Vk == VK_SHIFT_STICKY && (mShift || mCaps)) || (K->Vk == VK_CTRL_STICKY && mCtrl);
        BOOL Pressed = (Row == mLitRow && I == mLitKey) || (Row == mRepRow && I == mRepKey);
        const WCHAR *Text = K->Label;

        if (IsCharKey(K) && UpperCase(K)) Text = K->Shifted;
        if (K->Vk == VK_SHIFT_STICKY && mCaps) Text = L"CAPS";
        KeyRect(Wnd, Row, I, &R);
        FillRect(Dc, &R, Pressed ? Hit : (Sticky ? On : (IsCharKey(K) || K->Vk == VK_SPACE ? Key : Mod)));
        DrawTextW(Dc, Text, -1, &R, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      }
    }
  }

  SelectObject(Dc, OldFont);
  DeleteObject(Back); DeleteObject(Key); DeleteObject(Mod); DeleteObject(On); DeleteObject(Hit);
  EndPaint(Wnd, &Ps);
}

static LRESULT CALLBACK WndProc(HWND Wnd, UINT Msg, WPARAM W, LPARAM L)
{
  POINT P;

  switch (Msg) {
  case WM_MOUSEACTIVATE:
    return MA_NOACTIVATE;
  case WM_POINTERACTIVATE:
    return PA_NOACTIVATE;

  case WM_POINTERDOWN:      /* touch (Windows 8): screen coordinates; no mouse promotion */
    P.x = (short)LOWORD(L);
    P.y = (short)HIWORD(L);
    ScreenToClient(Wnd, &P);
    Tap(Wnd, P);
    return 0;
  case WM_POINTERUP:
    Release(Wnd);
    return 0;

  case WM_LBUTTONDOWN:
  case WM_LBUTTONDBLCLK:
  case WM_RBUTTONDOWN:      /* a tap is a tap, whatever the digitizer reports */
  case WM_RBUTTONDBLCLK:
    P.x = (short)LOWORD(L);
    P.y = (short)HIWORD(L);
    Tap(Wnd, P);
    return 0;
  case WM_LBUTTONUP:
  case WM_RBUTTONUP:
    Release(Wnd);
    return 0;
  case WM_CONTEXTMENU:
    return 0;

  case WM_TIMER:
    if (W == TIMER_TOPMOST) {
      PlaceKeyboard();
    } else if (W == TIMER_UNLIGHT) {
      KillTimer(Wnd, TIMER_UNLIGHT);
      mLitRow = mLitKey = -1;
      InvalidateRect(Wnd, NULL, FALSE);
    } else if (W == TIMER_REPEAT) {
      if (mRepRow >= 0) {
        Emit(&mRows[mRepRow].Keys[mRepKey]);
        SetTimer(Wnd, TIMER_REPEAT, 60, NULL);
      }
    } else if (W == TIMER_CONSOLE) {
      if (FindConsole() != NULL) {
        KillTimer(Wnd, TIMER_CONSOLE);
        SetForegroundWindow(mConsole);
      }
    }
    return 0;

  case WM_DISPLAYCHANGE:
    Measure();
    PlaceKeyboard();
    PlaceConsole();
    return 0;

  case WM_ERASEBKGND:
    return 1;
  case WM_PAINT:
    Paint(Wnd);
    return 0;
  case WM_CLOSE:
    return 0;               /* Alt+F4 must not kill the WinPE shell */
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(Wnd, Msg, W, L);
}

static BOOL HasArg(const WCHAR *Arg)
{
  const WCHAR *C = GetCommandLineW();
  int          N = lstrlenW(Arg);

  for ( ; *C; C++) {
    if (CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, C, N, Arg, N) == CSTR_EQUAL) return TRUE;
  }
  return FALSE;
}

static void Forever(void)
{
  for ( ; ; ) Sleep(INFINITE);
}

void WINAPI WinMainCRTStartup(void)
{
  HINSTANCE Inst  = GetModuleHandleW(NULL);
  BOOL      Shell = HasArg(L"/shell");
  WNDCLASSW Wc;
  MSG       Msg;

  LogOpen();
  Log("goyaosk 1.0 start%s", Shell ? " (shell)" : "");
  if (HasArg(L"/probe")) {
    Log("probe ok");
    ExitProcess(0x5050);
  }

  Measure();
  Log("screen %dx%d, keyboard height %d", mScreenW, mScreenH, mKbdH);
  if (Shell) DisableQuickEdit();

  ZeroMemory(&Wc, sizeof(Wc));
  Wc.lpfnWndProc   = WndProc;
  Wc.hInstance     = Inst;
  Wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
  Wc.lpszClassName = L"GoyaOsk";
  if (!RegisterClassW(&Wc)) Log("RegisterClass failed: %lu", GetLastError());

  mFont = CreateFontW(-(mKbdH / ROWS * 2 / 5), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  if (mFont == NULL) {
    mFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    Log("CreateFont failed, using DEFAULT_GUI_FONT");
  }

  mWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"GoyaOsk", L"Keyboard",
                         WS_POPUP | WS_VISIBLE, 0, mScreenH - mKbdH, mScreenW, mKbdH, NULL, NULL, Inst, NULL);
  if (mWnd == NULL) {
    Log("CreateWindow failed: %lu", GetLastError());
  } else {
    Log("keyboard window up");
    SetTimer(mWnd, TIMER_TOPMOST, 2000, NULL);
  }

  if (Shell) {
    if (!StartShell()) {
      Log("no shell, exiting");      /* winpeshl.ini's next entry (plain cmd) takes over */
      ExitProcess(1);
    }
    if (mWnd != NULL) SetTimer(mWnd, TIMER_CONSOLE, 300, NULL);
  }

  if (mWnd == NULL) Forever();       /* /shell must never exit; plain mode: nothing to do */

  while (GetMessageW(&Msg, NULL, 0, 0) > 0) {
    TranslateMessage(&Msg);
    DispatchMessageW(&Msg);
  }
  if (Shell) Forever();
  ExitProcess(0);
}
