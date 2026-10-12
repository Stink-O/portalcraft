#!/usr/bin/env python3
"""Find the player's Portal 2 install. Same rules as portal2_dir.sh.

    from portal2_dir import portal2_dir      # raises SystemExit with a clear message if not found
    python3 portal2_dir.py                   # prints the path

$PORTAL2_DIR if set, else Steam's libraryfolders.vdf (native, ~/.local/share/Steam, Flatpak) for the library holding appid 620.
"""
import os, re, sys

def _valid(d):
    return os.path.isdir(os.path.join(d, "portal2")) and (
        os.path.exists(os.path.join(d, "portal2_linux")) or os.path.isdir(os.path.join(d, "bin")))

def find_portal2_dir():
    """Returns (path, None) or (None, message)."""
    env = os.environ.get("PORTAL2_DIR")
    if env:
        if _valid(env):
            return env.rstrip("/"), None
        return None, "PORTAL2_DIR=%s is set but does not look like a Portal 2 folder (no portal2/ inside it)." % env
    home = os.path.expanduser("~")
    roots = [home + "/.steam/steam/steamapps", home + "/.local/share/Steam/steamapps",
             home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps"]
    libs = []
    for sa in roots:
        if not os.path.isdir(sa):
            continue
        libs.append(os.path.dirname(sa))
        try:
            text = open(sa + "/libraryfolders.vdf", errors="replace").read()
        except OSError:
            continue
        for m in re.finditer(r'"(path|\d+)"\s+"([^"]*)"', text):
            if m.group(1) == "path" or m.group(2).startswith("/"):
                libs.append(m.group(2).replace("\\\\", "\\"))
    seen, tried = set(), []
    for lib in libs:
        lib = os.path.realpath(lib)
        if lib in seen or not os.path.isdir(lib):
            continue
        seen.add(lib); tried.append(lib)
        inst = "Portal 2"
        try:
            m = re.search(r'"installdir"\s+"([^"]*)"', open(lib + "/steamapps/appmanifest_620.acf", errors="replace").read())
            if m and m.group(1):
                inst = m.group(1)
        except OSError:
            pass
        cand = "%s/steamapps/common/%s" % (lib, inst)
        if _valid(cand):
            return cand, None
    msg = ("could not find Portal 2 (Steam app 620). Install it in Steam, start it once, or set PORTAL2_DIR to the folder\n"
           "  that contains portal2_linux (Steam: right-click Portal 2, Manage, Browse local files).\n")
    msg += ("  Steam libraries checked: " + " ".join(tried)) if tried else "  No Steam library was found under ~/.steam, ~/.local/share/Steam or the Flatpak path."
    return None, msg

def portal2_dir():
    p, err = find_portal2_dir()
    if p is None:
        sys.exit("portal2_dir: " + err)
    return p

if __name__ == "__main__":
    print(portal2_dir())
