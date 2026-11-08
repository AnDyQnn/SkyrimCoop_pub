"""Minimal Skyrim SE .nif (20.2.0.7, BS 100) reader/patcher: blocks by type and size, names, shader properties.
    python nif.py <file.nif>        list blocks, names, shader colours
Used as a library by make_scroll_nifs.py (recolours the emissive glow of shader properties)."""
import struct, sys


class Nif:
    def __init__(self, path):
        self.path = path
        self.d = bytearray(open(path, 'rb').read())
        d = self.d
        p = d.index(b'\n') + 1
        self.version, = struct.unpack_from('<I', d, p); p += 4
        p += 1                                               # endian
        self.user, nblocks = struct.unpack_from('<II', d, p); p += 8
        self.bsver, = struct.unpack_from('<I', d, p); p += 4
        for _ in range(3):                                   # author, process script, export script (short strings)
            p += 1 + d[p]
        if self.bsver >= 103:
            p += 1 + d[p]
        ntypes, = struct.unpack_from('<H', d, p); p += 2
        self.types = []
        for _ in range(ntypes):
            n, = struct.unpack_from('<I', d, p); self.types.append(d[p+4:p+4+n].decode()); p += 4 + n
        self.btype = list(struct.unpack_from('<%dH' % nblocks, d, p)); p += 2 * nblocks
        self.bsize = list(struct.unpack_from('<%dI' % nblocks, d, p)); p += 4 * nblocks
        nstr, maxlen = struct.unpack_from('<II', d, p); p += 8
        self.strings = []
        for _ in range(nstr):
            n, = struct.unpack_from('<I', d, p); self.strings.append(d[p+4:p+4+n].decode('latin-1')); p += 4 + n
        ngroups, = struct.unpack_from('<I', d, p); p += 4 + 4 * ngroups
        self.boff = []
        for s in self.bsize:
            self.boff.append(p); p += s

    def type(self, i):
        return self.types[self.btype[i] & 0x7FFF]

    def name(self, i, off=0):
        idx, = struct.unpack_from('<i', self.d, self.boff[i] + off)
        return self.strings[idx] if 0 <= idx < len(self.strings) else ''

    # BSLightingShaderProperty (BS < 130): u32 shader type, NiObjectNET (name, extra data, controller), flags1,
    # flags2, uv offset/scale, texture set, emissive colour (3f), emissive multiple, ...
    def lighting(self, i):
        o = self.boff[i]
        stype, = struct.unpack_from('<I', self.d, o)
        n_extra, = struct.unpack_from('<I', self.d, o + 8)
        q = o + 12 + 4 * n_extra + 4
        return {'type': stype, 'flags_at': q, 'emit_at': q + 8 + 16 + 4}

    # BSEffectShaderProperty: NiObjectNET, flags1, flags2, uv offset/scale, source texture (sized string), clamp u8,
    # lighting influence u8, env map min lod u8, unused u8, falloff 4f, base colour 4f, base colour scale f, ...
    def effect(self, i):
        o = self.boff[i]
        n_extra, = struct.unpack_from('<I', self.d, o + 4)
        q = o + 8 + 4 * n_extra + 4
        tex = q + 8 + 16
        n, = struct.unpack_from('<I', self.d, tex)
        after = tex + 4 + n
        colour = after + 4 + 16
        return {'flags_at': q, 'texture': self.d[tex+4:tex+4+n].decode('latin-1'), 'colour_at': colour}

    def save(self, path):
        open(path, 'wb').write(self.d)


