"""Find BSFixedString::BSFixedString(const char*) in a Switch NSO: the function most often called
with x1 = a C string from .rodata (ADRP+ADD just before) and x0 = a stack slot ("add x0, sp/x29, #n").

    python find_fixedstring.py <main nso>
"""
import os, struct, sys
from collections import Counter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import check_module as cm
from nso2elf import read_nso, build_image

segs, bss, _ = read_nso(sys.argv[1])
img, _ = build_image(segs, bss)
img = bytes(img)
(t0, text), (r0, ro) = segs[0], segs[1]
w = np.frombuffer(text, dtype='<u4').astype(np.int64)
bl = np.nonzero((w & 0xFC000000) == 0x94000000)[0]
cnt = Counter()
for i in bl.tolist():
    x0_stack = x1_str = False
    for k in range(1, 6):
        v = int(w[i - k])
        if cm.is_add(v) and cm.rd(v) == 0 and cm.rn(v) in (31, 29):           # add x0, sp|x29, #n
            x0_stack = True
        if cm.is_add(v) and cm.rd(v) == 1:                                     # add x1, x1, #lo
            for m in range(1, 5):
                a = int(w[i - k - m])
                if cm.is_adrp(a) and cm.rd(a) == 1:
                    addr = cm.page(t0 + (i - k - m) * 4, a) + cm.imm12(v)
                    if r0 <= addr < r0 + len(ro) and img[addr - 1] == 0:
                        x1_str = True
                    break
    if x0_stack and x1_str:
        imm = int(w[i]) & 0x3FFFFFF
        imm = imm - (1 << 26) if imm & (1 << 25) else imm
        cnt[t0 + i * 4 + imm * 4] += 1
for addr, n in cnt.most_common(6):
    print('0x%x  %d' % (addr, n))
