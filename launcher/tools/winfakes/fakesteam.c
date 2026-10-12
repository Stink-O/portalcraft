// steam.exe stand-in: "steam://rungameid/620" starts the stand-in portal2.exe named by FAKE_P2_EXE.
#include <windows.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
  if (argc > 1 && strstr(argv[1], "rungameid/620") && getenv("FAKE_P2_EXE")) {
    char dir[MAX_PATH]; strcpy(dir, getenv("FAKE_P2_EXE")); *strrchr(dir, '\\') = 0;
    STARTUPINFOA si = { sizeof si }; PROCESS_INFORMATION pi;
    CreateProcessA(getenv("FAKE_P2_EXE"), NULL, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir, &si, &pi);
  }
  return 0;
}