if __name__ == '__main__':
    n = Nif(sys.argv[1])
    print('версия %x user %d bs %d, блоков %d' % (n.version, n.user, n.bsver, len(n.bsize)))
    for i in range(len(n.bsize)):
        t = n.type(i)
        line = '%3d %-28s %6d' % (i, t, n.bsize[i])
        if t in ('NiNode', 'BSTriShape', 'BSFadeNode', 'NiTriShape', 'BSLODTriShape', 'BSDynamicTriShape'):
            line += '  "%s"' % n.name(i)
        elif t == 'BSLightingShaderProperty':
            L = n.lighting(i)
            f1, f2 = struct.unpack_from('<II', n.d, L['flags_at'])
            e = struct.unpack_from('<4f', n.d, L['emit_at'])
            line += '  "%s" тип %d flags %08x/%08x emissive %.2f %.2f %.2f x%.2f' % (n.name(i, 4), L['type'], f1, f2, *e)
        elif t == 'BSEffectShaderProperty':
            E = n.effect(i)
            c = struct.unpack_from('<5f', n.d, E['colour_at'])
            line += '  "%s" tex %s цвет %.2f %.2f %.2f %.2f x%.2f' % (n.name(i), E['texture'], *c)
        print(line)


def add_addon_nodes(src, dst, indices):
    """copy of a nif with BSValueNode "AddOnNode<i>" children under the root (as the Centurion dynamo core's
    AddOnNode47): the game hangs the ADDN effect of that index there (MPS particles: the aura / mist)"""
    n = Nif(src)
    d = n.d
    # header pieces
    p = d.index(b'\n') + 1 + 4 + 1 + 4
    nblocks_at = p
    types = list(n.types)
    if 'BSValueNode' not in types:
        types.append('BSValueNode')
    tval = types.index('BSValueNode')
    strings = list(n.strings)
    new_blocks = []
    first = len(n.bsize)
    for k, item in enumerate(indices):
        idx, pos = (item, (0.0, 0.0, 0.0)) if isinstance(item, int) else (item[0], item[1:])
        nm = 'AddOnNode%d' % idx
        if nm not in strings:
            strings.append(nm)
        b = struct.pack('<iIiI3f9ffi', strings.index(nm), 0, -1, 0x8000e, *pos,
                        1, 0, 0, 0, 1, 0, 0, 0, 1, 1.0, -1)
        b += struct.pack('<II', 0, 0) + struct.pack('<iB', idx, 0)
        new_blocks.append(b)
    # root: add the children refs
    ro = n.boff[0]
    root = bytearray(d[ro:ro + n.bsize[0]])
    ne, = struct.unpack_from('<I', root, 4)
    q = 8 + 4 * ne + 4
    struct.pack_into('<I', root, q, struct.unpack_from('<I', root, q)[0] | 0x80000)   # as the dynamo core's root
    q += 4 + 12 + 36 + 4 + 4
    nc, = struct.unpack_from('<I', root, q)
    struct.pack_into('<I', root, q, nc + len(indices))
    ins = q + 4 + 4 * nc
    root[ins:ins] = b''.join(struct.pack('<i', first + k) for k in range(len(indices)))
    # rebuild
    blocks = [bytes(root)] + [bytes(d[n.boff[i]:n.boff[i] + n.bsize[i]]) for i in range(1, len(n.bsize))] + new_blocks
    for i in range(len(n.bsize)):                               # BSX: + Animated (1) + Addon (0x10), as the dynamo core
        if n.type(i) == 'BSXFlags':
            b = bytearray(blocks[i])
            struct.pack_into('<I', b, 4, struct.unpack_from('<I', b, 4)[0] | 0x11)
            blocks[i] = bytes(b)
    btype = list(n.btype) + [tval] * len(new_blocks)
    # locate the header span to replace: from ntypes to the end of groups
    hp = nblocks_at + 4 + 4
    for _ in range(3):
        hp += 1 + d[hp]
    if n.bsver >= 103:
        hp += 1 + d[hp]
    head = bytearray(d[:hp])
    struct.pack_into('<I', head, nblocks_at, len(blocks))
    h = bytearray()
    h += struct.pack('<H', len(types))
    for t in types:
        h += struct.pack('<I', len(t)) + t.encode()
    h += struct.pack('<%dH' % len(btype), *btype)
    h += struct.pack('<%dI' % len(blocks), *[len(b) for b in blocks])
    h += struct.pack('<II', len(strings), max(len(s) for s in strings))
    for s in strings:
        h += struct.pack('<I', len(s)) + s.encode('latin-1')
    h += struct.pack('<I', 0)
    footer = d[n.boff[-1] + n.bsize[-1]:]
    open(dst, 'wb').write(bytes(head) + bytes(h) + b''.join(blocks) + bytes(footer))


