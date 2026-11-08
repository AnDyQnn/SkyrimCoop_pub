"""Offline prototype of the puppet resolver (mirrored in coop/game.cpp).

    python puppet_resolve.py <main nso>

From Papyrus natives (found like check_module.py does):
  PlaceAtMe   -> CreateReferenceAtLocation = the call with "mov x6, xzr" and "mov x7, xzr" just
                 before it; x0 at that call = *slot -> TESDataHandler singleton slot;
                 handle table slot = the slot whose register feeds "add xA, xT, wI, uxtw #4"
                 right after "and wI, wH, #0xfffff".
  SetPosition -> move task: allocator = call right after "mov w0, #0x28"; vtable = address stored
                 to [task, #0]; QueueTask = first call after that with x0 = *slot (queue singleton).
A tiny symbolic tracker walks the code linearly: ADRP -> page, ADD page -> address,
LDR [page+imm] -> GOT slot, LDR [slot] -> *slot, MOV copies; a write of any other kind forgets.
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_module as cm

MOV_X6_ZR, MOV_X7_ZR, MOV_W0_28 = 0xAA1F03E6, 0xAA1F03E7, 0x52800500
is_bl = lambda w: (w & 0xFC000000) == 0x94000000
is_mov = lambda w: (w & 0xFFE0FFE0) == 0xAA0003E0              # orr xd, xzr, xm
is_str_x = lambda w: (w & 0xFFC00000) == 0xF9000000             # str xt, [xn, #imm*8]
is_and_fffff = lambda w: (w & 0xFFFFFC00) == 0x12004C00         # and wd, wn, #0xfffff
is_add_uxtw4 = lambda w: (w & 0xFFE0FC00) == 0x8B205000         # add xd, xn, wm, uxtw #4

def writes_rd(w):
    """Does the instruction write register Rd (bits 0-4)? Conservative for the cases that matter."""
    if (w & 0x3A000000) == 0x38000000:                  # loads/stores (register/imm forms)
        return bool(w & 0x00400000)                     # L bit set = load
    if (w & 0x7C000000) == 0x14000000 or (w & 0xFE000000) == 0x54000000:
        return False                                    # b / bl / b.cond
    if (w & 0x7E000000) == 0x34000000 or (w & 0x7E000000) == 0x36000000:
        return False                                    # cbz/cbnz/tbz/tbnz
    if (w & 0xFFFFFC1F) == 0xD63F0000 or (w & 0xFFFFFC1F) == 0xD61F0000 or w == 0xD65F03C0:
        return False                                    # blr / br / ret
    if (w & 0x7F20001F) == 0x6B00001F or (w & 0x7F80001F) == 0x7100001F:
        return False                                    # cmp (subs to zr)
    if (w & 0xFFC00000) in (0xA9000000, 0xA9800000, 0x29000000, 0x6D000000):
        return False                                    # stp
    return True

def track(img, start, stop):
    regs = {}
    for pc in range(start, stop, 4):
        w = struct.unpack_from('<I', img, pc)[0]
        yield pc, w, regs
        d = w & 0x1F
        if cm.is_adrp(w):
            regs[d] = ('page', cm.page(pc, w))
        elif cm.is_ldrx(w):
            b, imm = regs.get(cm.rn(w)), cm.imm12(w) * 8
            v = ('slot', b[1] + imm) if b and b[0] == 'page' else ('deref', b[1]) if b and b[0] == 'slot' and imm == 0 else None
            regs[d] = v
        elif cm.is_add(w):
            b = regs.get(cm.rn(w))
            regs[d] = ('addr', b[1] + cm.imm12(w)) if b and b[0] == 'page' else None
        elif is_mov(w):
            regs[d] = regs.get((w >> 16) & 0x1F)
        elif is_bl(w) or (w & 0xFFFFFC1F) == 0xD63F0000:
            for r in range(19):
                regs.pop(r, None)
        elif writes_rd(w):
            regs[d] = None

def u32(img, a):
    return struct.unpack_from('<I', img, a)[0]

def resolve(path):
    build, img, fn = cm.find_natives(path, ['PlaceAtMe', 'SetPosition'])
    if len(fn) < 2:
        return build, None, 'нет нативов: %s' % [n for n in ('PlaceAtMe', 'SetPosition') if n not in fn]
    out = {}
    f = fn['PlaceAtMe']
    last_and = None
    for pc, w, regs in track(img, f, f + 0x800):
        if is_bl(w) and 'create' not in out:
            back = [u32(img, pc - 4 * k) for k in range(1, 11)]
            x0 = regs.get(0)
            if MOV_X7_ZR in back and x0 and x0[0] == 'deref':
                out['create'] = cm.btarget(pc, w)
                out['dataHandlerSlot'] = x0[1]
        if is_and_fffff(w):
            last_and = pc
        if last_and and is_add_uxtw4(w) and pc - last_and <= 16 and 'handleTableSlot' not in out:
            t = regs.get(cm.rn(w))
            out['handleTableSlot'] = t[1] if t and t[0] == 'slot' else None
    f = fn['SetPosition']
    prev = 0
    for pc, w, regs in track(img, f, f + 0x400):
        if is_bl(w) and prev == MOV_W0_28 and 'alloc' not in out:
            out['alloc'] = cm.btarget(pc, w)
        elif 'alloc' in out and 'taskVtable' not in out and is_str_x(w) and cm.imm12(w) == 0:
            v = regs.get(w & 0x1F)
            if v and v[0] == 'addr':
                out['taskVtable'] = v[1]
        elif 'taskVtable' in out and 'queue' not in out and is_bl(w):
            x0 = regs.get(0)
            if x0 and x0[0] == 'deref':
                out['queue'] = cm.btarget(pc, w)
                out['queueSlot'] = x0[1]
        prev = w
    out['natives'] = {k: hex(v) for k, v in fn.items()}
    return build, out, None

if __name__ == '__main__':
    build, r, err = resolve(sys.argv[1])
    print('сборка', build[:16])
    print(err or '\n'.join('  %-16s %s' % (k, hex(v) if isinstance(v, int) else v) for k, v in r.items()))
