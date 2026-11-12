#!/usr/bin/env python3
"""Terrain skin: a finer, lumpy copy of a level's organic terrain faces (stage 3, option 2).

    python3 tools/lightbake/terrain_skin.py LEVEL [--tris N] [--wall A] [--floor A] [--edge E]

Reads hd/lightbake/<level>/level.json (tools/lightbake/jkl_export.py) and takes every rock, cliff,
dirt, sand and grass face (never metal, panels or architecture; the same name list as the
parallax). Those faces are split into small triangles (longest-edge bisection, so neighbours
always share their new corners and nothing cracks) and every corner is pushed out along the
smoothed surface normal by rock-scale noise, mostly into the rock and a little out of it: on
walls up to --wall-in (0.10 units, about 1 m) in and --wall-out (0.025) out, on floors
--floor-in 0.006 / --floor-out 0.002 so the player still stands on them, and zero along any edge
where the terrain meets something else (doors, walls, panels, sky), so it still joins up.
Collision stays on the flat original, which the engine stops drawing where there is a skin (so
outward bumps stay under a player's width).

Writes
  hd/lightbake/<level>/skin.npz  (for the light bake: positions, triangles, surface, texture UV)
  hd/jkgm/lightmaps/<level>.jkls (for the engine, little endian):
      char[4] "JKLS", u32 version (1), u32 world vertex count (checked against the level),
      u32 surface count S, then S x { u32 surface, u32 triangles T, T x 3 x f32 x, y, z, u, v }
  (u, v = texture coordinates in texture widths, as the level's own.)
"""
import heapq
import json
import os
import re
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
# Organic only, checked against the textures: "wex" is metal wall panels, "wst"/"wstn"/"waq"/"dtw"
# are Barons Hed buildings, "stn"/"ston" palace masonry, "car3" machinery, "03wrck" stone with pipes.
TERRAIN = re.compile(r"cliff|clif|rock|grs|grass|dirt|sand|mud|grnd|cave|wexrck", re.I)


def arg(name, default, cast=float):
    return cast(sys.argv[sys.argv.index(name) + 1]) if name in sys.argv else default


def log(*a):
    print("SKIN:", *a, flush=True)


# ---- noise ---------------------------------------------------------------------------------------

def _hash3(i, j, k, seed):
    h = (i * 374761393 + j * 668265263 + k * 2147483647 + seed * 144665) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0


def value_noise(P, seed):
    """Smooth 3D value noise in [0, 1] at points P (N x 3)."""
    f = np.floor(P)
    t = P - f
    t = t * t * (3 - 2 * t)
    f = f.astype(np.int64)
    out = np.zeros(len(P))
    for dx in (0, 1):
        for dy in (0, 1):
            for dz in (0, 1):
                w = (t[:, 0] if dx else 1 - t[:, 0]) * (t[:, 1] if dy else 1 - t[:, 1]) * (t[:, 2] if dz else 1 - t[:, 2])
                out += w * _hash3(f[:, 0] + dx, f[:, 1] + dy, f[:, 2] + dz, seed)
    return out


def rock_noise(P):
    """Rock-scale relief in [0, 1]: broad lumps (about 10 m) with ledges and knobs on top.
    One unit is about 10 m."""
    n = 0.0
    amp, freq, tot = 1.0, 1.1, 0.0
    for o in range(3):
        v = value_noise(P * freq + o * 17.3, 11 + o)
        if o >= 1:
            v = 1.0 - np.abs(2 * v - 1)        # ridged: sharper crests, like rock strata and ledges
        n = n + amp * v
        tot += amp
        amp *= 0.5
        freq *= 2.2
    return n / tot


# ---- geometry ------------------------------------------------------------------------------------

def terrain_surfaces(d):
    return [s for s in d["surfaces"]
            if not s["sky"] and not s["alpha"] and "lm" in s and len(s["v"]) >= 3 and TERRAIN.search(s["mat"])]


