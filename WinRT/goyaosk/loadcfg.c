/* Load config directory + security cookie for the CRT-less ARM32 exe (as in every Microsoft-built
   image; WinPE on ARM refuses CRT-less images without it, see rossaosk). */
#include <windows.h>

DWORD __security_cookie = 0xBB40E64E;

const IMAGE_LOAD_CONFIG_DIRECTORY32 _load_config_used = {
  .Size = sizeof(IMAGE_LOAD_CONFIG_DIRECTORY32),
  .SecurityCookie = (DWORD)&__security_cookie,
};
