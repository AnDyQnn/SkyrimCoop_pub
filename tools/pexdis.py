"""Minimal Papyrus .pex (Skyrim, big-endian) disassembler: every function's instructions with names resolved.
    python pexdis.py <file.pex> [function name]"""
import struct, sys

OPS = ['nop', 'iadd', 'fadd', 'isub', 'fsub', 'imul', 'fmul', 'idiv', 'fdiv', 'imod', 'not', 'ineg', 'fneg', 'assign',
       'cast', 'cmp_eq', 'cmp_lt', 'cmp_lte', 'cmp_gt', 'cmp_gte', 'jmp', 'jmpt', 'jmpf', 'callmethod', 'callparent',
       'callstatic', 'return', 'strcat', 'propget', 'propset', 'array_create', 'array_length', 'array_getelement',
       'array_setelement', 'array_findelement', 'array_rfindelement']
NARGS = [0, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 1, 2, 2, -3, -2, -3, 1, 3, 3, 3, 2, 2, 3, 3, 4, 4]

d = open(sys.argv[1], 'rb').read()
p = 16


def u8():
    global p
    v = d[p]; p += 1; return v


def u16():
    global p
    v = struct.unpack_from('>H', d, p)[0]; p += 2; return v


def u32():
    global p
    v = struct.unpack_from('>I', d, p)[0]; p += 4; return v


def s():
    global p
    n = u16(); v = d[p:p+n].decode('latin-1'); p += n; return v


s(); s(); s()
strs = [s() for _ in range(u16())]
S = lambda: strs[u16()]
if u8():                                         # debug info
    u32(); u32()
    for _ in range(u16()):
        S(); S(); S(); u8()
        for _ in range(u16()):
            u16()
for _ in range(u16()):                           # user flags
    S(); u8()


def value():
    t = u8()
    if t == 0: return 'None'
    if t in (1, 2): return ('$' if t == 1 else '"') + S() + ('' if t == 1 else '"')
    if t == 3: return str(struct.unpack('>i', struct.pack('>I', u32()))[0])
    if t == 4: return '%g' % struct.unpack('>f', struct.pack('>I', u32()))[0]
    if t == 5: return 'True' if u8() else 'False'


def function(name):
    S(); S(); u32(); u8()                         # return type, doc, user flags, flags
    params = [(S(), S()) for _ in range(u16())]
    locs = [(S(), S()) for _ in range(u16())]
    code = []
    for _ in range(u16()):
        op = u8()
        n = NARGS[op]
        args = [value() for _ in range(abs(n) if n >= 0 else -n)]
        if n < 0:
            extra = int(value())
            args += [value() for _ in range(extra)]
        code.append((OPS[op], args))
    return params, code


want = sys.argv[2].lower() if len(sys.argv) > 2 else None
for _ in range(u16()):                           # objects
    oname = S(); size = u32(); end = p + size - 4
    S(); S(); u32(); S()                          # parent, doc, flags, auto state
    for _ in range(u16()):                        # variables
        S(); S(); u32(); value()
    for _ in range(u16()):                        # properties
        pn = S(); S(); S(); u32(); fl = u8()
        if fl & 4: S()
        else:
            if fl & 1: function('get')
            if fl & 2: function('set')
    for _ in range(u16()):                        # states
        st = S()
        for _ in range(u16()):
            fn = S()
            params, code = function(fn)
            if want and fn.lower() != want:
                continue
            print('== %s.%s(%s)' % (st or '<default>', fn, ', '.join(n for n, t in params)))
            for i, (op, args) in enumerate(code):
                print('  %3d %-12s %s' % (i, op, ' '.join(args)))
    p = end
