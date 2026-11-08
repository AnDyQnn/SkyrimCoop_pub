"""RTTI walk in the Switch NSO: class name -> typeinfo -> vtables -> code that loads the vtable.
    python rtti.py <nso> <MangledName e.g. 13TESActionData>"""
import struct, sys
import numpy as np
import xrefs

def main(path, name):
    img, t0, t1, _ = xrefs.load(path)
    s = img.find(b'\0' + name.encode() + b'\0') + 1
    print('name', hex(s))
    q = np.frombuffer(img[:len(img) - len(img) % 8], dtype='<u8')
    tis = [i * 8 - 8 for i in np.nonzero(q == s)[0]]            # typeinfo: {vtable_for_ti, name}
    for ti in tis:
        print('typeinfo', hex(ti))
        for v in np.nonzero(q == ti)[0]:
            vt = v * 8 + 8                                         # vtable address point after {offset, typeinfo}
            top = struct.unpack_from('<q', img, vt - 16)[0]
            if top > 0 or top < -0x10000:
                continue
            slots = [struct.unpack_from('<Q', img, vt + 8 * k)[0] for k in range(12)]
            print('  vtable %s top %d slots %s' % (hex(vt), top, ' '.join(hex(x) for x in slots)))
            pcs, addrs, kinds = xrefs.adrp_add_pairs(img, t0, t1)
            refs = list(pcs[(addrs == vt) & (kinds == 0)])
            for g in np.nonzero(q == vt - 16)[0]:                # GOT slot holding the vtable start (+0x10 in code)
                refs += list(pcs[(addrs == g * 8) & (kinds == 1)])
            print('    code refs:', ' '.join(hex(p) for p in sorted(refs)[:20]))

main(sys.argv[1], sys.argv[2])
