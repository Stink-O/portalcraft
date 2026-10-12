#!/usr/bin/env python3
"""bridge #78: carry the user's campaign player from the mirror world into the unified (terrain) world, once.

    python3 -I bridge/tools/world78/migrate.py [--dry-run] [--force]

Until #78 the campaign (`play_clip.sh --mc`) ran the void mirror world (save SkyCraft); from #78 every session runs the terrain world
(the player's save "Portalcraft"). The player's inventory, ender chest, armour, XP, health and food are per save, so the campaign player would
come back with the pit-flings player's things. This copies them from the mirror save's player into the terrain save's player:
  * both saves are backed up first, whole (cache/bridge/world_backups/<time>-migrate78/), and nothing is ever deleted;
  * the terrain player's own file is kept next to it (players/data/<uuid>.dat.pre78), its place (Pos, Rotation, Dimension, Motion,
    respawn) is kept (the mirror world's place means nothing in the terrain world);
  * the mirror save is not touched (it stays the fallback world: play_clip.sh --mc --world mirror);
  * the blocks placed in the mirror world stay in the mirror save: every map's chamber was at the same Minecraft place there, so which
    block belongs to which map is not known (that mixing is the #61/#78 leak itself);
  * a marker (portalcraft_chambers/migrated78.txt in the terrain save) makes a second run refuse unless --force.
Refuses while any Minecraft (Fabric) runs: the saves must be closed.
"""
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nbt  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import world_conf  # noqa: E402  (bridge #81: the save's name: Portalcraft, or Portalcraft_<PORTALCRAFT_SEED>)

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
SAVES = os.environ.get("PCB_SAVES") or os.path.join(ROOT, "bridge", "fabric", "run", "saves")  # (PCB_SAVES: a copy, for a dry test)
MIRROR, TERRAIN = os.path.join(SAVES, "Portalcraft Mirror"), os.path.join(SAVES, world_conf.SAVE_NAME)
CARRY = ["Inventory", "EnderItems", "equipment", "XpLevel", "XpP", "XpTotal", "XpSeed", "Score", "Health", "AbsorptionAmount", "foodLevel",
         "foodSaturationLevel", "foodExhaustionLevel", "foodTickTimer", "SelectedItemSlot", "active_effects", "attributes", "recipeBook", "Tags",
         "seenCredits"]


def mc_running():
    out = subprocess.run(["pgrep", "-x", "java", "-a"], capture_output=True, text=True).stdout
    return [l.split()[0] for l in out.splitlines() if "fabric.dli" in l]


def main():
    dry = "--dry-run" in sys.argv
    force = "--force" in sys.argv
    pids = [] if os.environ.get("PCB_SAVES") else mc_running()
    if pids:
        sys.exit("a Minecraft is running (pid %s): close it first (the saves must not be open)" % " ".join(pids))
    marker = os.path.join(TERRAIN, "portalcraft_chambers", "migrated78.txt")
    if os.path.exists(marker) and not force:
        sys.exit("already migrated (%s); --force to run again" % marker)
    mdir, tdir = os.path.join(MIRROR, "players", "data"), os.path.join(TERRAIN, "players", "data")
    players = [f for f in os.listdir(mdir) if f.endswith(".dat")] if os.path.isdir(mdir) else []
    if not players:
        # (bridge #81) no mirror save (a fresh world, the old saves moved away): nothing to carry over, and that is not an error. Quiet; the marker
        # (only once the terrain save exists: its directory is Minecraft's to create) stops the next launch from asking again.
        if os.path.isdir(TERRAIN) and not dry:
            os.makedirs(os.path.dirname(marker), exist_ok=True)
            with open(marker, "w") as fh:
                fh.write("bridge #81: no mirror save player to migrate (%s); nothing carried over\n" % time.strftime("%Y-%m-%d-%H%M%S"))
        if "--verbose" in sys.argv:
            print("no player in the mirror save (%s): nothing to migrate" % mdir)
        return
    stamp = time.strftime("%Y-%m-%d-%H%M%S")
    backup = os.path.join(os.environ["PCB_SAVES"] + "_backups" if os.environ.get("PCB_SAVES") else os.path.join(ROOT, "cache", "bridge", "world_backups"), stamp + "-migrate78")
    report = []
    for f in players:
        src, dst = os.path.join(mdir, f), os.path.join(tdir, f)
        _, (mt, m) = nbt.load(src)
        if os.path.exists(dst):
            name, (tt, t) = nbt.load(dst)
        else:
            name, tt, t = "", nbt.COMPOUND, {}
        merged = dict(t)
        for k in CARRY:
            if k in m:
                merged[k] = m[k]
        inv = len(m.get("Inventory", (0, (0, [])))[1][1])
        report.append("%s: %d inventory stacks, XP level %s, from the mirror save; place kept from the terrain save (%s)" % (
            f, inv, m.get("XpLevel", (0, "?"))[1], "Pos " + " ".join("%.1f" % v for v in t["Pos"][1][1]) if "Pos" in t else "no terrain player yet"))
        if dry:
            continue
        if not os.path.isdir(backup):
            os.makedirs(backup)
            for s in (MIRROR, TERRAIN):
                shutil.copytree(s, os.path.join(backup, os.path.basename(s)), symlinks=True)
        if os.path.exists(dst):
            shutil.copy2(dst, dst + ".pre78")
        os.makedirs(tdir, exist_ok=True)
        tmp = dst + ".tmp78"
        with open(tmp, "wb") as fh:
            fh.write(nbt.dumps(name, (tt, merged)))
        nbt.load(tmp)  # (it reads back)
        os.replace(tmp, dst)
    print("\n".join(report))
    if dry:
        print("dry run: nothing written")
        return
    os.makedirs(os.path.dirname(marker), exist_ok=True)
    with open(marker, "w") as fh:
        fh.write("bridge #78 migration %s: the mirror save's player carried into this save (backup %s)\n%s\n" % (stamp, backup, "\n".join(report)))
    print("backup: %s" % backup)
    print("done")


if __name__ == "__main__":
    main()
