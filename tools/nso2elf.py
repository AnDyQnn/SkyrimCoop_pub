"""Convert a Switch NSO (main, sdk, subsdkN) into an ELF64 that Ghidra loads natively.

    python nso2elf.py main main.elf

NSO: 0x100-byte header with three segments (.text, .rodata, .data), each optionally
LZ4-compressed, plus .bss size. The segments are laid out at their memory offsets in
one flat image; the ELF gets one PT_LOAD per segment and, if the image has a MOD0
header, a PT_DYNAMIC pointing at the module's _DYNAMIC so Ghidra picks up .dynsym
(import/export names such as nn::socket::*).
"""
import struct, sys, hashlib
import lz4.block

def read_nso(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'NSO0', 'not an NSO'
    flags = struct.unpack_from('<I', d, 0x0C)[0]
    segs = []
    for i in range(3):
        file_off, mem_off, size = struct.unpack_from('<III', d, 0x10 + i * 0x10)
        comp_size = struct.unpack_from('<I', d, 0x60 + i * 4)[0]
        raw = d[file_off:file_off + comp_size]
        if flags & (1 << i):
            raw = lz4.block.decompress(raw, uncompressed_size=size)
        if flags & (1 << (i + 3)):
            want = d[0xA0 + i * 0x20:0xC0 + i * 0x20]
            assert hashlib.sha256(raw).digest() == want, 'segment %d hash mismatch' % i
        segs.append((mem_off, raw))
    bss = struct.unpack_from('<I', d, 0x3C)[0]
    build_id = d[0x40:0x60].hex().upper()
    return segs, bss, build_id

def build_image(segs, bss):
    end = max(off + len(raw) for off, raw in segs)
    img = bytearray(end)
    for off, raw in segs:
        img[off:off + len(raw)] = raw
    return img, end + bss

def find_dynamic(img):
    """MOD0 offset lives at text+4; MOD0 has dynamic offset relative to itself."""
    mod0 = struct.unpack_from('<I', img, 4)[0]
    if img[mod0:mod0 + 4] != b'MOD0':
        return None
    dyn_rel = struct.unpack_from('<i', img, mod0 + 4)[0]
    return mod0 + dyn_rel

def write_elf(out, segs, img, mem_size, dyn):
    phdrs = []
    for (off, raw), flags in zip(segs, (5, 4, 6)):          # R-X, R--, RW-
        memsz = len(raw) if off != segs[2][0] else mem_size - off
        phdrs.append((1, flags, off, len(raw), memsz))
    if dyn is not None:
        dyn_size = 0
        while True:
            tag = struct.unpack_from('<q', img, dyn + dyn_size)[0]
            dyn_size += 16
            if tag == 0:
                break
        phdrs.append((2, 6, dyn, dyn_size, dyn_size))
    ehsize, phsz = 64, 56
    data_off = 0x1000
    hdr = bytearray(b'\x7fELF' + bytes([2, 1, 1, 0]) + bytes(8))
    hdr += struct.pack('<HHIQQQIHHHHHH', 3, 183, 1, 0, ehsize, 0, 0, ehsize, phsz, len(phdrs), 64, 0, 0)
    ph = bytearray()
    for ptype, flags, vaddr, filesz, memsz in phdrs:
        ph += struct.pack('<IIQQQQQQ', ptype, flags, data_off + vaddr, vaddr, vaddr, filesz, memsz, 0x1000)
    blob = hdr + ph
    blob += bytes(data_off - len(blob)) + img
    open(out, 'wb').write(blob)

if __name__ == '__main__':
    segs, bss, build_id = read_nso(sys.argv[1])
    img, mem = build_image(segs, bss)
    dyn = find_dynamic(img)
    write_elf(sys.argv[2], segs, img, mem, dyn)
    names = ('text', 'rodata', 'data')
    print('build id: %s' % build_id)
    for (off, raw), n in zip(segs, names):
        print('  %-6s 0x%08X  0x%08X' % (n, off, len(raw)))
    print('  bss    0x%08X  _DYNAMIC: %s' % (bss, hex(dyn) if dyn is not None else 'нет'))