def _rebuild(n, blocks, btype, types, strings, dst):
    d = n.d
    nblocks_at = d.index(b'\n') + 1 + 4 + 1 + 4
    hp = nblocks_at + 4 + 4
    for _ in range(3):
        hp += 1 + d[hp]
    if n.bsver >= 103:
        hp += 1 + d[hp]
    head = bytearray(d[:hp])
    struct.pack_into('<I', head, nblocks_at, len(blocks))
    h = bytearray(struct.pack('<H', len(types)))
    for t in types:
        h += struct.pack('<I', len(t)) + t.encode()
    h += struct.pack('<%dH' % len(btype), *btype)
    h += struct.pack('<%dI' % len(blocks), *[len(b) for b in blocks])
    h += struct.pack('<II', len(strings), max(len(s) for s in strings))
    for s in strings:
        h += struct.pack('<I', len(s)) + s.encode('latin-1')
    h += struct.pack('<I', 0)
    footer = d[n.boff[-1] + n.bsize[-1]:]
    open(dst, 'wb').write(bytes(head) + bytes(h) + b''.join(blocks) + bytes(footer))


def add_glow_shells(src, dst, mist_nif, shells, shape=None, parent=0):
    """copy of a nif with glowing shells of its mesh: for each (rgb, scale, strength) a copy of the shape scaled up
    around its centre with the mist's BSEffectShaderProperty (MagicCaustic, falloff, additive) in that colour.
    Unlike addon nodes this is part of the model, so the inventory / book menu shows it too."""
    n, m = Nif(src), Nif(mist_nif)
    types, strings = list(n.types), list(n.strings)
    for t in ('BSEffectShaderProperty', 'NiAlphaProperty', 'BSEffectShaderPropertyFloatController',
              'NiFloatInterpolator', 'NiFloatData'):
        if t not in types:
            types.append(t)
    shape = shape if shape is not None else next(i for i in range(len(n.bsize)) if n.type(i) == 'BSTriShape')
    fx = next(i for i in range(len(m.bsize)) if m.type(i) == 'BSEffectShaderProperty')
    al = next(i for i in range(len(m.bsize)) if m.type(i) == 'NiAlphaProperty')
    blocks = [bytes(n.d[n.boff[i]:n.boff[i] + n.bsize[i]]) for i in range(len(n.bsize))]
    btype = list(n.btype)
    new_children = []
    for k, shell in enumerate(shells):
        rgb, scale, strength = shell[:3]
        falloff = shell[3] if len(shell) > 3 else True
        base = len(blocks)
        sb = bytearray(blocks[shape])
        nm = 'SSWGlow%d' % k
        strings.append(nm)
        struct.pack_into('<i', sb, 0, len(strings) - 1)
        ne, = struct.unpack_from('<I', sb, 4)
        p = 8 + 4 * ne + 4 + 4
        cx, cy, cz, r = struct.unpack_from('<4f', sb, p + 12 + 36 + 4 + 4)
        # scale around the bounding centre: translation = c - c*scale
        struct.pack_into('<3f', sb, p, cx * (1 - scale), cy * (1 - scale), cz * (1 - scale))
        struct.pack_into('<f', sb, p + 12 + 36, scale)
        q = p + 12 + 36 + 4 + 4 + 16
        struct.pack_into('<3i', sb, q, -1, base + 1, base + 2)          # no skin, our shader, our alpha
        eb = bytearray(m.d[m.boff[fx]:m.boff[fx] + m.bsize[fx]])
        E = m.effect(fx)
        off = E['colour_at'] - m.boff[fx]
        struct.pack_into('<3f', eb, off, *rgb)
        struct.pack_into('<f', eb, off + 16, strength)
        if not falloff:                                  # SLSF1 bit 6 Use_Falloff off: the whole shell glows, not only its rim
            fo = E['flags_at'] - m.boff[fx]
            struct.pack_into('<I', eb, fo, struct.unpack_from('<I', eb, fo)[0] & ~0x40)
        ab = bytes(m.d[m.boff[al]:m.boff[al] + m.bsize[al]])
        # animation in the model itself (runs in the inventory / menus too): the mist texture flows (V offset) and
        # the glow breathes (emissive multiple); two controllers chained on the shader
        shader_i, c1, c2 = base + 1, base + 3, base + 6
        ne_e, = struct.unpack_from('<I', eb, 4)
        struct.pack_into('<i', eb, 8 + 4 * ne_e, c1)                   # NiObjectNET controller
        period = 3.0 + k
        def ctrl(nxt, interp, var, stop):
            return struct.pack('<iHffffiiI', nxt, 0x8, 1.0, 0.0, 0.0, stop, shader_i, interp, var)
        def fdata(keys):
            return struct.pack('<II', len(keys), 1) + b''.join(struct.pack('<ff', t, v) for t, v in keys)
        flow = [(0.0, 0.0), (period, 1.0 if k % 2 == 0 else -1.0)]
        pulse = [(0.0, strength * 0.6), (period / 2, strength * 1.4), (period, strength * 0.6)]
        blocks += [bytes(sb), bytes(eb), ab,
                   ctrl(c2, c1 + 1, 8, period), struct.pack('<fi', 0.0, c1 + 2), fdata(flow),
                   ctrl(-1, c2 + 1, 0, period), struct.pack('<fi', strength, c2 + 2), fdata(pulse)]
        tc, ti, td = (types.index(x) for x in ('BSEffectShaderPropertyFloatController', 'NiFloatInterpolator', 'NiFloatData'))
        btype += [n.btype[shape], types.index('BSEffectShaderProperty'), types.index('NiAlphaProperty'),
                  tc, ti, td, tc, ti, td]
        new_children.append(base)
    root = bytearray(blocks[parent])                     # the shells hang beside the shape (its own node: same motion)
    ne, = struct.unpack_from('<I', root, 4)
    q = 8 + 4 * ne + 4 + 4 + 12 + 36 + 4 + 4
    nc, = struct.unpack_from('<I', root, q)
    struct.pack_into('<I', root, q, nc + len(new_children))
    ins = q + 4 + 4 * nc
    root[ins:ins] = b''.join(struct.pack('<i', c) for c in new_children)
    blocks[parent] = bytes(root)
    _rebuild(n, blocks, btype, types, strings, dst)