def build_mesh(d, surfs):
    """Triangles over shared positions (merged by position: neighbouring sectors repeat corners),
    with per-corner texture UV and the surface each triangle came from."""
    V = np.array(d["verts"], float)
    key = {}
    pos = []

    def vid(p):
        k = tuple(np.round(p, 4))
        if k not in key:
            key[k] = len(pos)
            pos.append(np.array(p, float))
        return key[k]

    polys = []
    for s in surfs:
        polys.append(([vid(V[v]) for v in s["v"]], [tuple(uv) for uv in s["uv"]], s["index"]))
    P = np.array(pos)

    # T-junctions: a corner of one face lying on the edge of another. Split such edges there
    # first, or the two sides would be subdivided differently and crack apart.
    fixed = []
    for vs, uvs, si in polys:
        nv, nuv = [], []
        for i in range(len(vs)):
            a, b = vs[i], vs[(i + 1) % len(vs)]
            ua, ub = np.array(uvs[i]), np.array(uvs[(i + 1) % len(vs)])
            nv.append(a)
            nuv.append(tuple(ua))
            e = P[b] - P[a]
            L2 = float(e @ e)
            if L2 < 1e-10:
                continue
            t = (P - P[a]) @ e / L2
            off = P - P[a] - t[:, None] * e
            on = np.where((t > 1e-4) & (t < 1 - 1e-4) & ((off * off).sum(1) < 1e-8))[0]
            for j in on[np.argsort(t[on])]:
                nv.append(int(j))
                nuv.append(tuple(ua + t[j] * (ub - ua)))
        fixed.append((nv, nuv, si))

    tris, tuv, tsurf = [], [], []
    for vs, uvs, si in fixed:
        for i in range(1, len(vs) - 1):
            c = (0, i, i + 1)
            if len({vs[k] for k in c}) < 3:
                continue
            tris.append([vs[k] for k in c])
            tuv.append([uvs[k] for k in c])
            tsurf.append(si)
    return P, np.array(tris), np.array(tuv, float), np.array(tsurf)


def subdivide(P, T, UV, S, max_edge):
    """Longest-edge bisection until no edge is longer than max_edge. Splitting an edge splits
    every triangle using it, at the same new point, so the mesh stays watertight."""
    pos = [p for p in P]
    tris = [list(t) for t in T]
    uvs = [[np.array(u) for u in tu] for tu in UV]
    surf = list(S)
    alive = [True] * len(tris)
    edge_tris = {}

    def ek(a, b):
        return (a, b) if a < b else (b, a)

    def length(e):
        d = pos[e[0]] - pos[e[1]]
        return float(np.sqrt(d @ d))

    for i, t in enumerate(tris):
        for k in range(3):
            edge_tris.setdefault(ek(t[k], t[(k + 1) % 3]), set()).add(i)
    heap = [(-length(e), e) for e in edge_tris]
    heapq.heapify(heap)
    while heap:
        negL, e = heapq.heappop(heap)
        if -negL <= max_edge:
            break
        if e not in edge_tris:
            continue
        users = list(edge_tris.pop(e))
        a, b = e
        m = len(pos)
        pos.append((pos[a] + pos[b]) * 0.5)
        for ti in users:
            t = tris[ti]
            k = next(k for k in range(3) if ek(t[k], t[(k + 1) % 3]) == e)
            i0, i1, i2 = t[k], t[(k + 1) % 3], t[(k + 2) % 3]
            u0, u1, u2 = uvs[ti][k], uvs[ti][(k + 1) % 3], uvs[ti][(k + 2) % 3]
            um = (u0 + u1) * 0.5
            alive[ti] = False
            for kk in range(3):
                s = edge_tris.get(ek(t[kk], t[(kk + 1) % 3]))
                if s is not None:
                    s.discard(ti)
            for nt, nu in (([i0, m, i2], [u0, um, u2]), ([m, i1, i2], [um, u1, u2])):
                ni = len(tris)
                tris.append(nt)
                uvs.append(nu)
                surf.append(surf[ti])
                alive.append(True)
                for kk in range(3):
                    ed = ek(nt[kk], nt[(kk + 1) % 3])
                    if ed not in edge_tris:
                        edge_tris[ed] = set()
                        L = length(ed)
                        if L > max_edge:
                            heapq.heappush(heap, (-L, ed))
                    edge_tris[ed].add(ni)
    keep = [i for i, a in enumerate(alive) if a]
    return (np.array(pos), np.array([tris[i] for i in keep]), np.array([[u for u in uvs[i]] for i in keep]),
            np.array([surf[i] for i in keep]))


