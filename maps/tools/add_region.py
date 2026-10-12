#!/usr/bin/env python3
"""Add the hidden Minecraft region (and the map-name alias script) to a VMF.

usage: add_region.py <in.vmf> <out.vmf>

The region is a sealed hollow box below the chamber. Its six walls use
TOOLS/TOOLSSKYBOX: they render as the map's 2D sky (sky_white), never as a
wall texture; they are not portalable and get no lightmaps; and they let the
map's own light_environment light the inside (sky ambient baked into the
region's leaf ambient cubes, plus the sun), which is what Chell, cubes and
the plugin's ComputeLighting sample there.

Side effect handled elsewhere: vrad's sky tests are ray casts against faces,
and some rays from the chamber escape through nodraw surfaces and find this
sky, which would brighten the chamber's baked lighting. relight.py replaces
the chamber's baked lighting with Valve's own, so this does not show.
See maps/README.md.
"""
import sys

# Interior bounds in Source units (x, y, z). Keep in sync with maps/README.md.
REGION_MIN = (-16128, -16128, -16128)
REGION_MAX = (16128, 16128, -5632)
WALL = 128
WALL_MATERIAL = "TOOLS/TOOLSSKYBOX"
ROOF_MATERIAL = WALL_MATERIAL



class Ids:
    def __init__(self, start):
        self.n = start

    def __call__(self):
        self.n += 1
        return self.n


def box_solid(ids, mn, mx, material):
    x0, y0, z0 = mn
    x1, y1, z1 = mx
    # (plane points, uaxis, vaxis) per face; points are clockwise seen from
    # outside, the order Hammer writes.
    faces = [
        ((x0, y1, z1), (x1, y1, z1), (x1, y0, z1), "[1 0 0 0] 0.25", "[0 -1 0 0] 0.25"),  # top
        ((x0, y0, z0), (x1, y0, z0), (x1, y1, z0), "[1 0 0 0] 0.25", "[0 -1 0 0] 0.25"),  # bottom
        ((x0, y1, z1), (x0, y0, z1), (x0, y0, z0), "[0 1 0 0] 0.25", "[0 0 -1 0] 0.25"),  # -x
        ((x1, y1, z0), (x1, y0, z0), (x1, y0, z1), "[0 1 0 0] 0.25", "[0 0 -1 0] 0.25"),  # +x
        ((x1, y1, z1), (x0, y1, z1), (x0, y1, z0), "[1 0 0 0] 0.25", "[0 0 -1 0] 0.25"),  # +y
        ((x1, y0, z0), (x0, y0, z0), (x0, y0, z1), "[1 0 0 0] 0.25", "[0 0 -1 0] 0.25"),  # -y
    ]
    out = ["\tsolid", "\t{", '\t\t"id" "%d"' % ids()]
    for p0, p1, p2, ua, va in faces:
        out += ["\t\tside", "\t\t{", '\t\t\t"id" "%d"' % ids(),
                '\t\t\t"plane" "(%d %d %d) (%d %d %d) (%d %d %d)"' % (p0 + p1 + p2),
                '\t\t\t"material" "%s"' % material,
                '\t\t\t"uaxis" "%s"' % ua, '\t\t\t"vaxis" "%s"' % va,
                '\t\t\t"rotation" "0"', '\t\t\t"lightmapscale" "512"',
                '\t\t\t"smoothing_groups" "0"', "\t\t}"]
    out += ["\t}"]
    return out


def point_entity(ids, cls, origin, **kv):
    out = ["entity", "{", '\t"id" "%d"' % ids(), '\t"classname" "%s"' % cls]
    for k, v in kv.items():
        out.append('\t"%s" "%s"' % (k, v))
    out.append('\t"origin" "%d %d %d"' % tuple(origin))
    out.append("}")
    return out


def region_solids(ids):
    """The six wall brushes of the hollow region box (lines of VMF text)."""
    mn, mx, w = REGION_MIN, REGION_MAX, WALL
    walls = []
    walls += box_solid(ids, (mn[0] - w, mn[1] - w, mx[2]), (mx[0] + w, mx[1] + w, mx[2] + w), ROOF_MATERIAL)  # roof
    walls += box_solid(ids, (mn[0] - w, mn[1] - w, mn[2] - w), (mx[0] + w, mx[1] + w, mn[2]), WALL_MATERIAL)  # floor
    walls += box_solid(ids, (mn[0] - w, mn[1] - w, mn[2]), (mn[0], mx[1] + w, mx[2]), WALL_MATERIAL)  # -x
    walls += box_solid(ids, (mx[0], mn[1] - w, mn[2]), (mx[0] + w, mx[1] + w, mx[2]), WALL_MATERIAL)  # +x
    walls += box_solid(ids, (mn[0], mn[1] - w, mn[2]), (mx[0], mn[1], mx[2]), WALL_MATERIAL)  # -y
    walls += box_solid(ids, (mn[0], mx[1], mn[2]), (mx[0], mx[1] + w, mx[2]), WALL_MATERIAL)  # +y
    return walls


def region_markers(ids):
    """pc_mc_region (centre) and the two pc_mc_region_bounds info_targets."""
    mn, mx = REGION_MIN, REGION_MAX
    centre = [(a + b) // 2 for a, b in zip(mn, mx)]
    ents = []
    ents += point_entity(ids, "info_target", centre, targetname="pc_mc_region", spawnflags="0")
    ents += point_entity(ids, "info_target", mn, targetname="pc_mc_region_bounds", spawnflags="0")
    ents += point_entity(ids, "info_target", mx, targetname="pc_mc_region_bounds", spawnflags="0")
    return ents


def main():
    src, dst = sys.argv[1], sys.argv[2]
    text = open(src, encoding="latin-1").read()
    ids = Ids(10_000_000)
    mn, mx = REGION_MIN, REGION_MAX
    walls = region_solids(ids)

    # world solids go at the end of the world block, after Valve's brushes, so
    # the chamber's brush order (and so its BSP) is unchanged
    wi = text.index("\nworld\n{")
    depth, i = 0, text.index("{", wi)
    while True:
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                break
        i += 1
    world_end = i  # index of the world's closing brace
    text = text[:world_end] + "\n".join(walls) + "\n" + text[world_end:]

    ents = region_markers(ids)
    ents_text = "\n".join(ents) + "\n"

    # The alias script must run before any of Valve's scripts, so its
    # logic_script is the first entity after the world.
    alias = point_entity(ids, "logic_script", (0, 0, 0), targetname="pc_map_alias",
                         vscripts="portalcraft/pc_map_alias.nut")
    first_ent = text.index("\nentity\n{", world_end)
    text = text[:first_ent + 1] + "\n".join(alias) + "\n" + text[first_ent + 1:]
    open(dst, "w", encoding="latin-1").write(text + ents_text)
    print("region interior", mn, mx, "blocks at 40u:",
          [(b - a) // 40 for a, b in zip(mn, mx)])


if __name__ == "__main__":
    main()
