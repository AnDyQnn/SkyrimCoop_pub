"""Server data on disk: settings, the shared world state and the ban list survive a restart.

Folder: $SKYCOOP_DATA, else %APPDATA%\\SkyrimCoopServer (Windows), else ~/.skyrimcoop (Docker: /data).
Files are JSON, written atomically (temp file + rename).
"""
import json, os, random, threading

DEFAULT_SETTINGS = {
    'name': 'Skyrim',          # room name shown on the consoles
    'port': 27015,
    'web_port': 8080,
    'web_password': '',        # empty = the panel is open to the local network
    'time_mode': 'host',       # host: the first player's clock; fixed: 'fixed_hour' for everybody
    'fixed_hour': 12.0,
    'pvp': False,              # players can hurt each other
    'shared_quests': False,    # a quest stage reached by one player is set for everybody
    'respawn_hours': 3.6,      # non-unique dead actors come back after this many real hours (3 game days at x20)
}


def data_dir():
    d = os.environ.get('SKYCOOP_DATA')
    if not d:
        d = os.path.join(os.environ['APPDATA'], 'SkyrimCoopServer') if os.environ.get('APPDATA') else \
            os.path.join(os.path.expanduser('~'), '.skyrimcoop')
    os.makedirs(d, exist_ok=True)
    return d


class Store:
    def __init__(self, directory=None):
        self.dir = directory or data_dir()
        self.lock = threading.Lock()
        self.settings = dict(DEFAULT_SETTINGS, **self._load('settings.json', {}))
        world = self._load('world.json', {})
        self.killed = [int(x) for x in world.get('killed', [])]     # plugin actor FormIDs (dead now)
        # 03.10, the server owns the world: per kill [unix time, respawns]; revived: FormID -> time (newcomers resurrect)
        self.kill_meta = {int(k): v for k, v in world.get('kill_meta', {}).items()}
        self.revived = {int(k): v for k, v in world.get('revived', {}).items()}
        # container / corpse FormID -> [[form, delta], ...] (world.cpp on the consoles brings their copies to it)
        self.containers = {int(k): v for k, v in world.get('containers', {}).items()}
        self.container_time = {int(k): v for k, v in world.get('container_time', {}).items()}
        self.doors = {int(k): v for k, v in world.get('doors', {}).items()}
        self.companions = {int(k): v for k, v in world.get('companions', {}).items()}
        self.ground = {int(k): bytes.fromhex(v) for k, v in world.get('ground', {}).items()}   # item id -> body (things on the ground)   # follower FormID -> owner name        # door FormID -> bits (open 1, locked 2)
        self.banned = set(world.get('banned', []))                  # lower-case player names
        self.social = world.get('social', {})                       # clans, marriages (social.py)
        self.quests = {int(k): v for k, v in world.get('quests', {}).items()}   # quest FormID -> highest stage
        # 04.10: the campaign's number — the saves of this campaign carry it (SSW_CampaignId); a save without it is a
        # player's own and does not sync. A new number = a new campaign (a float in the save: up to 2^24 exactly)
        self.clock = world.get('clock')     # (08.10) {hour, scale, day, month, year, at}: the campaign's game clock
        self.campaign_id = int(world.get('campaign_id', 0)) or random.randint(1, 16000000)
        if not world.get('campaign_id'):
            self.save_world()

    def _load(self, name, default):
        try:
            with open(os.path.join(self.dir, name), encoding='utf-8') as f:
                return json.load(f)
        except (OSError, ValueError):
            return default

    def _save(self, name, data):
        path = os.path.join(self.dir, name)
        with self.lock:
            with open(path + '.tmp', 'w', encoding='utf-8') as f:
                json.dump(data, f, ensure_ascii=False, indent=1)
            os.replace(path + '.tmp', path)

    def save_settings(self):
        self._save('settings.json', self.settings)

    def save_world(self):
        self._save('world.json', {'killed': self.killed, 'banned': sorted(self.banned), 'social': self.social,
                                  'kill_meta': {str(k): v for k, v in self.kill_meta.items()},
                                  'revived': {str(k): v for k, v in self.revived.items()},
                                  'containers': {str(k): v for k, v in self.containers.items()},
                                  'doors': {str(k): v for k, v in self.doors.items()},
                                  'companions': {str(k): v for k, v in self.companions.items()},
                                  'ground': {str(k): v.hex() for k, v in self.ground.items()},
                                  'container_time': {str(k): v for k, v in self.container_time.items()},
                                      'quests': {str(k): v for k, v in self.quests.items()},
                                      'clock': self.clock,
                                      'campaign_id': self.campaign_id})