def _texset_paths(b):
    """BSShaderTextureSet block -> its texture paths"""
    n, = struct.unpack_from('<I', b, 0)
    p, out = 4, []
    for _ in range(n):
        L, = struct.unpack_from('<I', b, p)
        out.append(b[p + 4:p + 4 + L].decode('latin-1'))
        p += 4 + L
    return out


def _parents(n):
    """block index -> parent NiNode index (children lists of every NiNode-like block)"""
    par = {}
    for i in range(len(n.bsize)):
        if n.type(i) not in ('NiNode', 'BSFadeNode', 'BSLeafAnimNode', 'BSOrderedNode', 'BSValueNode', 'NiBillboardNode'):
            continue
        b = n.d[n.boff[i]:n.boff[i] + n.bsize[i]]
        ne, = struct.unpack_from('<I', b, 4)
        q = 8 + 4 * ne + 4 + 4 + 12 + 36 + 4 + 4
        nc, = struct.unpack_from('<I', b, q)
        for c in struct.unpack_from('<%di' % nc, b, q + 4):
            par[c] = i
    return par


def split_glow(src, dst, template):
    """Glow-mapped shapes (BSLightingShaderProperty type 2: on the Switch the diffuse of that shader comes out black)
    become two: the shape with the plain shader (type 0, no glow map) and a copy (its skin blocks copied too) drawn
    with a BSEffectShaderProperty of the glow map — additive, like the vanilla daedric glow shapes. -> count"""
    n, m = Nif(src), Nif(template)
    types, strings = list(n.types), list(n.strings)
    for t in ('BSEffectShaderProperty', 'NiAlphaProperty'):
        if t not in types:
            types.append(t)
    blocks = [bytearray(n.d[n.boff[i]:n.boff[i] + n.bsize[i]]) for i in range(len(n.bsize))]
    btype = list(n.btype)
    fx = next(i for i in range(len(m.bsize)) if m.type(i) == 'BSEffectShaderProperty')
    al = next(i for i in range(len(m.bsize)) if m.type(i) == 'NiAlphaProperty')
    fx_t = bytes(m.d[m.boff[fx]:m.boff[fx] + m.bsize[fx]])
    al_t = bytes(m.d[m.boff[al]:m.boff[al] + m.bsize[al]])
    par = _parents(n)
    adds = {}                                                  # parent -> new children
    count = 0
    for s in range(len(n.bsize)):
        if n.type(s) not in ('BSTriShape', 'BSSubIndexTriShape', 'BSDynamicTriShape'):
            continue
        sb = blocks[s]
        ne, = struct.unpack_from('<I', sb, 4)
        q = 8 + 4 * ne + 4 + 4 + 12 + 36 + 4 + 4 + 16
        skin, shader, alpha = struct.unpack_from('<3i', sb, q)
        if shader < 0 or n.type(shader) != 'BSLightingShaderProperty':
            continue
        L = n.lighting(shader)
        o = n.boff[shader]
        if L['type'] != 2:
            continue
        f1, f2 = struct.unpack_from('<II', n.d, L['flags_at'])
        if not f2 & 0x40:
            continue
        emit = struct.unpack_from('<4f', n.d, L['emit_at'])
        ts, = struct.unpack_from('<i', n.d, L['flags_at'] + 8 + 16)
        paths = _texset_paths(bytes(n.d[n.boff[ts]:n.boff[ts] + n.bsize[ts]])) if ts >= 0 else []
        glow = paths[2] if len(paths) > 2 else ''
        if not glow:
            continue
        # a glow layer of its own (a copy of the armour drawn with a mostly black "GLOW" diffuse and alpha): it
        # becomes the effect shape itself; a shape that is both the armour and its glow: plain shader + a glow copy
        # (02.10: only a black "...glow.dds" layer is an overlay; an alpha-tested armour / blade is not)
        overlay = alpha >= 0 and (paths[0].lower().endswith('glow.dds') if paths else True)
        if not overlay:
            lb = blocks[shader]
            struct.pack_into('<I', lb, 0, 0)
            rel = L['flags_at'] - o
            struct.pack_into('<II', lb, rel, f1 & ~0x00400000, f2 & ~0x40)   # no Own_Emit: only the glow layer glows
            struct.pack_into('<4f', lb, L['emit_at'] - o, 0.0, 0.0, 0.0, 0.0)
        base = len(blocks)
        new = []
        cs = sb if overlay else bytearray(sb)
        new_skin = skin if overlay else -1
        if skin >= 0 and not overlay:
            kb = bytearray(blocks[skin])
            data_i, part_i = struct.unpack_from('<ii', kb, 0)
            new_skin = base
            new.append((bytearray(kb), btype[skin]))
            nxt = base + 1
            if data_i >= 0:
                new.append((bytearray(blocks[data_i]), btype[data_i]))
                struct.pack_into('<i', new[0][0], 0, nxt); nxt += 1
            if part_i >= 0:
                new.append((bytearray(blocks[part_i]), btype[part_i]))
                struct.pack_into('<i', new[0][0], 4, nxt); nxt += 1
        fx_i = base + len(new)
        e = bytearray(fx_t)
        en, = struct.unpack_from('<I', e, 4)
        fq = 8 + 4 * en + 4
        struct.pack_into('<II', e, fq, 0x80000000 | (0x2 if skin >= 0 else 0), f2 & 0x20)
        struct.pack_into('<4f', e, fq + 8, 0.0, 0.0, 1.0, 1.0)            # uv offset / scale as the shape's
        tl, = struct.unpack_from('<I', e, fq + 24)
        tex = glow if glow.lower().startswith('textures') else 'textures' + chr(92) + glow
        e = e[:fq + 24] + struct.pack('<I', len(tex)) + tex.encode('latin-1') + e[fq + 28 + tl:]
        after = fq + 28 + len(tex)
        e[after] = 3                                                      # clamp: wrap s, wrap t
        e[after + 1] = 0                                                  # lighting influence: none
        struct.pack_into('<4f', e, after + 4, 1.0, 1.0, 1.0, 1.0)        # falloff: none
        r, g, b_, mult = emit
        if r + g + b_ < 0.05:
            r = g = b_ = 1.0
        struct.pack_into('<4f', e, after + 20, r, g, b_, 1.0)
        struct.pack_into('<f', e, after + 36, max(1.0, min(mult, 4.0)))
        new.append((e, types.index('BSEffectShaderProperty')))
        al_i = base + len(new)
        new.append((bytearray(al_t), types.index('NiAlphaProperty')))
        struct.pack_into('<3i', cs, q, new_skin, fx_i, al_i)
        if not overlay:
            nm, = struct.unpack_from('<i', sb, 0)
            strings.append((strings[nm] if 0 <= nm < len(strings) else 'Shape') + ':SSWGlow%d' % count)
            struct.pack_into('<i', cs, 0, len(strings) - 1)
            copy_i = base + len(new)
            new.append((cs, btype[s]))
            adds.setdefault(par.get(s, 0), []).append(copy_i)
        for blk, t in new:
            blocks.append(blk)
            btype.append(t)
        count += 1
    for p, kids in adds.items():
        nb = blocks[p]
        ne, = struct.unpack_from('<I', nb, 4)
        q = 8 + 4 * ne + 4 + 4 + 12 + 36 + 4 + 4
        nc, = struct.unpack_from('<I', nb, q)
        struct.pack_into('<I', nb, q, nc + len(kids))
        ins = q + 4 + 4 * nc
        nb[ins:ins] = b''.join(struct.pack('<i', c) for c in kids)
    if count:
        _rebuild(n, [bytes(b) for b in blocks], btype, types, strings, dst)
    return count


