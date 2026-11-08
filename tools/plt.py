"""Callers of an imported function (by mangled-name substring) in a Switch NSO.

    python plt.py <nso> <name substring>

.rela.plt JUMP_SLOT relocation -> GOT slot of the symbol; the PLT stub is
"adrp x16, page; ldr x17, [x16, #off]; add x16, x16, #off; br x17" loading that slot;
every BL to the stub is a caller.
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from nso2elf import read_nso, build_image
import check_module as cm

def main(path, needle):
    segs, bss, _ = read_nso(path)
    img, _ = build_image(segs, bss)
    img = bytes(img)
    mod0 = struct.unpack_from('<I', img, 4)[0]
    dyn = mod0 + struct.unpack_from('<i', img, mod0 + 4)[0]
    tags = {}
    for i in range(0, 0x400, 16):
        tag, val = struct.unpack_from('<qQ', img, dyn + i)
        if tag == 0:
            break
        tags.setdefault(tag, val)
    strtab, symtab, jmprel, pltsz = tags[5], tags[6], tags[23], tags[2]
    slots = {}
    for o in range(jmprel, jmprel + pltsz, 24):
        off, info, add = struct.unpack_from('<QQq', img, o)
        nm = struct.unpack_from('<I', img, symtab + (info >> 32) * 24)[0]
        name = img[strtab + nm:img.index(b'\0', strtab + nm)].decode('latin-1')
        if needle in name:
            slots[off] = name
    t0, text = segs[0]
    w = np.frombuffer(text, dtype='<u4').astype(np.int64)
    stubs = {}
    for i in np.nonzero((w & 0x9F00001F) == 0x90000010)[0].tolist():      # adrp x16
        l = int(w[i + 1])
        if (l & 0xFFC003FF) == 0xF9400211:                                  # ldr x17, [x16, #imm]
            slot = cm.page(t0 + i * 4, int(w[i])) + cm.imm12(l) * 8
            if slot in slots:
                stubs[t0 + i * 4] = slots[slot]
    bl = np.nonzero((w & 0xFC000000) == 0x94000000)[0]
    imm = (w[bl] & 0x3FFFFFF)
    imm = np.where(imm & (1 << 25), imm - (1 << 26), imm)
    tgt = t0 + bl * 4 + imm * 4
    for stub, name in stubs.items():
        callers = (t0 + bl[tgt == stub] * 4).tolist()
        print('%s  stub 0x%x  callers: %s' % (name, stub, ' '.join('0x%x' % c for c in callers[:12])))

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
