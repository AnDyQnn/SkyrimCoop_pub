"""Online saves: each player's save files kept on the server (storage dir / saves / <player name>)."""
import os, re, time
from storage import data_dir

SAFE = re.compile(r'^[\w\-. ]{1,95}$')


def player_dir(name):
    d = os.path.join(data_dir(), 'saves', re.sub(r'[\/:*?"<>|]', '_', name.strip().lower()) or '_')
    os.makedirs(d, exist_ok=True)
    return d


def avatar_path(name):
    d = os.path.join(data_dir(), 'avatars')
    os.makedirs(d, exist_ok=True)
    return os.path.join(d, (re.sub(r'[\/:*?"<>|]', '_', name.strip().lower()) or '_') + '.dat')


def avatar_read(name):
    p = avatar_path(name)
    return open(p, 'rb').read() if os.path.isfile(p) else None


def avatar_write(name, data):
    p = avatar_path(name)
    with open(p + '.part', 'wb') as f:
        f.write(data)
    os.replace(p + '.part', p)


def valid(fname):
    return bool(SAFE.match(fname)) and not fname.startswith('.')


class Upload:
    """One file coming in by chunks; written to <name>.part, renamed when complete."""

    def __init__(self, player, fname, total):
        self.path = os.path.join(player_dir(player), fname)
        self.total = total
        self.got = 0
        self.f = open(self.path + '.part', 'wb')
        self.f.truncate(total)

    def write(self, offset, data):
        self.f.seek(offset)
        self.f.write(data)
        self.got += len(data)
        if self.got >= self.total:
            self.f.close()
            os.replace(self.path + '.part', self.path)
            return True
        return False


def listing(player, keep=0):
    d = player_dir(player)
    files = sorted((f for f in os.listdir(d) if not f.endswith('.part')), reverse=True)   # names start with the date
    def when(f):                               # Skyrim names start with YYYYMMDDhhmmss
        m = re.match(r'(\d{4})(\d\d)(\d\d)(\d\d)(\d\d)', f)
        return '%s.%s %s:%s' % (m[3], m[2], m[4], m[5]) if m else             time.strftime('%d.%m %H:%M', time.localtime(os.path.getmtime(os.path.join(d, f))))
    return [(f, os.path.getsize(os.path.join(d, f)), when(f)) for f in files]


def prune(player, keep=20):
    """Only the newest `keep` saves of a player stay."""
    d = player_dir(player)
    for f, _, _ in listing(player)[keep:]:
        os.remove(os.path.join(d, f))


def read(player, fname):
    p = os.path.join(player_dir(player), fname)
    return open(p, 'rb').read() if valid(fname) and os.path.isfile(p) else None
