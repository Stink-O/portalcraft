// portal2.exe stand-in: reads portal2\cfg\autoexec.cfg like the game would, then writes what the bridge plugin would log.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
static void say(const char *path, const char *fmt, ...) {
  FILE *f = fopen(path, "a"); if (!f) return;
  va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fclose(f);
}
int main(void) {
  char d[MAX_PATH]; GetModuleFileNameA(NULL, d, MAX_PATH); *strrchr(d, '\\') = 0;
  char cfg[MAX_PATH * 2]; snprintf(cfg, sizeof cfg, "%s\\portal2\\cfg\\autoexec.cfg", d);
  Sleep(1500);
  FILE *f = fopen(cfg, "r"); char line[4096], plugin[4096] = "", map[256] = "";
  while (f && fgets(line, sizeof line, f)) {
    if (!strncmp(line, "plugin_load \"", 13)) { strcpy(plugin, line + 13); char *q = strchr(plugin, '"'); if (q) *q = 0; }
    if (!strncmp(line, "map ", 4)) { strcpy(map, line + 4); map[strcspn(map, "\r\n")] = 0; }
  }
  if (f) fclose(f);
  if (!plugin[0]) { Sleep(60000); return 1; }       // no plugin_load: a plain Portal 2
  // <data>/plugin/portalcraft_bridge.dll -> <data>/cache/bridge/bridge.log
  char *s = strrchr(plugin, '/'); if (s) *s = 0; s = strrchr(plugin, '/'); if (s) *s = 0;
  char logp[8192]; snprintf(logp, sizeof logp, "%s/cache/bridge/bridge.log", plugin);
  DWORD pid = GetCurrentProcessId();
  say(logp, "[bridge] link: Local\\Portalcraft_v1 created, 220 MB, host pid %lu\n", (unsigned long)pid);
  Sleep(500);
  say(logp, "[bridge] level init %s\n", map);
  Sleep(1500);
  say(logp, "[bridge] link up (Minecraft pid 1234)\n");
  say(logp, "[bridge] mode: Minecraft player at 0,0,0\n");
  Sleep(90000);
  return 0;
}
