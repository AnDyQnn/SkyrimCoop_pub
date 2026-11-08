"""Game / mod updates for the consoles: files put into <data dir>/updates, laid out as on the SD card root
(e.g. updates/atmosphere/contents/0100E9401F4A4000/exefs/subsdk9). The consoles compare size + CRC32 and
download what differs ("Проверить обновления" in the online panel)."""
import os
import zlib

_cache = {}                                   # path -> (mtime, size, crc)


def folder(store):
    d = os.path.join(store.dir, 'updates')
    os.makedirs(d, exist_ok=True)
    return d


def manifest(store):
    root = folder(store)
    out = []
    for base, _, files in os.walk(root):
        for f in sorted(files):
            full = os.path.join(base, f)
            rel = os.path.relpath(full, root).replace(os.sep, '/')
            st = os.stat(full)
            hit = _cache.get(full)
            if not hit or hit[0] != st.st_mtime or hit[1] != st.st_size:
                with open(full, 'rb') as fh:
                    hit = _cache[full] = (st.st_mtime, st.st_size, zlib.crc32(fh.read()) & 0xFFFFFFFF)
            out.append((rel, hit[1], hit[2]))
    return sorted(out)


def listing(store):
    return ''.join('%s\t%d\t%08x\n' % x for x in manifest(store))


def read(store, rel):
    if any(r == rel for r, _, _ in manifest(store)):
        with open(os.path.join(folder(store), *rel.split('/')), 'rb') as fh:
            return fh.read()
    return None
