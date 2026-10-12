"""bridge #81: bridge/world.conf defaults and the mod's world report for the python tools.

    import world_conf; world_conf.SAVE_NAME (the save this launch uses), world_conf.report() (dict of the open save's SEED, EXIT_W*, ... or None)
"""
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def _kv(path):
    kv = {}
    with open(path) as fh:
        for line in fh:
            line = line.split("#", 1)[0].strip()
            if "=" in line:
                k, v = line.split("=", 1)
                kv[k.strip()] = v.strip().strip("\"'")
    return kv


DEFAULTS = _kv(os.environ.get("PCB_WORLD_CONF") or os.path.join(ROOT, "bridge", "world.conf"))
SEED_FORCED = os.environ.get("PORTALCRAFT_SEED", "")
SAVE_NAME = "Portalcraft" + ("_" + SEED_FORCED if SEED_FORCED else "")


def report():
    p = os.environ.get("PCB_WORLD_ENV") or os.path.join(ROOT, "cache", "bridge", "portalcraft_world.env")
    return _kv(p) if os.path.exists(p) else None
