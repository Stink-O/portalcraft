#!/usr/bin/env python3
"""Builds the release folder (default: dist/) with launcher.json and every file it points at.

    launcher/tools/make_release.py --launcher target-launcher/release/portalcraft-launcher \
        --plugin bridge/build/release/portalcraft_bridge.so --mod bridge/fabric/build/libs/portalcraft-0.1.0.jar \
        [--out dist] [--release 0.1.0] [--base-url https://github.com/Stink-O/portalcraft/releases/download/v0.1.0] [--check-pins]

Windows (optional; schema stays 1, the Windows files go in the manifest's "platforms" map):
        --launcher-win target-launcher-win/x86_64-pc-windows-gnu/release/portalcraft-launcher.exe [--plugin-win portalcraft_bridge.dll]
    Without --plugin-win the Windows launcher sets everything up and says "no Windows plugin in this release yet" on Play.

Writes into --out:
    portalcraft-launcher-linux-x86_64    the launcher (what players download)
    portalcraft-launcher-windows-x86_64.exe   the Windows launcher (with --launcher-win)
    portalcraft_bridge.dll               the Windows Portal 2 plugin (with --plugin-win; does not exist yet)
    portalcraft_bridge.so                the 32-bit Portal 2 plugin
    portalcraft-<modversion>.jar         the Fabric mod
    instance-template.tar.gz             instance.cfg.in + mmc-pack.json (Prism profile template)
    map-tools.tar.gz                     the optional map-building scripts
    launcher.json                        the manifest (sha256 + size of everything above, plus the pinned Prism and Fabric API)

Upload every file in --out to the GitHub release (the manifest's "asset:" urls resolve next to launcher.json).
Python 3 standard library only. Nothing here touches Valve or Mojang files.
"""
import argparse, hashlib, io, json, os, re, shutil, sys, tarfile, urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
L = os.path.join(ROOT, "launcher")



def copy_replace(src, dst):
    """Copy via a temp file and rename, so a launcher that is running from dst does not block the copy."""
    tmp = dst + ".tmp"
    shutil.copyfile(src, tmp)
    shutil.copymode(src, tmp)
    os.replace(tmp, dst)

def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def entry(version, url, path):
    return {"version": version, "url": url, "sha256": sha256(path), "size": os.path.getsize(path)}


def tar_gz(out, files):
    """files: [(arcname, source path)]. Deterministic (sorted, no timestamps/owners)."""
    with open(out, "wb") as raw:
        import gzip
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as gz:
            with tarfile.open(fileobj=gz, mode="w", format=tarfile.GNU_FORMAT) as t:
                for arc, src in sorted(files):
                    ti = t.gettarinfo(src, arcname=arc)
                    ti.uid = ti.gid = 0; ti.uname = ti.gname = ""; ti.mtime = 0
                    ti.mode = 0o755 if arc.endswith(".sh") else 0o644
                    with open(src, "rb") as f:
                        t.addfile(ti, f)


