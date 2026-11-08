"""Offline checks of the built module against a game build.

    python check_module.py <exefs dir> <our subsdk9>

1. every import of our subsdk9 is exported by the game's sdk/main (else rtld aborts at boot);
2. the runtime resolver of source/program/coop/game.cpp, mirrored here instruction for
   instruction, finds every native and decodes the same offsets on this build.
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nso2elf import read_nso, build_image

def dynsyms(nso):
    segs, bss, _ = read_nso(nso)
    img, _ = build_image(segs, bss)
    img = bytes(img)
    mod0 = struct.unpack_from('<I', img, 4)[0]
    dyn = mod0 + struct.unpack_from('<i', img, mod0 + 4)[0]
    tags = {}
    for i in range(0, 0x400, 16):
        tag, val = struct.unpack_from('<qQ', img, dyn + i)
        if tag == 0:
            break
        tags[tag] = val
    strtab, symtab = tags[5], tags[6]
    n = (strtab - symtab) // 24 if strtab > symtab else 0
    imp, exp = set(), set()
    for i in range(1, n):
        nm, info, oth, shn, val, sz = struct.unpack_from('<IBBHQQ', img, symtab + i * 24)
        name = img[strtab + nm:img.index(b'\0', strtab + nm)].decode('latin-1')
        if name:
            (imp if shn == 0 and (info >> 4) != 2 else exp).add(name)   # weak undefined is fine
    return imp, exp

# ---- mirror of game.cpp -------------------------------------------------------------
def s64(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v

is_adrp = lambda w: (w & 0x9F000000) == 0x90000000
rd = lambda w: w & 0x1F
rn = lambda w: (w >> 5) & 0x1F
page = lambda pc, w: (pc & ~0xFFF) + (s64((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3), 21) << 12)
is_add = lambda w: (w & 0xFFC00000) == 0x91000000
imm12 = lambda w: (w >> 10) & 0xFFF
is_ldrx = lambda w: (w & 0xFFC00000) == 0xF9400000
is_ldrw = lambda w: (w & 0xFFC00000) == 0xB9400000
is_ldrs = lambda w: (w & 0xFFC00000) == 0xBD400000
is_ldrh = lambda w: (w & 0xFFC00000) == 0x79400000
is_b = lambda w: (w & 0xFC000000) == 0x14000000
btarget = lambda pc, w: pc + s64(w & 0x3FFFFFF, 26) * 4

def find_natives(main, names):
    """name -> native function address, the same way game.cpp FindNatives does."""
    import numpy as np
    segs, bss, build = read_nso(main)
    img, _ = build_image(segs, bss)
    img = bytes(img)
    (t0, text), (r0, ro) = segs[0], segs[1]
    t1 = t0 + len(text)
    strs = {}
    for n in names:
        needle, hits, i = n.encode() + b'\0', [], img.find(n.encode() + b'\0', r0)
        while i >= 0 and i < r0 + len(ro) and len(hits) < 4:
            if img[i - 1] == 0:
                hits.append(i)
            i = img.find(needle, i + 1)
        strs[n] = hits
    w = np.frombuffer(img[t0:t1], dtype='<u4').astype(np.int64)
    def pair_at(j):
        d = int(w[j])
        if not is_add(d):
            return 0
        for m in range(1, 13):
            if m > j:
                break
            a = int(w[j - m])
            if is_adrp(a) and rd(a) == rn(d):
                return page(t0 + (j - m) * 4, a) + imm12(d)
        return 0
    wanted = {s_: n for n in names for s_ in strs[n]}
    fn = {}
    for i in np.nonzero((w & 0xFFC00000) == 0x91000000)[0].tolist():
        if i < 1:
            continue
        n = wanted.get(pair_at(i))
        if not n or n in fn:
            continue
        for j in range(i + 1, i + 16):
            f = pair_at(j)
            if f and t0 <= f < t1:
                fn[n] = f
                break
    return build, img, fn

def resolve(main):
    names = ['GetPlayer', 'Notification', 'GetPositionY', 'GetAngleZ', 'GetParentCell', 'GetFormID',
             'GetWorldSpace', 'IsWeaponDrawn', 'IsDead']
    build, img, fn = find_natives(main, names)
    word = lambda a: struct.unpack_from('<I', img, a)[0]
    missing = [n for n in names if n not in fn]
    if missing:
        return build, None, 'не найдены: %s' % missing
    def field(f, isfn, scale, n=6):
        for i in range(n):
            x = word(f + 4 * i)
            if isfn(x) and rn(x) == 2:
                return imm12(x) * scale
    def tail(f, n=16):
        for i in range(n):
            x = word(f + 4 * i)
            if is_b(x):
                return btarget(f + 4 * i, x)
    c = [word(fn['GetPlayer'] + 4 * i) for i in range(3)]
    ok = is_adrp(c[0]) and is_ldrx(c[1]) and rn(c[1]) == rd(c[0]) and is_ldrx(c[2])
    vt = next((imm12(x) * 8 for x in (word(fn['IsDead'] + 4 * i) for i in range(6)) if is_ldrx(x) and rn(x) == 8), None)
    r = dict(natives={k: hex(v) for k, v in fn.items()},
             playerSlot=hex(page(fn['GetPlayer'], c[0]) + imm12(c[1]) * 8) if ok else None,
             notify=hex(tail(fn['Notification']) or 0), getWorldspace=hex(tail(fn['GetWorldSpace'], 2) or 0),
             offPos=hex(field(fn['GetPositionY'], is_ldrs, 4) - 4), offRotZ=hex(field(fn['GetAngleZ'], is_ldrs, 4)),
             offCell=hex(field(fn['GetParentCell'], is_ldrx, 8)), offFormId=hex(field(fn['GetFormID'], is_ldrw, 4)),
             offActorState=hex(field(fn['IsWeaponDrawn'], is_ldrh, 2, 2)), vtIsDead=hex(vt or 0))
    return build, r, None

if __name__ == '__main__':
    exefs, ours = sys.argv[1], sys.argv[2]
    imp, _ = dynsyms(ours)
    _, sdk = dynsyms(os.path.join(exefs, 'sdk'))
    _, main = dynsyms(os.path.join(exefs, 'main'))
    missing = sorted(x for x in imp if x not in sdk and x not in main)
    print('импортов модуля: %d, не найдено в игре: %s' % (len(imp), missing or 'нет'))
    build, r, err = resolve(os.path.join(exefs, 'main'))
    print('сборка игры', build[:16])
    if err:
        print('РЕЗОЛВЕР:', err)
    else:
        for k, v in r.items():
            print('  %-14s %s' % (k, v))