def sky_segments(d):
    V = np.array(d["verts"], float)
    A, B = [], []
    for s in d["surfaces"]:
        if s["sky"]:
            for i in range(len(s["v"])):
                A.append(V[s["v"][i]])
                B.append(V[s["v"][(i + 1) % len(s["v"])]])
    return np.array(A).reshape(-1, 3), np.array(B).reshape(-1, 3)


def on_segments(Q, A, B, tol=1e-3):
    """For each point in Q: does it lie on any segment A-B?"""
    out = np.zeros(len(Q), bool)
    if not len(A):
        return out
    e = B - A
    L2 = np.maximum((e * e).sum(1), 1e-12)
    for i in range(0, len(Q), 256):
        q = Q[i:i + 256, None, :]
        t = np.clip(((q - A[None]) * e[None]).sum(2) / L2[None], 0, 1)
        d = q - (A[None] + t[..., None] * e[None])
        out[i:i + 256] = ((d * d).sum(2) < tol * tol).any(1)
    return out


def displace(P, T, wall, floor, sky=None):
    n = len(P)
    a, b, c = P[T[:, 0]], P[T[:, 1]], P[T[:, 2]]
    fn = np.cross(b - a, c - a)           # area weighted
    N = np.zeros_like(P)
    for k in range(3):
        np.add.at(N, T[:, k], fn)
    N /= np.linalg.norm(N, axis=1, keepdims=True) + 1e-12

    # Boundary: edges used by one triangle only (where terrain meets anything else).
    E = np.sort(np.concatenate([T[:, [0, 1]], T[:, [1, 2]], T[:, [2, 0]]]), 1)
    ue, cnt = np.unique(E, axis=0, return_counts=True)
    be = ue[cnt == 1]
    # Edges against the sky (cliff tops meeting the sky lid) stay free: a dip there just shows
    # more sky, a bump covers a little, and the skyline stops being a ruler-straight line.
    if sky is not None and len(be):
        free = on_segments((P[be[:, 0]] + P[be[:, 1]]) * 0.5, *sky)
        log("%d of %d border edges are against the sky (left free)" % (int(free.sum()), len(be)))
        skyv = np.zeros(n, bool)
        skyv[be[free].ravel()] = True
        be = be[~free]
    else:
        skyv = np.zeros(n, bool)
    border = np.zeros(n, bool)
    border[be.ravel()] = True
    # Fade the relief in over about 3 m from the border, so joins stay tight.
    from scipy.spatial import cKDTree
    dist = np.full(n, 1e9)
    if border.any():
        dist, _ = cKDTree(P[border]).query(P)
    fade = np.clip(dist / 0.3, 0, 1)
    fade = fade * fade * (3 - 2 * fade)

    up = np.clip((N[:, 2] - 0.35) / 0.4, 0, 1)            # 0 = wall or ceiling, 1 = floor
    a_in = wall[0] + (floor[0] - wall[0]) * up
    a_out = wall[1] + (floor[1] - wall[1]) * up
    h = rock_noise(P)
    # Mostly into the rock (hollows, recesses), a little out of it (knobs): the player collides
    # with the flat original, so what sticks out must stay under a player's width.
    k = 0.6
    D = np.where(h < k, (h - k) / k * a_in, (h - k) / (1 - k) * a_out) * fade
    # On the skyline only outwards, over the sky: a dip there would open onto the void behind
    # the cliff (nothing is drawn beyond the sky lid). Nobody walks up there, so the full depth.
    D[skyv] = np.abs(D[skyv])
    D[border] = 0.0
    return P + N * D[:, None], D, border


