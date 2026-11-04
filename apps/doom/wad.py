#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""IWADs for the DOOM pod.

  wad.py fetch [--dir DIR]                 download Freedoom (phase 1) and check it
  wad.py cut IN.wad OUT.wad [--map E1M1]   one map and the lumps the engine needs for it

`cut` keeps the map, the textures, flats and patches its sides and sectors
use (with the skies, the episode's switches and whole animations), the
sprites of everything that can appear on it (its things, what they fire and
drop, the player's weapons, puffs, blood and fog), and the status bar, menu,
font and intermission graphics. It drops the other maps, sounds, music,
demos and the pages a single map never shows. The engine tables it reads
(info.c, p_spec.c, p_switch.c) come from doomgeneric/ next to this file.
"""
import argparse
import hashlib
import os
import re
import struct
import sys
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(HERE, "doomgeneric")
FREEDOOM_URL = "https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip"
FREEDOOM_SHA256 = "3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59"
MAP_LUMPS = ["THINGS", "LINEDEFS", "SIDEDEFS", "VERTEXES", "SEGS", "SSECTORS", "NODES", "SECTORS", "REJECT", "BLOCKMAP"]
# Actions that spawn another thing, and the things a death drops
SPAWNS = {
    "A_TroopAttack": ["MT_TROOPSHOT"], "A_HeadAttack": ["MT_HEADSHOT"], "A_BruisAttack": ["MT_BRUISERSHOT"],
    "A_CyberAttack": ["MT_ROCKET"], "A_FatAttack1": ["MT_FATSHOT"], "A_FatAttack2": ["MT_FATSHOT"],
    "A_FatAttack3": ["MT_FATSHOT"], "A_SkelMissile": ["MT_TRACER"], "A_Tracer": ["MT_SMOKE"],
    "A_BspiAttack": ["MT_ARACHPLAZ"], "A_PainAttack": ["MT_SKULL"], "A_PainDie": ["MT_SKULL"],
    "A_VileTarget": ["MT_FIRE"], "A_BrainSpit": ["MT_SPAWNSHOT"], "A_SpawnFly": ["MT_SPAWNFIRE"],
    "A_FireMissile": ["MT_ROCKET"], "A_FirePlasma": ["MT_PLASMA"], "A_FireBFG": ["MT_BFG"],
    "A_BFGSpray": ["MT_EXTRABFG"], "A_BrainScream": ["MT_ROCKET"],
}
DROPS = {"MT_POSSESSED": ["MT_CLIP"], "MT_WOLFSS": ["MT_CLIP"], "MT_SHOTGUY": ["MT_SHOTGUN"], "MT_CHAINGUY": ["MT_CHAINGUN"]}
ALWAYS = ["MT_PLAYER", "MT_PUFF", "MT_BLOOD", "MT_TFOG", "MT_IFOG"]
# The fist and pistol, and each weapon with its pickup (and what it fires)
WEAPON_SPRITES = {"PUNG", "PISG", "PISF"}
WEAPONS = {"MT_SHOTGUN": ({"SHTG", "SHTF"}, []), "MT_SUPERSHOTGUN": ({"SHT2", "SHTF"}, []),
           "MT_CHAINGUN": ({"CHGG", "CHGF"}, []), "MT_MISC27": ({"MISG", "MISF"}, ["MT_ROCKET"]),
           "MT_MISC28": ({"PLSG", "PLSF"}, ["MT_PLASMA"]), "MT_MISC25": ({"BFGG", "BFGF"}, ["MT_BFG", "MT_EXTRABFG"]),
           "MT_MISC26": ({"SAWG"}, [])}
PAGES = {"CREDIT", "HELP", "HELP1", "HELP2", "VICTORY2", "ENDPIC", "BOSSBACK", "PFUB1", "PFUB2", "TITLEPIC",
         "ENDOOM", "GENMIDI", "DMXGUS", "DMXGUSC"} | {"END%d" % i for i in range(7)}


def read_wad(path):
    data = open(path, "rb").read()
    kind, n, off = struct.unpack_from("<4sii", data, 0)
    lumps = []
    for i in range(n):
        pos, size, name = struct.unpack_from("<ii8s", data, off + 16 * i)
        lumps.append([name.rstrip(b"\0").decode("latin-1").upper(), data[pos:pos + size]])
    return kind, lumps


def write_wad(path, kind, lumps):
    body = bytearray()
    entries = []
    for name, data in lumps:
        entries.append((12 + len(body), len(data), name))
        body += data
    out = struct.pack("<4sii", kind, len(lumps), 12 + len(body)) + body
    for pos, size, name in entries:
        out += struct.pack("<ii8s", pos if size else 0, size, name.encode("latin-1"))
    open(path, "wb").write(out)
    return len(out)


def c_tables():
    info = open(os.path.join(ENGINE, "info.c"), encoding="latin-1").read()
    states = re.findall(r"\{SPR_(\w+),\d+,-?\d+,\{(\w+)\},(S_\w+),\d+,\d+\},\s*//\s*(S_\w+)", info)
    state = {name: (spr, action, nxt) for spr, action, nxt, name in states}
    mobj = {}
    for block in re.findall(r"\{\s*//\s*(MT_\w+)(.*?)\n\s*\}", info, re.S):
        fields = dict((k, v.strip()) for v, k in re.findall(r"\n\s*([^\n]*?),?\s*//\s*(\w+)", block[1]))
        mobj[block[0]] = fields
    spec = open(os.path.join(ENGINE, "p_spec.c"), encoding="latin-1").read()
    anims = [(t == "true", start, end) for t, end, start in re.findall(r'\{(true|false),\s*"(\w+)",\s*"(\w+)",\s*8\}', spec)]
    sw = open(os.path.join(ENGINE, "p_switch.c"), encoding="latin-1").read()
    switches = re.findall(r'\{"(\w+)",\s*"(\w+)",\s*(\d)\}', sw)
    return state, mobj, anims, switches


def sprites_for(things, state, mobj):
    by_num = {int(f["doomednum"]): mt for mt, f in mobj.items() if f.get("doomednum", "-1").lstrip("-").isdigit() and int(f["doomednum"]) > 0}
    todo = set(ALWAYS) | {by_num[t] for t in things if t in by_num}
    seen, sprites, weapon = set(), set(), set(WEAPON_SPRITES)
    queue = list(todo)
    states_todo = []
    while queue or states_todo:
        if queue:
            mt = queue.pop()
            if mt in seen or mt not in mobj:
                continue
            seen.add(mt)
            if mt in WEAPONS:
                weapon |= WEAPONS[mt][0]
                queue += WEAPONS[mt][1]
                states_todo += [s for s, v in state.items() if v[0] in WEAPONS[mt][0]]
            f = mobj[mt]
            states_todo += [f[k] for k in ("spawnstate", "seestate", "painstate", "meleestate", "missilestate",
                                           "deathstate", "xdeathstate", "raisestate") if f.get(k, "S_NULL") != "S_NULL"]
            queue += DROPS.get(mt, [])
            continue
        s = states_todo.pop()
        visited = set()
        while s in state and s != "S_NULL" and s not in visited:
            visited.add(s)
            spr, action, nxt = state[s]
            sprites.add(spr)
            queue += SPAWNS.get(action, [])
            s = nxt
    for s, v in state.items():
        if v[0] in WEAPON_SPRITES:
            sprites.add(v[0])
    return sprites | weapon


def cut(src, dst, mapname, intermission=True):
    kind, lumps = read_wad(src)
    names = [n for n, _ in lumps]
    state, mobj, anims, switches = c_tables()
    i = names.index(mapname)
    maplumps = dict(lumps[i + 1:i + 1 + len(MAP_LUMPS)])
    things = [struct.unpack_from("<hhhhh", maplumps["THINGS"], o)[3] for o in range(0, len(maplumps["THINGS"]), 10)]
    sides = maplumps["SIDEDEFS"]
    walls = set()
    for o in range(0, len(sides), 30):
        for k in (4, 12, 20):
            walls.add(sides[o + k:o + k + 8].rstrip(b"\0").decode("latin-1").upper())
    secs = maplumps["SECTORS"]
    floors = {secs[o + k:o + k + 8].rstrip(b"\0").decode("latin-1").upper() for o in range(0, len(secs), 26) for k in (4, 12)}
    episode = int(mapname[1]) if mapname.startswith("E") else 0
    walls |= {"SKY%d" % max(episode, 1)}
    for a, b, ep in switches:
        if int(ep) <= 1:
            walls |= {a, b}
    floors |= {"F_SKY1"}

    # textures, in their numbering order (TEXTURE1 then TEXTURE2)
    pnames_lump = dict(lumps)["PNAMES"]
    pnames = [pnames_lump[4 + 8 * k:12 + 8 * k].rstrip(b"\0").decode("latin-1").upper() for k in range(struct.unpack_from("<i", pnames_lump)[0])]
    textures = []
    for tl in ("TEXTURE1", "TEXTURE2"):
        if tl not in names:
            continue
        d = dict(lumps)[tl]
        n = struct.unpack_from("<i", d)[0]
        for off in struct.unpack_from("<%di" % n, d, 4):
            pc = struct.unpack_from("<h", d, off + 20)[0]
            textures.append((d[off:off + 8].rstrip(b"\0").decode("latin-1").upper(), d[off:off + 22 + 10 * pc]))
    tnames = [t for t, _ in textures]
    keep_tex = {0} | {k for k, t in enumerate(tnames) if t in walls}
    for is_tex, start, end in anims:
        if is_tex and start in tnames and end in tnames:
            a, b = tnames.index(start), tnames.index(end)
            if any(k in keep_tex for k in range(a, b + 1)):
                keep_tex |= set(range(a, b + 1))
    used_patches = []
    tex_out = []
    for k in sorted(keep_tex):
        name, raw = textures[k]
        raw = bytearray(raw)
        pc = struct.unpack_from("<h", raw, 20)[0]
        for p in range(pc):
            pn = pnames[struct.unpack_from("<h", raw, 22 + 10 * p + 4)[0]]
            if pn not in used_patches:
                used_patches.append(pn)
            struct.pack_into("<h", raw, 22 + 10 * p + 4, used_patches.index(pn))
        tex_out.append(bytes(raw))
    head = struct.pack("<i", len(tex_out))
    offs, body, pos = [], b"", 4 + 4 * len(tex_out)
    for t in tex_out:
        offs.append(pos + len(body))
        body += t
    texture1 = head + struct.pack("<%di" % len(offs), *offs) + body
    pnames_out = struct.pack("<i", len(used_patches)) + b"".join(p.encode("latin-1").ljust(8, b"\0") for p in used_patches)

    # flats: the used ones and whole animations, in lump order
    f0, f1 = names.index("F_START"), names.index("F_END")
    flat_names = names[f0 + 1:f1]
    keep_flat = {k for k, f in enumerate(flat_names) if f in floors}
    for is_tex, start, end in anims:
        if not is_tex and start in flat_names and end in flat_names:
            a, b = flat_names.index(start), flat_names.index(end)
            if any(k in keep_flat for k in range(a, b + 1)):
                keep_flat |= set(range(a, b + 1))

    sprites = sprites_for(things, state, mobj)
    s0, s1 = names.index("S_START"), names.index("S_END")
    other_maps = set()
    for k, n in enumerate(names):
        if k + 1 < len(names) and names[k + 1] == "THINGS" and n != mapname:
            other_maps |= set(range(k, k + 1 + len(MAP_LUMPS)))
    patch_set = set(used_patches)
    in_patches = False
    out = []
    sizes = {}
    for k, (name, data) in enumerate(lumps):
        cat = "other"
        if k in other_maps or name.startswith("DEMO") or name.startswith("DS") or name.startswith("DP") or name.startswith("D_") \
                or name in PAGES or re.match(r"WI(MAP[12]|A[12])", name) \
                or (not intermission and (name.startswith("WI") or name == "INTERPIC")):
            continue
        if name in ("P_START", "PP_START"):
            in_patches = True
        elif name in ("P_END", "PP_END"):
            in_patches = False
        if name == "TEXTURE2":
            continue
        if name == "TEXTURE1":
            data, cat = texture1, "textures"
        elif name == "PNAMES":
            data, cat = pnames_out, "textures"
        elif f0 < k < f1:
            if k - f0 - 1 not in keep_flat:
                continue
            cat = "flats"
        elif s0 < k < s1:
            if name[:4] not in sprites:
                continue
            cat = "sprites"
        elif in_patches and data:
            if name not in patch_set:
                continue
            cat = "patches"
        elif name in patch_set:
            cat = "patches"
        elif k == i or k in range(i + 1, i + 1 + len(MAP_LUMPS)):
            cat = "map"
        out.append((name, data))
        sizes[cat] = sizes.get(cat, 0) + len(data)
    total = write_wad(dst, b"IWAD", out)
    print("%s: %d lumps, %d bytes (from %d lumps, %d bytes)" % (dst, len(out), total, len(lumps), os.path.getsize(src)))
    for cat in sorted(sizes):
        print("  %-9s %9d bytes" % (cat, sizes[cat]))
    print("  textures %d of %d, patches %d of %d, flats %d of %d, sprites %s" % (
        len(tex_out), len(textures), len(used_patches), len(pnames), len(keep_flat), len(flat_names), " ".join(sorted(sprites))))


def fetch(directory):
    os.makedirs(directory, exist_ok=True)
    zpath = os.path.join(directory, os.path.basename(FREEDOOM_URL))
    if not os.path.exists(zpath):
        print("downloading", FREEDOOM_URL)
        urllib.request.urlretrieve(FREEDOOM_URL, zpath)
    digest = hashlib.sha256(open(zpath, "rb").read()).hexdigest()
    if digest != FREEDOOM_SHA256:
        sys.exit("%s: sha256 %s, expected %s" % (zpath, digest, FREEDOOM_SHA256))
    with zipfile.ZipFile(zpath) as z:
        member = next(m for m in z.namelist() if m.endswith("/freedoom1.wad"))
        out = os.path.join(directory, "freedoom1.wad")
        open(out, "wb").write(z.read(member))
    print(out, os.path.getsize(out), "bytes")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch")
    f.add_argument("--dir", default=os.path.join(HERE, "build"))
    c = sub.add_parser("cut")
    c.add_argument("src")
    c.add_argument("dst")
    c.add_argument("--map", default="E1M1")
    c.add_argument("--no-intermission", action="store_true", help="also drop the intermission screens (the map's exit then stops the game)")
    a = ap.parse_args()
    if a.cmd == "fetch":
        fetch(a.dir)
    else:
        cut(a.src, a.dst, a.map.upper(), not a.no_intermission)


if __name__ == "__main__":
    main()
