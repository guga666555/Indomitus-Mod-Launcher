/* out\test\dummy_old.dll: an empty dll that tools\parity.py P-5b copies under an old dll's name (gem_penfilter.dll) to drive the
 * core's guard in a load test of out\dll\gem_melee.dll (early: loaded before it, late: loaded after it). Does nothing. */
#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res) { (void)inst; (void)reason; (void)res; return TRUE; }
