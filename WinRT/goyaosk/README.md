# goyaosk - on-screen keyboard for WinPE on the Galaxy Tab 3 Lite (goyawifi)

A touch keyboard for WinPE (Windows 8 / RT, ARM32) on edk2-goyawifi, modelled on
[rossaosk](https://github.com/notnoelchannel2/rossaosk). WinPE has no osk.exe or TabTip, so this small
Win32 program (no C runtime, ~14 KB) docks a keyboard at the bottom of the 1024x600 screen and types into the
window in front with `SendInput`.

- `goyaosk.exe /shell`: WinPE shell from `winpeshl.ini`; starts `cmd /k startnet.cmd` above the keyboard, never exits.
- `goyaosk.exe`: keyboard only, for whatever window is in front. `/probe` exits with 0x5050.
- Touch through Windows 8 pointer messages (goyatouch) or mouse clicks; the window never takes focus.
- Unicode characters for printable keys; real key presses for Ctrl, Esc, Tab, Enter, Home/End, Del, arrows.
- Sticky Shift and Ctrl; double-tap Shift = Caps lock. Backspace, Del and arrows repeat while held.
- `Hide` folds it into a "Keyboard" tab in the bottom-right corner (tap to restore); the console resizes.
- QuickEdit off in the console. Log: `goyaosk.log` next to `\sources\boot.wim` on the card.

Build (Linux/WSL): `bash build.sh` (clang, lld, llvm, mingw-w64-common) -> `out/goyaosk.exe`, ARM32 Thumb-2 for the
Cortex-A9 (no hardware divide; MulDiv instead), subsystem 6.02, load config, `.pdata`. CI builds it and signs it with
the Goya test certificate (goya-drivers-signed artifact / signed-drivers branch). `bash build.sh --sign` signs locally
with a throwaway certificate instead. Needs `testsigning on`.

`WinRT/winpe/make-goya-sd.cmd` installs it into boot.wim and writes `winpeshl.ini` when `goyaosk.exe` is next to
the script or in `..\drivers`.
