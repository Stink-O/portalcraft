// Stand-in for Minecraft's javaw.exe in the wine tests: stays alive and uses a little processor time, so the launcher's
// "is it frozen?" check sees progress.
#include <windows.h>
int main(void) {
  for (int i = 0; i < 6000; i++) { DWORD t = GetTickCount(); while (GetTickCount() - t < 15) {} Sleep(35); }
  return 0;
}
