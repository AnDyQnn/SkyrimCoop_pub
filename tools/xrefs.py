"""Fast string / code cross-references in a Switch NSO without a full Ghidra analysis.

    python xrefs.py <nso> natives            # Papyrus native registrations -> notes/natives_<build>.tsv
    python xrefs.py <nso> str <text>         # code that loads the string
    python xrefs.py <nso> dis <hexaddr> [n]  # disassemble n instructions

AArch64 code loads addresses with ADRP Xd, page + ADD Xd, Xd, #lo12 (sometimes a few
instructions apart). All such pairs are decoded with numpy over the whole .text, giving
address -> [code addresses that compute it]. Papyrus registers natives as
NativeFunctionN(name, className, fn, vm): the three pointers are computed next to each other,
so for each wanted name the code pointer computed right after the name/class loads is the native.
"""
import os, re, struct, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nso2elf import read_nso, build_image

def apply_relative_relocs(img):
    """Fill R_AARCH64_RELATIVE slots (vtables, GOT) with module offsets, as rtld would at load."""
    mod0 = struct.unpack_from('<I', img, 4)[0]
    dyn = mod0 + struct.unpack_from('<i', img, mod0 + 4)[0]
    tags = {}
    for i in range(0, 0x400, 16):
        tag, val = struct.unpack_from('<qQ', img, dyn + i)
        if tag == 0:
            break
        tags.setdefault(tag, val)
    rela, size = tags.get(7), tags.get(8, 0)
    for o in range(rela or 0, (rela or 0) + size, 24):
        off, info, addend = struct.unpack_from('<QQq', img, o)
        if info & 0xFFFFFFFF == 1027 and off + 8 <= len(img):
            struct.pack_into('<Q', img, off, addend)

def load(path):
    segs, bss, build_id = read_nso(path)
    img, _ = build_image(segs, bss)
    apply_relative_relocs(img)
    text = segs[0]
    return bytes(img), text[0], text[0] + len(text[1]), build_id[:16]

def adrp_add_pairs(img, t0, t1, window=6):
    w = np.frombuffer(img[t0:t1], dtype='<u4')
    pc = np.arange(t0, t1, 4, dtype=np.int64)
    is_adrp = (w & 0x9F000000) == 0x90000000
    rd = (w & 0x1F).astype(np.int64)
    immlo = ((w >> 29) & 3).astype(np.int64)
    immhi = ((w >> 5) & 0x7FFFF).astype(np.int64)
    imm = (immhi << 2) | immlo
    imm = np.where(imm & (1 << 20), imm - (1 << 21), imm)
    page = (pc & ~0xFFF) + (imm << 12)
    # ADD Xd, Xn, #imm12 (64-bit, shift 0): 0x91000000 mask 0xFFC00000
    is_add = (w & 0xFFC00000) == 0x91000000
    add_rn = ((w >> 5) & 0x1F).astype(np.int64)
    add_imm = ((w >> 10) & 0xFFF).astype(np.int64)
    # LDR Xt, [Xn, #imm] (unsigned offset, 64-bit): 0xF9400000 mask 0xFFC00000 -> address of a pointer
    is_ldr = (w & 0xFFC00000) == 0xF9400000
    ldr_imm = (((w >> 10) & 0xFFF).astype(np.int64)) * 8
    out_pc, out_addr, out_kind = [], [], []
    idx = np.nonzero(is_adrp)[0]
    for k in range(1, window + 1):
        j = idx + k
        ok = j < len(w)
        i2, j2 = idx[ok], j[ok]
        m = is_add[j2] & (add_rn[j2] == rd[i2])
        out_pc.append(pc[j2[m]]); out_addr.append(page[i2[m]] + add_imm[j2[m]]); out_kind.append(np.zeros(m.sum(), np.int8))
        m = is_ldr[j2] & (add_rn[j2] == rd[i2])
        out_pc.append(pc[j2[m]]); out_addr.append(page[i2[m]] + ldr_imm[j2[m]]); out_kind.append(np.ones(m.sum(), np.int8))
    return np.concatenate(out_pc), np.concatenate(out_addr), np.concatenate(out_kind)