def check_pin(name, pin):
    print("checking pin %s (downloading %.1f MB)..." % (name, pin["size"] / 1e6))
    tmp = os.path.join(L, ".pin-check-" + name)
    urllib.request.urlretrieve(pin["url"], tmp)
    ok = sha256(tmp) == pin["sha256"] and os.path.getsize(tmp) == pin["size"]
    os.remove(tmp)
    if not ok:
        sys.exit("pin %s does not match its url" % name)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--launcher", required=True)
    ap.add_argument("--plugin", required=True)
    ap.add_argument("--mod", required=True)
    ap.add_argument("--out", default=os.path.join(ROOT, "dist"))
    ap.add_argument("--release", default=None, help="release name (default: the mod's version)")
    ap.add_argument("--launcher-win", default=None, help="the Windows launcher .exe (adds platforms.windows-x86_64)")
    ap.add_argument("--plugin-win", default=None, help="the Windows Portal 2 plugin .dll (needs --launcher-win)")
    ap.add_argument("--base-url", default=None, help="where the files will be; default: 'asset:' (next to launcher.json)")
    ap.add_argument("--check-pins", action="store_true", help="download the pinned Prism and Fabric API and verify them")
    a = ap.parse_args()

    pins = json.load(open(os.path.join(L, "pins.json")))
    if a.check_pins:
        for k in ("prism", "prism_windows", "fabric_api"):
            check_pin(k, pins[k])

    cargo = open(os.path.join(L, "Cargo.toml")).read()
    launcher_version = re.search(r'^version\s*=\s*"([^"]+)"', cargo, re.M).group(1)
    props = open(os.path.join(ROOT, "bridge/fabric/gradle.properties")).read()
    mod_version = re.search(r"^version=(.+)$", props, re.M).group(1).strip()
    release = a.release or mod_version
    jar_name = "portalcraft-%s.jar" % mod_version
    if os.path.basename(a.mod) != jar_name:
        print("note: the mod file is %s, expected %s; publishing it as %s" % (os.path.basename(a.mod), jar_name, jar_name))

    if a.plugin_win and not a.launcher_win:
        sys.exit("--plugin-win needs --launcher-win")
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    names = {"launcher": "portalcraft-launcher-linux-x86_64", "plugin": "portalcraft_bridge.so", "mod": jar_name,
             "instance": "instance-template.tar.gz", "map_tools": "map-tools.tar.gz",
             "launcher_win": "portalcraft-launcher-windows-x86_64.exe", "plugin_win": "portalcraft_bridge.dll"}
    copy_replace(a.launcher, os.path.join(out, names["launcher"]))
    os.chmod(os.path.join(out, names["launcher"]), 0o755)
    copy_replace(a.plugin, os.path.join(out, names["plugin"]))
    copy_replace(a.mod, os.path.join(out, names["mod"]))
    if a.launcher_win:
        copy_replace(a.launcher_win, os.path.join(out, names["launcher_win"]))
    if a.plugin_win:
        copy_replace(a.plugin_win, os.path.join(out, names["plugin_win"]))
    tar_gz(os.path.join(out, names["instance"]), [
        ("instance.cfg.in", os.path.join(L, "prism/instance/instance.cfg.in")),
        ("mmc-pack.json", os.path.join(L, "prism/instance/mmc-pack.json")),
    ])
    mt = ["add_region.py", "bsp.py", "bsp2vmf.py", "relight.py", "pack.py", "compile.sh", "build.sh", "pc_map_alias.nut"]
    tar_gz(os.path.join(out, names["map_tools"]),
           [("maps/tools/" + n, os.path.join(ROOT, "maps/tools", n)) for n in mt] +
           [("bridge/tools/portal2_dir." + e, os.path.join(ROOT, "bridge/tools/portal2_dir." + e)) for e in ("sh", "py")])

    def url(key):
        return ("%s/%s" % (a.base_url.rstrip("/"), names[key])) if a.base_url else "asset:" + names[key]

    manifest = {
        "schema": 1,
        "release": release,
        "launcher": entry(launcher_version, url("launcher"), os.path.join(out, names["launcher"])),
        "plugin": entry(release, url("plugin"), os.path.join(out, names["plugin"])),
        "mod": entry(mod_version, url("mod"), os.path.join(out, names["mod"])),
        "fabric_api": pins["fabric_api"],
        "prism": pins["prism"],
        "instance": entry("1", url("instance"), os.path.join(out, names["instance"])),
        "map_tools": entry(release, url("map_tools"), os.path.join(out, names["map_tools"])),
    }
    if a.launcher_win:
        # Everything that differs per system. The top level above stays exactly what schema 1 always meant (Linux), so
        # old Linux launchers read this manifest unchanged and ignore "platforms"; the Windows launcher reads only this.
        win = {"launcher": entry(launcher_version, url("launcher_win"), os.path.join(out, names["launcher_win"])),
               "prism": pins["prism_windows"]}
        if a.plugin_win:
            win["plugin"] = entry(release, url("plugin_win"), os.path.join(out, names["plugin_win"]))
        manifest["platforms"] = {"windows-x86_64": win}
    with open(os.path.join(out, "launcher.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print("wrote", os.path.join(out, "launcher.json"))
    for n in sorted(os.listdir(out)):
        print("  %-40s %12d bytes" % (n, os.path.getsize(os.path.join(out, n))))


if __name__ == "__main__":
    main()
