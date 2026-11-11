#!/usr/bin/env python3
"""Read and write Jedi Knight .3do (version 2.1) models, and compute their rest pose
exactly as the engine does (rdThing_AccumulateMatrices).

Used by the model tools (skinned exports, retargeting). Library only; see
tools/make_skinned_test.py for an example.
"""
import math
import re

import numpy as np


class Mesh:
    def __init__(self):
        self.name = ""
        self.radius = 0.0
        self.geo_mode, self.light_mode, self.tex_mode = 4, 3, 0
        self.verts = []        # [(x, y, z, i)]
        self.uvs = []          # [(u, v)] in texels (3DO 2.1)
        self.normals = []      # [(x, y, z)]
        self.faces = []        # [{'mat', 'type', 'geo', 'light', 'tex', 'extralight', 'idx': [(v, uv)]}]
        self.face_normals = []


class Node:
    def __init__(self, parts):
        # num: flags type mesh parent child sibling numChildren x y z pitch yaw roll px py pz name
        self.flags = parts[1]
        self.type = parts[2]
        self.mesh = int(parts[3])
        self.parent = int(parts[4])
        self.child = int(parts[5])
        self.sibling = int(parts[6])
        self.num_children = int(parts[7])
        self.pos = tuple(map(float, parts[8:11]))
        self.rot = tuple(map(float, parts[11:14]))  # pitch, yaw, roll in degrees
        self.pivot = tuple(map(float, parts[14:17]))
        self.name = parts[17]


class Model:
    def __init__(self):
        self.materials = []
        self.radius = 0.0
        self.insert_offset = (0.0, 0.0, 0.0)
        self.geosets = []      # [[Mesh]]
        self.nodes = []


def _strip(text):
    return [l.split("#", 1)[0].rstrip() for l in text.splitlines()]


def load(path):
    lines = [l for l in _strip(open(path, encoding="latin1").read()) if l.strip()]
    m = Model()
    i = 0

    def nxt():
        nonlocal i
        i += 1
        return lines[i - 1]

    while i < len(lines):
        l = nxt().strip()
        u = l.upper()
        if u.startswith("MATERIALS"):
            for _ in range(int(l.split()[1])):
                m.materials.append(nxt().split()[1])
        elif u.startswith("RADIUS") and not m.geosets:
            m.radius = float(l.split()[1])
        elif u.startswith("INSERT OFFSET"):
            m.insert_offset = tuple(map(float, l.split()[2:5]))
        elif u.startswith("GEOSET ") :
            m.geosets.append([])
        elif u.startswith("MESH ") and not u.startswith("MESHES"):
            me = Mesh()
            m.geosets[-1].append(me)
        elif u.startswith("NAME"):
            m.geosets[-1][-1].name = l.split()[1]
        elif u.startswith("RADIUS"):
            m.geosets[-1][-1].radius = float(l.split()[1])
        elif u.startswith("GEOMETRYMODE"):
            m.geosets[-1][-1].geo_mode = int(l.split()[1])
        elif u.startswith("LIGHTINGMODE"):
            m.geosets[-1][-1].light_mode = int(l.split()[1])
        elif u.startswith("TEXTUREMODE"):
            m.geosets[-1][-1].tex_mode = int(l.split()[1])
        elif u.startswith("VERTICES"):
            me = m.geosets[-1][-1]
            for _ in range(int(l.split()[1])):
                p = nxt().split()
                me.verts.append(tuple(map(float, p[1:5])))
        elif u.startswith("TEXTURE VERTICES"):
            me = m.geosets[-1][-1]
            for _ in range(int(l.split()[2])):
                p = nxt().split()
                me.uvs.append(tuple(map(float, p[1:3])))
        elif u.startswith("VERTEX NORMALS"):
            me = m.geosets[-1][-1]
            for _ in range(len(me.verts)):
                p = nxt().split()
                me.normals.append(tuple(map(float, p[1:4])))
        elif u.startswith("FACES"):
            me = m.geosets[-1][-1]
            for _ in range(int(l.split()[1])):
                p = nxt().replace(",", " ").split()
                n = int(p[7])
                idx = [(int(p[8 + 2 * k]), int(p[9 + 2 * k])) for k in range(n)]
                me.faces.append({"mat": int(p[1]), "type": p[2], "geo": int(p[3]), "light": int(p[4]),
                                 "tex": int(p[5]), "extralight": float(p[6]), "idx": idx})
        elif u.startswith("FACE NORMALS"):
            me = m.geosets[-1][-1]
            for _ in range(len(me.faces)):
                p = nxt().split()
                me.face_normals.append(tuple(map(float, p[1:4])))
        elif u.startswith("HIERARCHY NODES"):
            for _ in range(int(l.split()[2])):
                m.nodes.append(Node(nxt().split()))
    return m