def find_strings(img, text):
    needle = text.encode() + b'\0'
    out, i = [], img.find(needle)
    while i >= 0:
        if img[i - 1] == 0:
            out.append(i)
        i = img.find(needle, i + 1)
    return out

def dis(img, addr, n=40):
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    return ['0x%08x  %-8s %s' % (i.address, i.mnemonic, i.op_str) for i in md.disasm(img[addr:addr + 4 * n], addr)]

WANTED = [('SetUnconscious', 'Actor'), ('PushActorAway', 'ObjectReference'), ('ForceAddRagdollToWorld', 'ObjectReference'), ('SetDontMove', 'Actor'), ('PlayIdle', 'Actor')] + [('IsInCombat', 'Actor'), ('GetCombatTarget', 'Actor'), ('IsHostileToActor', 'Actor'), ('IsPlayerTeammate', 'Actor'), ('GetActorValuePercentage', 'Actor'), ('IsCommandedActor', 'Actor'), ('IsGuard', 'Actor'), ('GetCombatState', 'Actor'), ('IsEssential', 'Actor'), ('GetCurrentCrosshairRef', 'Game'), ('GetDisplayName', 'ObjectReference'), ('GetHeadingAngle', 'ObjectReference'), ('GetHeight', 'ObjectReference'), ('GetScale', 'ObjectReference'), ('GetBaseActorValue', 'Actor'), ('SetActorValue', 'Actor'), ('AddPerk', 'Actor'), ('HasPerk', 'Actor'), ('AddSpell', 'Actor'), ('HasSpell', 'Actor'), ('GetLevel', 'Actor'), ('AddShout', 'Actor'), ('HasSpell', 'Actor'), ('GetActorValue', 'Actor'), ('SetRace', 'Actor'), ('AddItem', 'ObjectReference'), ('QueueNiNodeUpdate', 'Actor'), ('Start', 'Quest'), ('SetCurrentStageID', 'Quest'), ('IsRunning', 'Quest'), ('IsCompleted', 'Quest'), ('Reset', 'Quest'), ('PlaceAtMe', 'ObjectReference'), ('SetPosition', 'ObjectReference'), ('MoveTo', 'ObjectReference'),
          ('GetPositionX', 'ObjectReference'), ('GetPositionY', 'ObjectReference'), ('GetPositionZ', 'ObjectReference'),
          ('GetAngleZ', 'ObjectReference'), ('SetAngle', 'ObjectReference'), ('TranslateTo', 'ObjectReference'),
          ('Disable', 'ObjectReference'), ('Enable', 'ObjectReference'), ('Delete', 'ObjectReference'),
          ('GetParentCell', 'ObjectReference'), ('GetWorldSpace', 'ObjectReference'), ('GetFormID', 'Form'),
          ('GetPlayer', 'Game'), ('GetForm', 'Game'), ('GetFormFromFile', 'Game'),
          ('SendAnimationEvent', 'Debug'), ('Notification', 'Debug'), ('MessageBox', 'Debug'),
          ('IsSneaking', 'Actor'), ('IsRunning', 'Actor'), ('IsSprinting', 'Actor'), ('IsWeaponDrawn', 'Actor'),
          ('IsInCombat', 'Actor'), ('IsDead', 'Actor'), ('SetAlpha', 'Actor'), ('SetGhost', 'Actor'),
          ('SetDontMove', 'Actor'), ('EnableAI', 'Actor'), ('GetActorBase', 'Actor'), ('KeepOffsetFromActor', 'Actor'),
          ('SetRestrained', 'Actor'), ('PathToReference', 'Actor'),
          ('EquipItem', 'Actor'), ('UnequipItem', 'Actor'), ('UnequipAll', 'Actor'), ('AddItem', 'ObjectReference'),
          ('RemoveAllItems', 'ObjectReference'), ('GetEquippedWeapon', 'Actor'), ('IsEquipped', 'Actor'), ('GetItemCount', 'ObjectReference'),
          ('IsOnMount', 'Actor'), ('GetPlayersLastRiddenHorse', 'Game'), ('GetBaseObject', 'ObjectReference'), ('Dismount', 'Actor'), ('Activate', 'ObjectReference'), ('IsBeingRidden', 'Actor'),
          ('SetObjectiveDisplayed', 'Quest'), ('SetObjectiveCompleted', 'Quest'), ('AddToMap', 'ObjectReference'),
          ('ForceRefTo', 'ReferenceAlias'), ('GetDistance', 'ObjectReference'), ('ModActorValue', 'Actor'),
          ('ForceActorValue', 'Actor'), ('StartCombat', 'Actor'), ('SetScale', 'ObjectReference'), ('Stop', 'Quest'),
          ('CompleteQuest', 'Quest'), ('SetActive', 'Quest'), ('GetCurrentStageID', 'Quest'),
          ('Is3DLoaded', 'ObjectReference'), ('GetHeadingAngle', 'ObjectReference'),
          ('SetFactionRank', 'Actor'), ('RemoveFromFaction', 'Actor'), ('AddToFaction', 'Actor'), ('GetFactionRank', 'Actor'),
          ('ResetInventory', 'ObjectReference'),
          ('GetStageDone', 'Quest'), ('IsStageDone', 'Quest'), ('QueryStat', 'Game'), ('IsCleared', 'Location'),
          ('GetCrimeGold', 'Faction'), ('GetValue', 'GlobalVariable'), ('IsWordUnlocked', 'Game')]