# ---- rubble -------------------------------------------------------------------------------------

def icosphere(subdiv=1):
    t = (1 + 5 ** 0.5) / 2
    V = [(-1, t, 0), (1, t, 0), (-1, -t, 0), (1, -t, 0), (0, -1, t), (0, 1, t), (0, -1, -t), (0, 1, -t),
         (t, 0, -1), (t, 0, 1), (-t, 0, -1), (-t, 0, 1)]
    F = [(0, 11, 5), (0, 5, 1), (0, 1, 7), (0, 7, 10), (0, 10, 11), (1, 5, 9), (5, 11, 4), (11, 10, 2),
         (10, 7, 6), (7, 1, 8), (3, 9, 4), (3, 4, 2), (3, 2, 6), (3, 6, 8), (3, 8, 9), (4, 9, 5),
         (2, 4, 11), (6, 2, 10), (8, 6, 7), (9, 8, 1)]
    V = [np.array(v, float) / np.linalg.norm(v) for v in V]
    if not subdiv:
        return np.array(V), np.array(F)
    mid = {}

    def m(a, b):
        k = (min(a, b), max(a, b))
        if k not in mid:
            c = V[a] + V[b]
            mid[k] = len(V)
            V.append(c / np.linalg.norm(c))
        return mid[k]
    F2 = []
    for a, b, c in F:
        ab, bc, ca = m(a, b), m(b, c), m(c, a)
        F2 += [(a, ab, ca), (b, bc, ab), (c, ca, bc), (ab, bc, ca)]
    return np.array(V), np.array(F2)


def uv_scale(d, s):
    """Texture repeats per world unit along the surface (from its own texture mapping)."""
    V = np.array(d["verts"], float)[s["v"]]
    UV = np.array(s["uv"], float)
    X, *_ = np.linalg.lstsq(V - V[0], UV - UV[0], rcond=None)
    return float(np.linalg.norm(X, axis=0).mean())


def floor_creases(d, P0, T0, S0):
    """Concave edges where an organic floor (normal up) meets an organic wall, from the terrain
    triangles before subdivision (T-junctions already split, so partly shared edges count)."""
    byidx = {s["index"]: s for s in d["surfaces"]}
    edges = {}
    for t, si in zip(T0, S0):
        for k in range(3):
            a, b = int(t[k]), int(t[(k + 1) % 3])
            edges.setdefault((min(a, b), max(a, b)), set()).add(int(si))
    out = []
    for (ia, ib), ss in edges.items():
        if len(ss) != 2:
            continue
        A, B = (byidx[x] for x in ss)
        nA = np.array(A["normal"]) / np.linalg.norm(A["normal"])
        nB = np.array(B["normal"]) / np.linalg.norm(B["normal"])
        a, b = P0[ia], P0[ib]
        cB = np.array(d["verts"])[B["v"]].mean(0)
        if np.dot(cB - a, nA) <= 1e-3:
            continue                                     # convex: an outside edge, nothing collects
        (fs, fn), (ws, wn) = ((A, nA), (B, nB)) if nA[2] > nB[2] else ((B, nB), (A, nA))
        if fn[2] > 0.6 and wn[2] < 0.5:
            out.append((a, b, fs, fn, ws, wn))
    return out


