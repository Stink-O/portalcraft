// Stand-in for prismlauncher.exe in the wine tests: --launch copies fake-client.jar into Prism's libraries folder and starts
// a stand-in javaw.exe with the JvmArgs from instance.cfg; no arguments just stays open for a few seconds.
#include <windows.h>
#include <stdio.h>
#include <string.h>
static void mkdirs(char *p) { for (char *q = p + 3; *q; q++) if (*q == '\\') { *q = 0; CreateDirectoryA(p, 0); *q = '\\'; } CreateDirectoryA(p, 0); }
int main(int argc, char **argv) {
  char d[MAX_PATH]; GetModuleFileNameA(NULL, d, MAX_PATH); *strrchr(d, '\\') = 0;
  char logp[MAX_PATH + 32]; snprintf(logp, sizeof logp, "%s\\fakeprism.log", d);
  FILE *lg = fopen(logp, "a"); if (lg) { for (int i = 0; i < argc; i++) fprintf(lg, "%s ", argv[i]); fprintf(lg, "\n"); fclose(lg); }
  if (argc > 1 && !strcmp(argv[1], "--launch")) {
    Sleep(1500);
    char dir[MAX_PATH * 2], src[MAX_PATH * 2], dst[MAX_PATH * 2];
    snprintf(dir, sizeof dir, "%s\\libraries\\com\\mojang\\minecraft\\26.3", d); mkdirs(dir);
    snprintf(src, sizeof src, "%s\\fake-client.jar", d); snprintf(dst, sizeof dst, "%s\\minecraft-26.3-client.jar", dir);
    CopyFileA(src, dst, FALSE);
    Sleep(1000);
    char cfg[MAX_PATH * 2]; snprintf(cfg, sizeof cfg, "%s\\instances\\Portalcraft\\instance.cfg", d);
    char args[8192] = ""; FILE *f = fopen(cfg, "r"); char line[8192];
    while (f && fgets(line, sizeof line, f)) if (!strncmp(line, "JvmArgs=", 8)) { strncpy(args, line + 8, sizeof args - 1); }
    if (f) fclose(f);
    char *w = args; for (char *r = args; *r; r++) if (*r != '"' && *r != '\r' && *r != '\n') *w++ = *r; *w = 0;
    char cmd[16384]; snprintf(cmd, sizeof cmd, "\"%s\\javaw.exe\" %s", d, args);
    STARTUPINFOA si = { sizeof si }; PROCESS_INFORMATION pi;
    CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, d, &si, &pi);
    return 0;
  }
  Sleep(8000);
  return 0;
}