def add_overlay(src, dst, template, tex_of, colour, mult, falloff=False, shader_types=(0, 1), pulse=0.0):
    """every lit shape (BSLightingShaderProperty) gets a copy drawn with a BSEffectShaderProperty — additive, the
    texture tex_of(texture set paths) (None: that shape gets none) in colour x mult; skinned shapes get copies of
    their skin blocks. The originals are untouched. -> count of overlays"""
    n, m = Nif(src), Nif(template)
    types, strings = list(n.types), list(n.strings)
    for t in ('BSEffectShaderProperty', 'NiAlphaProperty', 'BSEffectShaderPropertyFloatController',
              'NiFloatInterpolator', 'NiFloatData'):
        if t not in types:
            types.append(t)
    blocks = [bytearray(n.d[n.boff[i]:n.boff[i] + n.bsize[i]]) for i in range(len(n.bsize))]
    btype = list(n.btype)
    fx = next(i for i in range(len(m.bsize)) if m.type(i) == 'BSEffectShaderProperty')
    al = next(i for i in range(len(m.bsize)) if m.type(i) == 'NiAlphaProperty')
    fx_t = bytes(m.d[m.boff[fx]:m.boff[fx] + m.bsize[fx]])
    al_t = bytes(m.d[m.boff[al]:m.boff[al] + m.bsize[al]])
    par = _parents(n)
    adds, count = {}, 0
    for s in range(len(n.bsize)):
        if n.type(s) not in ('BSTriShape', 'BSSubIndexTriShape', 'BSDynamicTriShape'):
            continue
        sb = blocks[s]
        ne, = struct.unpack_from('<I', sb, 4)
        q = 8 + 4 * ne + 4 + 4 + 12 + 36 + 4 + 4 + 16
        skin, shader, alpha = struct.unpack_from('<3i', sb, q)
        if shader < 0 or n.type(shader) != 'BSLightingShaderProperty':
            continue
        L = n.lighting(shader)
        if L['type'] not in shader_types:                     # skin, face, hair...: no glow
            continue
        f1, f2 = struct.unpack_from('<II', n.d, L['flags_at'])
        ts, = struct.unpack_from('<i', n.d, L['flags_at'] + 8 + 16)
        paths = _texset_paths(bytes(n.d[n.boff[ts]:n.boff[ts] + n.bsize[ts]])) if ts >= 0 else []
        tex = tex_of(paths)
        if not tex:
            continue
        base = len(blocks)
        new = []
        new_skin = -1
        if skin >= 0:
            kb = bytearray(blocks[skin])
            data_i, part_i = struct.unpack_from('<ii', kb, 0)
            new_skin = base
            new.append((kb, btype[skin]))
            nxt = base + 1
            if data_i >= 0:
                new.append((bytearray(blocks[data_i]), btype[data_i]))
                struct.pack_into('<i', kb, 0, nxt); nxt += 1
            if part_i >= 0:
                new.append((bytearray(blocks[part_i]), btype[part_i]))
                struct.pack_into('<i', kb, 4, nxt); nxt += 1
        fx_i = base + len(new)
        e = bytearray(fx_t)
        en, = struct.unpack_from('<I', e, 4)
        fq = 8 + 4 * en + 4
        struct.pack_into('<II', e, fq, 0x80000000 | (0x2 if skin >= 0 else 0) | (0x40 if falloff else 0), f2 & 0x20)
        struct.pack_into('<4f', e, fq + 8, 0.0, 0.0, 1.0, 1.0)
        tl, = struct.unpack_from('<I', e, fq + 24)
        e = e[:fq + 24] + struct.pack('<I', len(tex)) + tex.encode('latin-1') + e[fq + 28 + tl:]
        after = fq + 28 + len(tex)
        e[after] = 3
        e[after + 1] = 0                                      # lighting influence: none, it glows in the dark too
        struct.pack_into('<4f', e, after + 4, 1.0, 1.0, 1.0, 1.0)
        struct.pack_into('<4f', e, after + 20, colour[0], colour[1], colour[2], 1.0)
        struct.pack_into('<f', e, after + 36, mult)
        new.append((e, types.index('BSEffectShaderProperty')))
        al_i = base + len(new)
        new.append((bytearray(al_t), types.index('NiAlphaProperty')))
        if pulse:
            # the glow breathes: emissive multiple mult*0.45 -> mult*1.25 -> back, a cycle of `pulse` seconds (the
            # same controller chain as add_glow_shells, which the game loads)
            c_i = base + len(new)
            struct.pack_into('<i', e, 8 + 4 * en, c_i)                    # NiObjectNET controller
            new.append((bytearray(struct.pack('<iHffffiiI', -1, 0x8, 1.0, 0.0, 0.0, pulse, fx_i, c_i + 1, 0)),
                        types.index('BSEffectShaderPropertyFloatController')))
            new.append((bytearray(struct.pack('<fi', mult, c_i + 2)), types.index('NiFloatInterpolator')))
            keys = [(0.0, mult * 0.45), (pulse / 2, mult * 1.25), (pulse, mult * 0.45)]
            new.append((bytearray(struct.pack('<II', len(keys), 1) + b''.join(struct.pack('<ff', t, v) for t, v in keys)),
                        types.index('NiFloatData')))
        cs = bytearray(sb)
        struct.pack_into('<3i', cs, q, new_skin, fx_i, al_i)
        nm, = struct.unpack_from('<i', sb, 0)                 # its own name: the armour loader skips same-named shapes
        strings.append((strings[nm] if 0 <= nm < len(strings) else 'Shape') + ':SSWGlow%d' % count)
        struct.pack_into('<i', cs, 0, len(strings) - 1)
        copy_i = base + len(new)
        new.append((cs, btype[s]))
        for blk, t in new:
            blocks.append(blk)
            btype.append(t)
        adds.setdefault(par.get(s, 0), []).append(copy_i)
        count += 1
    for p, kids in adds.items():
        nb = blocks[p]
        ne, = struct.unpack_from('<I', nb, 4)
        q = 8 + 4 * ne + 4 + 4 + 12 + 36 + 4 + 4
        nc, = struct.unpack_from('<I', nb, q)
        struct.pack_into('<I', nb, q, nc + len(kids))
        ins = q + 4 + 4 * nc
        nb[ins:ins] = b''.join(struct.pack('<i', c) for c in kids)
    if count:
        _rebuild(n, [bytes(b) for b in blocks], btype, types, strings, dst)
    return count
