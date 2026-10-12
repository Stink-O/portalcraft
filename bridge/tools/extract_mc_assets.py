#!/usr/bin/env python3
"""Unpack Minecraft 26.3's textures from the player's own client jar.

The Portal 2 plugin draws Minecraft blocks with Minecraft's own textures and reads them from

    <repo>/cache/minecraft-26.3/resourcepacks/local/minecraft-26.3/assets/minecraft/textures/block/*.png

No Mojang file is ever committed to the repository: run this once on the player's machine, from
the Minecraft 26.3 client jar that the launcher (or Gradle/Loom) has already downloaded.

Usage:
    extract_mc_assets.py [JAR] [--out DIR] [--force] [--quiet]

JAR      the Minecraft 26.3 client jar. Without it the usual places are searched (Prism Launcher
         libraries/com/mojang/minecraft/26.3/, the official launcher and Flatpak versions/26.3/26.3.jar,
         the Gradle/Loom cache used by the development build). $MC_CLIENT_JAR and $PRISM_DIR also work.
--out    the directory that becomes assets/minecraft (default: the path above, under the repo root,
         which is $PORTALCRAFT_ROOT or the folder holding .portalcraft-root).
--force  unpack again even if the result is already there.

Python 3 standard library only. Exit status 0 on success, 1 when no jar is found or it is not a
Minecraft client jar.
"""
import argparse
import glob
import os
import sys
import zipfile

VERSION = "26.3"
PREFIX = "assets/minecraft/"
PROBE = PREFIX + "textures/block/dirt.png"
STAMP = ".portalcraft-extracted"


def repo_root():
    env = os.environ.get("PORTALCRAFT_ROOT")
    if env and os.path.isdir(env):
        return os.path.abspath(env)
    here = os.path.dirname(os.path.abspath(__file__))
    p = here
    for _ in range(6):
        if os.path.exists(os.path.join(p, ".portalcraft-root")):
            return p
        p = os.path.dirname(p)
    return os.path.dirname(os.path.dirname(here))  # bridge/tools -> repo root


def default_out():
    return os.path.join(repo_root(), "cache", "minecraft-" + VERSION, "resourcepacks", "local",
                        "minecraft-" + VERSION, "assets", "minecraft")


def candidates():
    home = os.path.expanduser("~")
    out = []
    env = os.environ.get("MC_CLIENT_JAR")
    if env:
        out.append(env)
    prism_dirs = [os.environ.get("PRISM_DIR")] if os.environ.get("PRISM_DIR") else []
    root = repo_root()
    prism_dirs += [
        os.path.join(root, "Prism"), os.path.join(root, "..", "Prism"),
        os.path.join(home, ".local/share/PrismLauncher"),
        os.path.join(home, ".var/app/org.prismlauncher.PrismLauncher/data/PrismLauncher"),
    ]
    for d in prism_dirs:
        out += sorted(glob.glob(os.path.join(d, "libraries/com/mojang/minecraft", VERSION, "*.jar")))
        out += sorted(glob.glob(os.path.join(d, "**/libraries/com/mojang/minecraft", VERSION, "*.jar"), recursive=True))
    for mc in (os.path.join(home, ".minecraft"), os.path.join(home, ".var/app/com.mojang.Minecraft/.minecraft")):
        out.append(os.path.join(mc, "versions", VERSION, VERSION + ".jar"))
    # development: Fabric Loom / Gradle caches
    out.append(os.path.join(home, ".gradle/caches/fabric-loom", VERSION, "minecraft-client.jar"))
    out.append(os.path.join(home, ".gradle/caches/neoformruntime/artifacts", "minecraft_%s_client.jar" % VERSION))
    out.append(os.path.join(root, "cache", "minecraft-" + VERSION, "client.jar"))
    seen, res = set(), []
    for c in out:
        c = os.path.abspath(c)
        if c not in seen and os.path.isfile(c):
            seen.add(c)
            res.append(c)
    return res


def is_client_jar(path):
    try:
        with zipfile.ZipFile(path) as z:
            return PROBE in z.namelist()
    except (zipfile.BadZipFile, OSError):
        return False


def extract(jar, out, quiet):
    out = os.path.abspath(out)
    n = 0
    with zipfile.ZipFile(jar) as z:
        for info in z.infolist():
            name = info.filename
            if not name.startswith(PREFIX) or name.endswith("/"):
                continue
            rel = name[len(PREFIX):]
            dest = os.path.abspath(os.path.join(out, rel))
            if not dest.startswith(out + os.sep):  # refuse paths that climb out of the target
                continue
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with z.open(info) as src, open(dest + ".part", "wb") as dst:
                while True:
                    chunk = src.read(1 << 20)
                    if not chunk:
                        break
                    dst.write(chunk)
            os.replace(dest + ".part", dest)
            n += 1
    with open(os.path.join(out, STAMP), "w") as f:
        f.write(os.path.basename(jar) + "\n")
    if not quiet:
        print("extracted %d files from %s into %s" % (n, jar, out))
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("jar", nargs="?", help="Minecraft %s client jar (default: search)" % VERSION)
    ap.add_argument("--out", default=None)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()
    out = a.out or default_out()

    if not a.force and os.path.exists(os.path.join(out, STAMP)) and os.path.exists(os.path.join(out, "textures/block/dirt.png")):
        if not a.quiet:
            print("already extracted: %s (use --force to redo)" % out)
        return 0

    if a.jar:
        jars = [a.jar]
    else:
        jars = candidates()
        if not jars:
            sys.stderr.write("No Minecraft %s client jar found. Start Minecraft %s once from the launcher, then run:\n"
                             "  %s /path/to/%s.jar\n" % (VERSION, VERSION, sys.argv[0], VERSION))
            return 1
    for jar in jars:
        if is_client_jar(jar):
            extract(jar, out, a.quiet)
            return 0
        if a.jar:
            sys.stderr.write("%s is not a Minecraft %s client jar (no %s)\n" % (jar, VERSION, PROBE))
            return 1
    sys.stderr.write("Found jars but none is a Minecraft client jar with assets: %s\n" % ", ".join(jars))
    return 1


if __name__ == "__main__":
    sys.exit(main())