def save(m, path, comment="written by tools/jk3do.py"):
    o = ["# MODEL %s" % comment, "", "SECTION: HEADER", "", "3DO 2.1", "",
         "SECTION: MODELRESOURCE", "", "MATERIALS %d" % len(m.materials), ""]
    o += ["%10d: %14s" % (k, n) for k, n in enumerate(m.materials)]
    o += ["", "", "SECTION: GEOMETRYDEF", "", "RADIUS %10.6f" % m.radius, "",
          "INSERT OFFSET %10.6f %10.6f %10.6f" % m.insert_offset, "", "GEOSETS %d" % len(m.geosets), ""]
    for g, geo in enumerate(m.geosets):
        o += ["GEOSET %d" % g, "", "MESHES %d" % len(geo), ""]
        for k, me in enumerate(geo):
            o += ["", "MESH %d" % k, "", "NAME %s" % me.name, "", "RADIUS %10.6f" % me.radius, "",
                  "GEOMETRYMODE\t%d" % me.geo_mode, "LIGHTINGMODE\t%d" % me.light_mode, "TEXTUREMODE\t%d" % me.tex_mode,
                  "", "", "VERTICES %d" % len(me.verts), ""]
            o += ["%6d: %10.6f %10.6f %10.6f %10.6f" % ((n,) + tuple(v)) for n, v in enumerate(me.verts)]
            o += ["", "", "TEXTURE VERTICES %d" % len(me.uvs), ""]
            o += ["%6d: %10.6f %10.6f" % ((n,) + tuple(t)) for n, t in enumerate(me.uvs)]
            o += ["", "", "VERTEX NORMALS", ""]
            o += ["%6d: %10.6f %10.6f %10.6f" % ((n,) + tuple(t)) for n, t in enumerate(me.normals)]
            o += ["", "", "FACES %d" % len(me.faces), ""]
            for n, f in enumerate(me.faces):
                verts = " ".join("%3d,%3d" % vi for vi in f["idx"])
                o.append("%6d: %9d %7s %5d %7d %6d %12.4f %7d %s" % (n, f["mat"], f["type"], f["geo"], f["light"],
                                                                     f["tex"], f["extralight"], len(f["idx"]), verts))
            o += ["", "", "FACE NORMALS", ""]
            o += ["%6d: %10.6f %10.6f %10.6f" % ((n,) + tuple(t)) for n, t in enumerate(me.face_normals)]
            o.append("")
    o += ["", "SECTION: HIERARCHYDEF", "", "HIERARCHY NODES %d" % len(m.nodes), ""]
    for n, nd in enumerate(m.nodes):
        o.append("%6d: %7s %7s %8d %8d %7d %9d %13d %10.6f %10.6f %10.6f %10.6f %10.6f %10.6f %10.6f %10.6f %10.6f  %s" % (
            n, nd.flags, nd.type, nd.mesh, nd.parent, nd.child, nd.sibling, nd.num_children,
            *nd.pos, *nd.rot, *nd.pivot, nd.name))
    o.append("")
    open(path, "w", encoding="latin1", newline="\n").write("\n".join(o))


# ---------------------------------------------------------------- engine maths

def build34(pyr, pos):
    """rdMatrix_Build34 as a 4x4 (columns: rvec, lvec, uvec, translation). Angles in degrees."""
    xs, xc = math.sin(math.radians(pyr[0])), math.cos(math.radians(pyr[0]))
    ys, yc = math.sin(math.radians(pyr[1])), math.cos(math.radians(pyr[1]))
    zs, zc = math.sin(math.radians(pyr[2])), math.cos(math.radians(pyr[2]))
    r = [-(zs * ys) * xs + zc * yc, (zs * yc) * xs + zc * ys, -zs * xc]
    lv = [-ys * xc, yc * xc, xs]
    uv = [(zc * ys) * xs + zs * yc, -xs * (yc * zc) + ys * zs, zc * xc]
    M = np.eye(4)
    M[:3, 0], M[:3, 1], M[:3, 2], M[:3, 3] = r, lv, uv, pos
    return M


def translate(v):
    M = np.eye(4)
    M[:3, 3] = v
    return M


def rest_matrices(m, local=None):
    """World matrix of every node at rest (or with `local` per-node matrices), as the engine's
    rdThing_AccumulateMatrices computes them with an identity placement."""
    out = [None] * len(m.nodes)

    def acc(i, parent_world):
        nd = m.nodes[i]
        loc = local[i] if local is not None else build34(nd.rot, nd.pos)
        mat = loc @ translate(nd.pivot)
        if nd.parent >= 0:
            mat = translate([-c for c in m.nodes[nd.parent].pivot]) @ mat
        out[i] = parent_world @ mat
        c = nd.child
        for _ in range(nd.num_children):
            if c < 0:
                break
            acc(c, out[i])
            c = m.nodes[c].sibling

    acc(0, np.eye(4))
    return out


def write_skin(path, meshes):
    """meshes: [(geoset, mesh_index, [[(node, weight), ...] per vertex])]"""
    o = ["# skin weights (tools/jk3do.py): vertex: count node weight ...", "SKIN 1", ""]
    for g, k, weights in meshes:
        o.append("MESH %d %d %d" % (g, k, len(weights)))
        for v, ws in enumerate(weights):
            o.append("%d: %d %s" % (v, len(ws), " ".join("%d %.6f" % (n, w) for n, w in ws)))
        o.append("")
    open(path, "w", newline="\n").write("\n".join(o))
