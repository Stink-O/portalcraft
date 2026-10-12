#!/usr/bin/env python3
"""Offline check (bridge #54): a Minecraft model cube's triangles, after the exporter's (0,1,2),(0,2,3) split, the entity pose's
scale(-1,-1,1) and avatar.cpp's mapping (x, y, z) -> (x, -z, y), have normals (cross product) pointing OUT of the cube.
Cube corners and per-face vertex order are the ones ModelPart.Cube builds (the order is what a back-face-culling renderer needs).
Run: python3 avatar_winding_check.py   (exit 1 on a failure)"""
import itertools, sys

def cross(a, b): return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
def sub(a, b): return tuple(x-y for x, y in zip(a, b))
def dot(a, b): return sum(x*y for x, y in zip(a, b))

def cube_faces(mirror):
    minx, miny, minz, maxx, maxy, maxz = -2.0, -3.0, -1.0, 2.0, 3.0, 1.0   # an arbitrary non-symmetric box
    if mirror: minx, maxx = maxx, minx
    t0, t1, t2, t3 = (minx, miny, minz), (maxx, miny, minz), (maxx, maxy, minz), (minx, maxy, minz)
    l0, l1, l2, l3 = (minx, miny, maxz), (maxx, miny, maxz), (maxx, maxy, maxz), (minx, maxy, maxz)
    faces = {"DOWN": [l1, l0, t0, t1], "UP": [t2, t3, l3, l2], "WEST": [t0, l0, l3, t3],
             "NORTH": [t1, t0, t3, t2], "EAST": [l1, t1, t2, l2], "SOUTH": [l0, l1, l2, l3]}
    if mirror: faces = {k: v[::-1] for k, v in faces.items()}   # Polygon's constructor reverses a mirrored face
    return faces

def to_source(p):
    x, y, z = -p[0], -p[1], p[2]          # LivingEntityRenderer: scale(-1, -1, 1)
    return (x, -z, y)                     # avatar.cpp: Minecraft (x, y up, z) -> Source (x, -z, y)

bad = 0
for mirror in (False, True):
    faces = cube_faces(mirror)
    pts = [to_source(p) for f in faces.values() for p in f]
    centre = tuple(sum(c)/len(pts) for c in zip(*pts))
    for name, q in faces.items():
        v = [to_source(p) for p in q]
        for tri in ((0, 1, 2), (0, 2, 3)):
            a, b, c = (v[i] for i in tri)
            n = cross(sub(b, a), sub(c, a))
            out = dot(n, sub(tuple(sum(x)/3 for x in zip(a, b, c)), centre))
            if out <= 0:
                bad += 1
                print(f"INWARD  mirror={mirror} face={name} tri={tri}")
print("winding: all outward" if not bad else f"winding: {bad} inward triangles")
sys.exit(1 if bad else 0)