def natives(path):
    img, t0, t1, build = load(path)
    pcs, addrs, kinds = adrp_add_pairs(img, t0, t1)
    order = np.argsort(pcs)
    pcs, addrs, kinds = pcs[order], addrs[order], kinds[order]
    by_addr = {}
    for p, a in zip(pcs[kinds == 0].tolist(), addrs[kinds == 0].tolist()):
        by_addr.setdefault(a, []).append(p)
    rows = []
    for name, cls in WANTED:
        cls_addrs = set(find_strings(img, cls))
        for s in find_strings(img, name):
            for p in by_addr.get(s, []):
                # the class string must be loaded within 16 instructions around the name load
                lo, hi = np.searchsorted(pcs, [p - 64, p + 64])
                near = addrs[lo:hi][kinds[lo:hi] == 0].tolist()
                if not cls_addrs & set(near):
                    continue
                code = sorted({a for a in near if t0 <= a < t1})
                rows.append((name, cls, '0x%x' % s, '0x%x' % p, ','.join('0x%x' % a for a in code)))
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'notes', 'natives_%s.tsv' % build)
    with open(out, 'w', encoding='utf-8') as f:
        f.write('name\tclass\tstring\tload_at\tcode_ptrs\n')
        for r in rows:
            f.write('\t'.join(r) + '\n')
    found = {r[0] for r in rows}
    print('build %s: найдено %d/%d, нет: %s' % (build, len(found), len(WANTED), [n for n, _ in WANTED if n not in found]))
    print(os.path.abspath(out))

if __name__ == '__main__':
    path, cmd = sys.argv[1], sys.argv[2]
    if cmd == 'natives':
        natives(path)
    elif cmd == 'str':
        img, t0, t1, _ = load(path)
        pcs, addrs, kinds = adrp_add_pairs(img, t0, t1)
        for s in find_strings(img, sys.argv[3]):
            print('string 0x%x:' % s, ['0x%x' % p for p in pcs[(addrs == s) & (kinds == 0)].tolist()])
    elif cmd == 'ptr':      # 8-byte values at an address (vtables, GOT slots)
        img, *_ = load(path)
        a = int(sys.argv[3], 16)
        for i in range(int(sys.argv[4]) if len(sys.argv) > 4 else 8):
            print('0x%08x  0x%x' % (a + 8 * i, struct.unpack_from('<Q', img, a + 8 * i)[0]))
    elif cmd == 'dis':
        img, *_ = load(path)
        print('\n'.join(dis(img, int(sys.argv[3], 16), int(sys.argv[4]) if len(sys.argv) > 4 else 40)))