def rubble(d, creases, P, Pd, density, dmin, dmax, max_rocks, seed=7):
    """Rocks along every organic floor-wall crease: clustered, mostly small (--rock-min, a tennis
    ball and up, 10 cm), a few big (--rock-max, 45 cm), big ones tucked against the wall, small ones
    scattered out, each a random lumpy shape with a couple of flat broken faces. They sit on the
    skin (P -> Pd), take the wall's texture and are drawn with the wall."""
    from scipy.spatial import cKDTree
    rng = np.random.default_rng(seed)
    shapes = (icosphere(0), icosphere(1))   # 20 triangles under 30 cm, 80 above
    tree = cKDTree(P)
    rocks = []
    step = 0.005
    for a, b, fs, fn, ws, wn in creases:
        L = float(np.linalg.norm(b - a))
        t = (b - a) / max(L, 1e-9)
        o = wn - np.dot(wn, fn) * fn                     # along the floor, away from the wall
        o /= np.linalg.norm(o) + 1e-12
        xs = np.arange(step * 0.5, L, step)
        if not len(xs):
            continue
        Q = a[None] + xs[:, None] * t[None]
        c = np.clip((value_noise(Q * 3.0, 91) - 0.2) / 0.4, 0, 1) ** 1.2 * 2.0   # clusters and short gaps
        hit = rng.random(len(xs)) < density * step * c
        for q in Q[hit]:
            dia = dmin * (dmax / dmin) ** (rng.random() ** 1.3)
            r = dia / 2
            dist = r * 0.7 + rng.exponential(0.006 * (dmin / dia) ** 0.5 + 0.002)
            rocks.append((q + o * dist + t * rng.normal(0, 0.004), r, fn, ws))
    if len(rocks) > max_rocks:
        keep = rng.choice(len(rocks), max_rocks, replace=False)
        rocks = [rocks[i] for i in sorted(keep)]

    pos, tris, uvs, surf, nrm = [], [], [], [], []
    scale_cache = {}
    for base, r, fn, ws in rocks:
        _, i = tree.query(base)
        base = base + (Pd[i] - P[i])                     # onto the skin
        IV, IF = shapes[r > 0.015]
        v = IV.copy()
        v *= (1 + 0.45 * (value_noise(v * 1.7 + rng.random(3) * 50, 5) - 0.5))[:, None]
        v *= np.array([1.0, rng.uniform(0.65, 1.0), rng.uniform(0.6, 0.9)])
        for _ in range(2):                               # broken faces
            pl = rng.normal(size=3)
            pl /= np.linalg.norm(pl)
            cut = rng.uniform(0.5, 0.8)
            v -= np.maximum(v @ pl - cut, 0)[:, None] * pl[None]
        yaw, tilt = rng.uniform(0, 2 * np.pi), rng.normal(0, 0.25)
        cy, sy, ct, st = np.cos(yaw), np.sin(yaw), np.cos(tilt), np.sin(tilt)
        R = np.array([[cy, -sy, 0], [sy, cy, 0], [0, 0, 1]]) @ np.array([[1, 0, 0], [0, ct, -st], [0, st, ct]])
        v = v @ R.T * r
        # floor frame: z along the floor normal; sit about a third buried
        ex = np.cross(fn, [0.0, 1.0, 0.0] if abs(fn[1]) < 0.9 else [1.0, 0.0, 0.0])
        ex /= np.linalg.norm(ex)
        ey = np.cross(fn, ex)
        zmin = v[:, 2].min()
        W = base[None] + v[:, :1] * ex + v[:, 1:2] * ey + (v[:, 2:3] - zmin - 0.25 * (v[:, 2].max() - zmin)) * fn
        k = ws["index"]
        if k not in scale_cache:
            scale_cache[k] = uv_scale(d, ws)
        sc = scale_cache[k]
        off = rng.random(2)
        b0 = sum(len(p) for p in pos)
        pos.append(W)
        tris.append(IF + b0)
        # Smooth corner normals (the engine lights rocks with these, not the flat facets).
        fnrm = np.cross(W[IF[:, 1]] - W[IF[:, 0]], W[IF[:, 2]] - W[IF[:, 0]])
        vn = np.zeros_like(W)
        for c in range(3):
            np.add.at(vn, IF[:, c], fnrm)
        nrm.append(vn / (np.linalg.norm(vn, axis=1, keepdims=True) + 1e-12))
        # One texture projection for the whole rock (no seams between facets), facing the open
        # side: between up off the floor and out from the wall.
        wn = np.array(ws["normal"], float)
        ax = fn + 0.7 * (wn - np.dot(wn, fn) * fn)
        ax /= np.linalg.norm(ax)
        e1 = np.cross(ax, [0.0, 0.0, 1.0] if abs(ax[2]) < 0.9 else [1.0, 0.0, 0.0])
        e1 /= np.linalg.norm(e1)
        e2 = np.cross(ax, e1)
        vuv = np.stack([W @ e1, W @ e2], 1) * sc + off
        uvs.extend(vuv[IF])
        surf += [k] * len(IF)
    if not rocks:
        return None
    return (np.concatenate(pos).astype(np.float32), np.concatenate(tris).astype(np.int32),
            np.array(uvs, np.float32), np.array(surf, np.int32), len(rocks), len(creases),
            np.concatenate(nrm).astype(np.float32))


def main():
    level = sys.argv[1].replace(".jkl", "")
    folder = os.path.join(ROOT, "hd", "lightbake", level)
    d = json.load(open(os.path.join(folder, "level.json")))
    surfs = terrain_surfaces(d)
    if not surfs:
        log("%s: no terrain faces" % level)
        return
    P, T, UV, S = build_mesh(d, surfs)
    area = 0.5 * np.linalg.norm(np.cross(P[T[:, 1]] - P[T[:, 0]], P[T[:, 2]] - P[T[:, 0]]), axis=1).sum()
    budget = arg("--tris", 500000, int)
    # Roughly area / (edge^2 * 0.14) triangles come out of bisection at a given longest edge.
    edge = arg("--edge", max(0.06, float(np.sqrt(area / (budget * 0.14)))))
    log("%s: %d terrain faces, %d triangles, %.0f sq units; splitting to edges under %.3f" % (
        level, len(surfs), len(T), area, edge))
    creases = floor_creases(d, P, T, S)
    P, T, UV, S = subdivide(P, T, UV, S, edge)
    Pd, D, border = displace(P, T, (arg("--wall-in", 0.10), arg("--wall-out", 0.025)),
                             (arg("--floor-in", 0.006), arg("--floor-out", 0.002)), sky_segments(d))
    log("%d vertices, %d triangles; relief %.3f to %.3f (mean %.3f); %d pinned" % (
        len(P), len(T), float(D.min()), float(D.max()), float(D.mean()), int(border.sum())))

    rb = rubble(d, creases, P, Pd, arg("--rocks", 80.0), arg("--rock-min", 0.010), arg("--rock-max", 0.045),
                arg("--rocks-max", 40000, int))
    rpath = os.path.join(folder, "rubble.npz")
    if rb:
        rP, rT, rUV, rS, nrocks, ncre, rN = rb
        np.savez_compressed(rpath, pos=rP, tris=rT, uv=rUV, surf=rS, nrm=rN)
        log("rubble: %d rocks along %d floor creases, %d triangles" % (nrocks, ncre, len(rT)))
    elif os.path.exists(rpath):
        os.remove(rpath)
    np.savez_compressed(os.path.join(folder, "skin.npz"), pos=Pd.astype(np.float32), tris=T.astype(np.int32),
                        uv=UV.astype(np.float32), surf=S.astype(np.int32), edge=edge)
    out = os.path.join(ROOT, "hd", "jkgm", "lightmaps", level + ".jkls")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    order = np.argsort(S, kind="stable")
    with open(out, "wb") as f:
        f.write(b"JKLS")
        ids = np.unique(S)
        f.write(struct.pack("<3I", 1, len(d["verts"]), len(ids)))
        for si in ids:
            sel = order[S[order] == si]
            f.write(struct.pack("<2I", int(si), len(sel)))
            rec = np.concatenate([Pd[T[sel]], UV[sel]], 2).astype("<f4")   # T x 3 x 5
            f.write(rec.tobytes())
    log("SIDECAR %s, %.1f MB" % (os.path.relpath(out, ROOT), os.path.getsize(out) / 1e6))


if __name__ == "__main__":
    main()
