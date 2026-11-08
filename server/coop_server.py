"""SkyrimCoop server: the room the consoles join over the local network.

    python coop_server.py [--name "Комната"] [--port 27015] [--mirror]
    (or SkyrimCoopServer.exe — the same server with a window, see app.py)

Consoles find the room by themselves (UDP "SKYCOOP?" broadcast on the same port); an address can also be
typed in the game's co-op menu. Every player gets an id; STATE / EVENT / CHAT / APPEARANCE / EQUIP from one
player go to all others, and the last ones are kept so that a newcomer sees everybody at once.

Local control (127.0.0.1:port+1, see ctl.py): "[@who] press a 200 | stick lx ly rx ry ms | cmd <console command>"
and "say <text>", "kick <who>", "list"; who = id, name or "all".
"""
import argparse, asyncio, collections, math, os, socket, struct, sys, time
import actors as actorcore
from protocol import *
from storage import Store
import saves
import updates
from social import Social

WORLDS = {0x0000003C: 'Скайрим', 0x0001A26F: 'Вайтран'}   # known worldspaces; interiors have world 0


def local_addresses():
    if os.environ.get('SKYCOOP_ADDRESS'):          # Docker: the PC's own address, the container cannot see it
        return [a.strip() for a in os.environ['SKYCOOP_ADDRESS'].split(',') if a.strip()]
    ips = set()
    try:
        ips |= {ai[4][0] for ai in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)}
    except OSError:
        pass
    try:                                           # the address used to reach the LAN
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(('192.168.255.255', 1))
        ips.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    def rank(ip):                                  # home Wi-Fi first; VPN / WSL / Docker adapters last
        a, b, c, d = (int(x) for x in ip.split('.'))
        virtual = d == 1 or (a == 172 and 16 <= b <= 31)
        return (virtual, 0 if a == 192 and b == 168 else 1 if a == 10 else 2, ip)
    return sorted((ip for ip in ips if not ip.startswith('127.')), key=rank)


class Player:
    def __init__(self, pid, name, writer, addr):
        self.id, self.name, self.writer, self.addr = pid, name, writer, addr
        self.joined = time.time()
        self.state = None          # STATE payload (with our id)
        self.appearance = None     # (flags, bytes)
        self.equipment = None      # (count, item bytes)
        self.last_print = 0.0
        self.uploads = {}          # file name -> saves.Upload in progress
        self.zone = None           # the world zone this player is in (see Server.zone_of)
        self.avatar_up = None      # the avatar coming in by chunks
        self.plugins = None        # load order (list of file names) as the console reported it
        self.stats = None          # STATS payload (with our id): level, health / magicka / stamina %
        self.tele = ''             # TELE: the module's text picture of the game (test tools)

    def place(self):
        if not self.state:
            return 'загружается'
        f = STATE_S.unpack(self.state)
        if not f[3]:
            return 'загружается'
        where = WORLDS.get(f[2], 'мир %08X' % f[2]) if f[2] else 'помещение %08X' % f[3]
        flags = [n for n, b in (('идёт', FLAG_MOVING), ('с оружием', FLAG_WEAPON_DRAWN), ('мёртв', FLAG_DEAD)) if f[8] & b]
        return '%s (%.0f, %.0f, %.0f)%s' % (where, f[4], f[5], f[6], ', ' + ', '.join(flags) if flags else '')


class Server:
    def __init__(self, name='Skyrim', port=PORT, mirror=False, log=print, verbose=False, record=None, store=None):
        self.store = store or Store()
        self.actors = actorcore.Actors(self)           # (08.10) the actor core, Skyrim Together model
        self.name, self.port, self.mirror, self.verbose = name, port, mirror, verbose
        self._print = log
        self.lines = collections.deque(maxlen=1000)        # (seq, text) for the web panel
        self.chat = collections.deque(maxlen=200)          # (time, who, text)
        self.seq = 0
        self.started = time.time()
        self.record = open(record, 'ab') if record else None     # what players send, for replay_client.py
        self.record_t0 = time.time()
        self.players = {}          # id -> Player
        self.next_id = 1
        self.banned = self.store.banned      # names kicked (kept on disk)
        self.killed = self.store.killed      # FormIDs of plugin actors killed (shared world, kept on disk)
        self.zone_owner = {}                 # zone -> player id owning its NPCs (MMO-style world ownership)
        self.zone_npcs = {}                  # zone -> last NPC states from its owner (formId, x, y, z, rot, speed, flags)
        self.follow = {}                     # follower FormID -> leader player id (this session)
        self.social = Social(self.store, lambda: [p.name for p in self.players.values()], self.say)
        self.social.on_join = lambda name: [self.send_quests(p) for p in self.players.values() if p.name == name]
        self.tcp = None
        self.transports = []

    # ---------------------------------------------------------------- helpers
    def log(self, text):
        self.seq += 1
        self.lines.append((self.seq, text))
        self._print(text)
        # (07.10) the journal on disk too (logs/<date>.log in the data folder): a console crash can be read afterwards
        if not text.startswith('  '):                        # not the position lines printed every 2 s
            try:
                d = os.path.join(self.store.dir, 'logs')
                os.makedirs(d, exist_ok=True)
                with open(os.path.join(d, time.strftime('%Y%m%d') + '.log'), 'a', encoding='utf-8') as f:
                    f.write(text + chr(10))
            except Exception:  # noqa: BLE001
                pass

    def stamp(self):
        return time.strftime('%H:%M:%S')

    def send(self, player, ptype, payload=b''):
        try:
            player.writer.write(pack(ptype, payload))
        except (ConnectionError, RuntimeError):
            pass

    def broadcast(self, sender_id, ptype, payload):
        for p in list(self.players.values()):
            if p.id != sender_id:
                self.send(p, ptype, payload)

    def targets(self, who):
        if who in (None, '', 'all', 'все'):
            return list(self.players.values())
        out = [p for p in self.players.values() if str(p.id) == who or p.name.lower() == who.lower()]
        return out

    def say(self, text, who=None):
        data = CHAT_HDR.pack(0) + text.encode('utf-8')[:190]
        n = 0
        for p in self.targets(who):
            self.send(p, CHAT, data)
            n += 1
        self.chat.append((time.time(), 'Сервер', text))
        self.log('[%s] сообщение (%d): %s' % (self.stamp(), n, text))
        return n

    def kick(self, who):
        n = 0
        for p in self.targets(who):
            self.banned.add(p.name.lower())
            self.send(p, CHAT, CHAT_HDR.pack(0) + 'Вас отключили от комнаты'.encode('utf-8'))
            p.writer.close()
            n += 1
        self.store.save_world()
        return n

    def check_plugins(self, me):
        """Everybody needs the same mods in the same order (FormIDs of mod things depend on it)."""
        ref = next((p for p in self.players.values() if p is not me and p.plugins), None)
        if ref is None:
            self.log('[%s] %s: модов %d (%s)' % (self.stamp(), me.name, len(me.plugins), ', '.join(me.plugins[:12])))
            return
        missing = [x for x in ref.plugins if x not in me.plugins]
        extra = [x for x in me.plugins if x not in ref.plugins]
        order = not missing and not extra and ref.plugins != me.plugins
        if not (missing or extra or order):
            self.log('[%s] %s: моды совпадают с %s (%d)' % (self.stamp(), me.name, ref.name, len(me.plugins)))
            return
        text = 'Моды не совпадают с %s:' % ref.name
        if missing:
            text += ' нет у тебя: ' + ', '.join(missing[:6])
        if extra:
            text += ' лишние: ' + ', '.join(extra[:6])
        if order:
            text += ' другой порядок загрузки'
        self.log('[%s] %s: %s' % (self.stamp(), me.name, text))
        self.send(me, CHAT, CHAT_HDR.pack(0) + text.encode('utf-8')[:190])
        self.send(ref, CHAT, CHAT_HDR.pack(0) + ('У %s другие моды — возможны ошибки' % me.name).encode('utf-8')[:190])

    def quest_book(self, player):
        """The campaign's quest progress: one for everybody on the server (03.10, no guilds)."""
        return self.store.quests, list(self.players.values()), 'кампания'

    def quest_progress(self, me, qid, stage):
        book, members, who = self.quest_book(me)
        if book is None or stage <= int(book.get(str(qid), book.get(qid, -1))):
            return
        book[qid] = stage
        self.store.save_world()
        self.log('[%s] %s: квест %08X — стадия %d (%s)' % (self.stamp(), me.name, qid, stage, who))
        for p in members:
            if p is not me:
                self.send(p, QUEST, QUEST_S.pack(qid, stage, me.id))

    # ---------------------------------------------------------------- companions (03.10)
    def companion_leader(self, fid):
        """The id of the companion's owner if online, else FOLLOW_HOME."""
        owner = self.store.companions.get(fid)
        for p in self.players.values():
            if p.name == owner:
                return p.id
        return FOLLOW_HOME

    def companion_request(self, me, fid, leader):
        """A companion is one player's: recruited by the first, the others can talk / trade / dress it, not lead it.
        leader = me.id: take it; 0: let it go (its owner only)."""
        owner = self.store.companions.get(fid)
        if leader == me.id:
            if owner and owner != me.name:
                cur = self.companion_leader(fid)
                self.send(me, FOLLOW, FOLLOW_S.pack(fid, cur, 0))        # undo it on the asker's console
                self.send(me, CHAT, CHAT_HDR.pack(0) + ('Этот спутник уже в отряде у %s — с тобой не пойдёт' % owner).encode('utf-8')[:190])
                self.log('[%s] %s: спутник %08X занят (%s)' % (self.stamp(), me.name, fid, owner))
                return
            self.store.companions[fid] = me.name
            self.log('[%s] %s: спутник %08X теперь его' % (self.stamp(), me.name, fid))
        elif leader == 0:
            if owner != me.name:
                self.send(me, FOLLOW, FOLLOW_S.pack(fid, self.companion_leader(fid) if owner else 0, 0))
                return
            self.store.companions.pop(fid, None)
            self.log('[%s] %s: отпустил спутника %08X' % (self.stamp(), me.name, fid))
        else:
            self.send(me, FOLLOW, FOLLOW_S.pack(fid, self.companion_leader(fid) if owner else 0, 0))
            return                                                       # nobody leads someone else's companion
        self.store.save_world()
        self.broadcast(me.id, FOLLOW, FOLLOW_S.pack(fid, leader, 0))

    def send_quests(self, me):
        """Everything the campaign has done: a newcomer gets it — and the stages' rewards — at once."""
        book, _, _ = self.quest_book(me)
        for qid, stage in (book or {}).items():
            self.send(me, QUEST, QUEST_S.pack(int(qid), int(stage), 0))

    # ---------------------------------------------------------------- zones (world ownership)
    @staticmethod
    def zone_of(state):
        f = STATE_S.unpack(state)
        if not f[3]:
            return None                                              # loading
        if f[2]:                                                     # outdoors: a square of the worldspace
            return ('w', f[2], int(f[4] // ZONE_OUTDOOR), int(f[5] // ZONE_OUTDOOR))
        return ('c', f[3])

    @staticmethod
    def zone_name(zone):
        if zone[0] == 'c':
            return 'помещение %08X' % zone[1]
        return '%s, квадрат %d:%d' % (WORLDS.get(zone[1], 'мир %08X' % zone[1]), zone[2], zone[3])

    def set_zone(self, me, zone):
        if zone == me.zone:
            return
        old, me.zone = me.zone, zone
        for z in (old, zone):
            if z is not None:
                self.elect(z)

    def elect(self, zone):
        """The zone's owner stays while there; otherwise the longest-connected player in it (lowest id)."""
        ids = sorted(p.id for p in self.players.values() if p.zone == zone)
        owner = self.zone_owner.get(zone)
        if not ids:
            self.zone_owner.pop(zone, None)
            return
        if owner not in ids:
            owner = ids[0]
            self.zone_owner[zone] = owner
            self.log('[%s] %s — хозяин зоны %s' % (self.stamp(), self.players[owner].name, self.zone_name(zone)))
        for p in self.players.values():
            if p.zone == zone:
                self.send(p, ZONE, ZONE_S.pack(owner, 0))

    def unban(self, name):
        self.banned.discard(name.lower())
        self.store.save_world()

    def revive(self, fid=None):
        """Forget kills (one FormID or all): the actors stay dead on consoles that already killed them,
        newcomers get them alive."""
        if fid is None:
            self.killed.clear()
        elif fid in self.killed:
            self.killed.remove(fid)
        self.store.save_world()

    def unban_all(self):
        self.banned.clear()
        self.store.save_world()

    # ---------------------------------------------------------------- connection
    async def handle(self, reader, writer):
        peer = writer.get_extra_info('peername') or ('?', 0)
        sock = writer.get_extra_info('socket')
        if sock is not None:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        me = None
        try:
            ptype, payload = await asyncio.wait_for(read_packet(reader), 10)
            if ptype != HELLO or len(payload) != HELLO_S.size:
                return
            version, raw = HELLO_S.unpack(payload)
            name = raw.split(b'\0', 1)[0].decode('utf-8', 'replace') or 'Игрок'
            if version != VERSION or name.lower() in self.banned:   # WELCOME with id 0 = refused
                self.log('[%s] %s с %s: %s — отказ' % (self.stamp(), name, peer[0],
                         'версия мода %d, у сервера %d' % (version, VERSION) if version != VERSION else 'выгнан'))
                writer.write(pack(WELCOME, WELCOME_S.pack(0, VERSION)))
                await writer.drain()
                try:                                  # let the console read it before the connection goes
                    await asyncio.wait_for(reader.read(-1), 3)
                except asyncio.TimeoutError:
                    pass
                return
            if any(p.name == name for p in self.players.values()):     # two "Игрок" -> "Игрок 2"
                k = 2
                while any(p.name == '%s %d' % (name, k) for p in self.players.values()):
                    k += 1
                name = '%s %d' % (name, k)
            me = Player(self.next_id, name, writer, peer[0])
            self.next_id += 1
            self.players[me.id] = me
            self.send(me, WELCOME, WELCOME_S.pack(me.id, VERSION))
            for p in self.players.values():                       # everybody's name, incl. own
                self.send(me, JOIN, JOIN_S.pack(p.id, p.name.encode('utf-8')[:32]))
            for p in self.players.values():
                if p is me:
                    continue
                if p.state:
                    self.send(me, STATE, p.state)
                if p.appearance:
                    self.send(me, APPEARANCE, APPEAR_S.pack(p.id, 0, p.appearance[0]) + p.appearance[1])
                if p.equipment:
                    self.send(me, EQUIP, EQUIP_HDR.pack(p.id, p.equipment[0]) + p.equipment[1])
                if p.stats:
                    self.send(me, STATS, p.stats)
                if self.mirror and p.appearance:                  # test: the newcomer looks like them
                    self.send(p, APPEARANCE, APPEAR_S.pack(me.id, 0, p.appearance[0]) + p.appearance[1])
            self.send(me, WORLD, WORLD_HDR.pack(WK_CAMPAIGN, 0, self.store.campaign_id))   # first: which saves belong here
            self.send_clock(me)                                  # (08.10) the campaign's game clock
            for fid in self.killed:
                self.send(me, KILL, KILL_S.pack(0, 0, fid))
            for ref, items in self.store.containers.items():   # the world's containers as they are now
                self.send(me, WORLD, WORLD_HDR.pack(WK_CONTAINER, len(items), ref) +
                          b''.join(struct.pack('<Ii', f, n) for f, n in items))
            for ref, bits in self.store.doors.items():       # doors: open / locked as the world has them
                self.send(me, WORLD, WORLD_HDR.pack(WK_DOOR, bits, ref))
            for fid in self.store.revived:                   # their console may still hold it dead
                self.send(me, KILL, KILL_S.pack(0, KILL_REVIVE, fid))
            self.send_quests(me)
            for iid, body in self.store.ground.items():     # things lying on the ground
                self.send(me, WORLD, WORLD_HDR.pack(WK_ITEM, 1, iid) + body)
            for fid in self.store.companions:                 # every companion: with its owner, or at home
                self.send(me, FOLLOW, FOLLOW_S.pack(fid, self.companion_leader(fid), 0))
            for fid, owner in self.store.companions.items():  # ours come back to us; the others see them come
                if owner == me.name:
                    self.broadcast(me.id, FOLLOW, FOLLOW_S.pack(fid, me.id, 0))
            await writer.drain()
            self.log('[%s] + %s (id %d) с %s, игроков: %d' % (self.stamp(), name, me.id, peer[0], len(self.players)))
            self.broadcast(me.id, JOIN, JOIN_S.pack(me.id, name.encode('utf-8')[:32]))

            while True:
                ptype, payload = await read_packet(reader)
                if self.record and ptype in (STATE, APPEARANCE, EQUIP, EVENT, CHAT, ACTION):
                    self.record.write(REC_S.pack(time.time() - self.record_t0, me.id, ptype, len(payload)) + payload)
                    self.record.flush()
                if ptype == STATE and len(payload) == STATE_S.size:
                    payload = LEFT_S.pack(me.id) + payload[2:]           # the real sender id
                    me.state = payload
                    self.set_zone(me, self.zone_of(payload))
                    if self.verbose and time.time() - me.last_print >= 2:
                        me.last_print = time.time()
                        self.log('  %s: %s' % (me.name, me.place()))
                elif ptype in (NPC_STATE, NPC_ACTION) and len(payload) >= 4:
                    if me.zone is None or self.zone_owner.get(me.zone) != me.id:
                        continue                                     # only the zone owner's NPCs are real
                    if ptype == NPC_STATE:                            # the world's NPCs as the owner sees them
                        n = (len(payload) - NPC_HDR.size) // NPC_SIZE
                        self.zone_npcs[me.zone] = [struct.unpack_from('<IfffffH', payload, NPC_HDR.size + i * NPC_SIZE)
                                                   for i in range(n)]
                    for p in list(self.players.values()):
                        if p is not me and p.zone == me.zone:
                            self.send(p, ptype, payload)
                    continue
                elif ptype == TELE:                                   # test tools: the latest picture, kept
                    me.tele = payload.decode('utf-8', 'replace')
                    continue
                elif ptype == LOG:                                    # the module's own log line
                    self.log('[%s] %s: %s' % (self.stamp(), me.name, payload.decode('utf-8', 'replace')))
                    continue
                elif ptype == APPEARANCE and len(payload) > APPEAR_S.size:
                    _, _, fl = APPEAR_S.unpack_from(payload)
                    me.appearance = (fl, payload[APPEAR_S.size:])
                    self.log('[%s] %s: внешность %d байт' % (self.stamp(), me.name, len(me.appearance[1])))
                    if self.mirror:
                        for p in self.players.values():
                            if p is not me and not p.appearance:
                                self.send(me, APPEARANCE, APPEAR_S.pack(p.id, 0, fl) + me.appearance[1])
                    payload = APPEAR_S.pack(me.id, 0, fl) + me.appearance[1]
                elif ptype == EQUIP and len(payload) >= EQUIP_HDR.size:
                    _, count = EQUIP_HDR.unpack_from(payload)
                    items = payload[EQUIP_HDR.size:]
                    if len(items) != count * 8:
                        continue
                    me.equipment = (count, items)
                    self.log('[%s] %s: снаряжение %s' % (self.stamp(), me.name, ' '.join(
                        '%08X%s' % (struct.unpack_from('<I', items, i * 8)[0], 'L' if items[i * 8 + 4] else '') for i in range(count))))
                    if self.mirror:
                        for p in self.players.values():
                            if p is not me and not p.equipment:
                                self.send(me, EQUIP, EQUIP_HDR.pack(p.id, count) + items)
                    payload = EQUIP_HDR.pack(me.id, count) + items
                elif ptype == CHAT and len(payload) > CHAT_HDR.size:
                    text = payload[CHAT_HDR.size:].decode('utf-8', 'replace')
                    self.chat.append((time.time(), me.name, text))
                    self.log('[%s] %s: %s' % (self.stamp(), me.name, text))
                    payload = CHAT_HDR.pack(me.id) + payload[CHAT_HDR.size:]
                elif ptype == KILL and len(payload) == KILL_S.size:
                    _, kflags, fid = KILL_S.unpack(payload)
                    if fid in self.killed:
                        continue
                    self.killed.append(fid)
                    self.store.kill_meta[fid] = [time.time(), bool(kflags & KILL_RESPAWNS)]
                    self.store.revived.pop(fid, None)
                    self.store.save_world()
                    self.log('[%s] %s: убит %08X' % (self.stamp(), me.name, fid))
                    payload = KILL_S.pack(me.id, 0, fid)
                elif ptype == WORLD and len(payload) >= WORLD_HDR.size:
                    kind, count, ref = WORLD_HDR.unpack_from(payload)
                    if kind == WK_ITEM:                          # a thing on the ground: dropped / taken
                        body = payload[WORLD_HDR.size:WORLD_HDR.size + 28]
                        if count & 1 and len(body) == 28:
                            self.store.ground[ref] = body
                        else:
                            self.store.ground.pop(ref, None)
                        self.store.save_world()
                        self.broadcast(me.id, WORLD, WORLD_HDR.pack(WK_ITEM, count & 1, ref) + (body if count & 1 else b''))
                        self.log('[%s] %s: предмет %08X %s' % (self.stamp(), me.name, ref, 'брошен' if count & 1 else 'поднят'))
                        continue
                    if kind == WK_ACTIVATE:                      # a lever / chain / button: used for the others too
                        if (ref >> 24) != 0xFF:
                            self.broadcast(me.id, WORLD, WORLD_HDR.pack(WK_ACTIVATE, me.id, ref))
                            self.log('[%s] %s: активирован %08X' % (self.stamp(), me.name, ref))
                        continue
                    if kind == WK_MARK:                          # a map marker for the whole party
                        self.broadcast(me.id, WORLD, WORLD_HDR.pack(WK_MARK, (me.id << 1) | (count & 1), ref) +
                                       payload[WORLD_HDR.size:WORLD_HDR.size + 12])
                        self.log('[%s] %s: метка на карте %s' % (self.stamp(), me.name, 'поставлена' if count & 1 else 'снята'))
                        continue
                    if kind in (WK_SOUL, WK_WORD):              # a dragon soul / a word for the party around
                        for p in list(self.players.values()):
                            if p is not me and me.zone is not None and p.zone == me.zone:
                                self.send(p, WORLD, WORLD_HDR.pack(kind, me.id, ref))
                        self.log('[%s] %s: %s %08X — всем рядом' % (self.stamp(), me.name, 'душа дракона' if kind == WK_SOUL else 'слово силы', ref))
                        continue
                    if kind == WK_SAY:                          # an NPC's line to this player: heard in its zone
                        text = payload[WORLD_HDR.size:WORLD_HDR.size + min(count, 200)]
                        self.log('[%s] %s слышит: %s' % (self.stamp(), me.name, text.decode('utf-8', 'replace')))
                        for p in list(self.players.values()):
                            if p is not me and me.zone is not None and p.zone == me.zone:
                                self.send(p, WORLD, WORLD_HDR.pack(WK_SAY, len(text), me.id) + text)
                        continue
                    if kind == WK_DOOR and (ref >> 24) != 0xFF:
                        self.store.doors[ref] = count & 3
                        self.store.save_world()
                        self.log('[%s] %s: дверь %08X — %s%s' % (self.stamp(), me.name, ref, 'открыта' if count & 1 else 'закрыта',
                                                                  ', заперта' if count & 2 else ''))
                    elif kind != WK_CONTAINER or WORLD_HDR.size + count * 8 > len(payload) or (ref >> 24) == 0xFF:
                        continue
                    if kind == WK_CONTAINER:
                        items = [list(struct.unpack_from('<Ii', payload, WORLD_HDR.size + i * 8)) for i in range(count)]
                        self.store.containers[ref] = items
                        self.store.container_time[ref] = time.time()
                        self.store.save_world()
                        self.log('[%s] %s: контейнер %08X — %d изменений' % (self.stamp(), me.name, ref, count))
                elif ptype == CLOCK and len(payload) == CLOCK_S.size:
                    if not self.store.clock:                     # the campaign's clock starts as the first player's
                        h, sc, d, m, y = CLOCK_S.unpack(payload)
                        if 0 <= h < 24 and 0 < sc < 1000:
                            self.store.clock = {'hour': h, 'scale': sc, 'day': d, 'month': m, 'year': y, 'at': time.time()}
                            self.store.save_world()
                            self.log('[%s] часы кампании начаты от %s: %.2f ч, день %d, месяц %d' % (self.stamp(), me.name, h, d, m + 1))
                            for p in list(self.players.values()):
                                self.send_clock(p)
                    continue
                elif ptype == TIME and len(payload) == TIME_S.size:
                    if self.store.settings['time_mode'] == 'fixed' or me.id != min(self.players):
                        continue                                     # the room host keeps the clock
                    payload = LEFT_S.pack(me.id) + payload[2:]
                elif ptype == SAVE_PUT and len(payload) > SAVE_HDR.size:
                    total, off, raw = SAVE_HDR.unpack_from(payload)
                    fname = raw.split(b'\0', 1)[0].decode('utf-8', 'replace')
                    if not saves.valid(fname) or total > 64 << 20:
                        continue
                    up = me.uploads.get(fname)
                    if up is None or off == 0:
                        up = me.uploads[fname] = saves.Upload(me.name, fname, total)
                    if up.write(off, payload[SAVE_HDR.size:]):
                        del me.uploads[fname]
                        saves.prune(me.name)
                        self.log('[%s] %s: сохранение на сервере — %s (%d КБ)' % (self.stamp(), me.name, fname, total // 1024))
                    continue
                elif ptype == SAVE_LIST_REQ:
                    lines = ''.join('%s\t%d\t%s\n' % x for x in saves.listing(me.name)[:8])
                    self.send(me, SAVE_LIST, lines.encode('utf-8')[:4000])
                    continue
                elif ptype == SAVE_GET and payload:
                    fname = payload.decode('utf-8', 'replace')
                    data = saves.read(me.name, fname)
                    if data is None:
                        continue
                    raw = fname.encode('utf-8')[:95]
                    for off in range(0, len(data), SAVE_CHUNK):
                        self.send(me, SAVE_DATA, SAVE_HDR.pack(len(data), off, raw) + data[off:off + SAVE_CHUNK])
                        await writer.drain()
                    self.log('[%s] %s: скачал сохранение %s' % (self.stamp(), me.name, fname))
                    continue
                elif ptype == UPD_LIST_REQ:
                    self.send(me, UPD_LIST, updates.listing(self.store).encode('utf-8')[:8000])
                    continue
                elif ptype == UPD_GET and payload:
                    rel = payload.decode('utf-8', 'replace')
                    data = updates.read(self.store, rel)
                    if data is None:
                        continue
                    raw = rel.encode('utf-8')[:159]
                    for off in range(0, max(len(data), 1), SAVE_CHUNK):
                        self.send(me, UPD_DATA, UPD_HDR.pack(len(data), off, raw) + data[off:off + SAVE_CHUNK])
                        await writer.drain()
                    self.log('[%s] %s: обновление %s' % (self.stamp(), me.name, rel))
                    continue
                elif ptype == AVATAR_PUT and len(payload) >= UPD_HDR.size:
                    total, off, _ = UPD_HDR.unpack_from(payload)
                    if total > AVATAR_MAX:
                        continue
                    if off == 0 or me.avatar_up is None or len(me.avatar_up) != total:
                        me.avatar_up = bytearray(total)
                    chunk = payload[UPD_HDR.size:]
                    me.avatar_up[off:off + len(chunk)] = chunk
                    if off + len(chunk) >= total:
                        saves.avatar_write(me.name, bytes(me.avatar_up))
                        me.avatar_up = None
                        self.log('[%s] %s: персонаж сохранён на сервере (%d КБ)' % (self.stamp(), me.name, (total + 1023) // 1024))
                    continue
                elif ptype == AVATAR_GET:
                    data = saves.avatar_read(me.name) or b''
                    for off in range(0, max(len(data), 1), SAVE_CHUNK):
                        self.send(me, AVATAR_DATA, UPD_HDR.pack(len(data), off, b'avatar') + data[off:off + SAVE_CHUNK])
                        await writer.drain()
                    self.log('[%s] %s: %s' % (self.stamp(), me.name, 'персонаж загружен с сервера' if data else 'персонажа на сервере нет'))
                    continue
                elif ptype == SOCIAL:
                    answer = self.social.run(me.name, payload.decode('utf-8', 'replace'))
                    self.send(me, SOCIAL_INFO, answer.encode('utf-8')[:1500])
                    continue
                elif ptype == NPC_DAMAGE and len(payload) == DAMAGE_S.size:
                    fid, amount = DAMAGE_S.unpack(payload)
                    if not 0 < amount < 100000 or (fid >> 24) == 0xFF or fid in self.killed:
                        continue
                elif ptype == PLUGINS:
                    me.plugins = [x for x in payload.decode('utf-8', 'replace').splitlines() if x]
                    self.check_plugins(me)
                    continue
                elif self.actors.handle(me, ptype, payload):
                    continue
                elif ptype == QUEST and len(payload) == QUEST_S.size:
                    qid, stage, _ = QUEST_S.unpack(payload)
                    self.quest_progress(me, qid, stage)
                    continue
                elif ptype == FOLLOW and len(payload) == FOLLOW_S.size:
                    fid, leader, _ = FOLLOW_S.unpack(payload)
                    self.companion_request(me, fid, leader)
                    continue
                elif ptype == HIT and len(payload) == HIT_S.size:
                    victim, _, amount = HIT_S.unpack(payload)
                    target = self.players.get(victim)
                    if self.store.settings.get('pvp') and target and target is not me and 0 < amount < 10000:
                        self.send(target, HIT, HIT_S.pack(me.id, 0, amount))
                        self.log('[%s] %s ударил %s: %.0f' % (self.stamp(), me.name, target.name, amount))
                    continue
                elif ptype == GIVE and len(payload) == GIVE_S.size:
                    to, _, fid, cnt = GIVE_S.unpack(payload)
                    target = self.players.get(to)
                    if target and target is not me:
                        self.send(target, GIVE, GIVE_S.pack(me.id, 0, fid, cnt))
                        self.log('[%s] %s -> %s: %d x %08X' % (self.stamp(), me.name, target.name, cnt, fid))
                    continue
                elif ptype == STATS and len(payload) == STATS_S.size:
                    payload = LEFT_S.pack(me.id) + payload[2:]
                    me.stats = payload
                elif ptype == ACTION and len(payload) == ACTION_SIZE:
                    payload = LEFT_S.pack(me.id) + payload[2:]
                elif ptype == EVENT and len(payload) >= 2:
                    if payload[EVENT_HDR.size:].startswith(b'coopweather') and me.id != min(self.players):
                        continue                                     # the room host's sky only
                    payload = LEFT_S.pack(me.id) + payload[2:]
                else:
                    continue
                self.broadcast(me.id, ptype, payload)
                await writer.drain()
        except (asyncio.IncompleteReadError, asyncio.TimeoutError, ConnectionError, OSError):
            pass
        finally:
            if me is not None:
                self.players.pop(me.id, None)
                self.actors.player_left(me)
                self.set_zone(me, None)
                self.broadcast(me.id, LEFT, LEFT_S.pack(me.id))
                if not any(p.name == me.name for p in self.players.values()):
                    for fid, owner in self.store.companions.items():      # its owner left: home it goes
                        if owner == me.name:
                            self.broadcast(me.id, FOLLOW, FOLLOW_S.pack(fid, FOLLOW_HOME, 0))
                self.log('[%s] - %s (id %d), игроков: %d' % (self.stamp(), me.name, me.id, len(self.players)))
            writer.close()

    # ---------------------------------------------------------------- lifetime
    async def start(self, control=True):
        loop = asyncio.get_running_loop()
        self.tcp = await asyncio.start_server(self.handle, '0.0.0.0', self.port)
        t, _ = await loop.create_datagram_endpoint(lambda: Discovery(self), local_addr=('0.0.0.0', self.port),
                                                   allow_broadcast=True)
        self.transports.append(t)
        if control:
            try:
                t, _ = await loop.create_datagram_endpoint(lambda: Control(self), local_addr=('127.0.0.1', self.port + 1))
                self.transports.append(t)
            except OSError:
                pass
        self.clock = asyncio.ensure_future(self.fixed_clock())
        self.respawner = asyncio.ensure_future(self.respawn_loop())
        self.log('Комната "%s" открыта, порт %d. Консоли в этой сети найдут её сами; вручную: %s' % (
            self.name or (local_addresses() or ['?'])[0], self.port, ', '.join('%s:%d' % (ip, self.port) for ip in local_addresses()) or 'нет сети'))

    async def respawn_loop(self):
        """The world's own respawn (03.10): non-unique actors killed long enough ago come back for everybody."""
        while True:
            await asyncio.sleep(30)
            self.respawn_due()

    def respawn_due(self, now=None):
        now = now or time.time()
        hours = float(self.store.settings.get('respawn_hours', 3.6))
        back = [fid for fid in self.killed if (m := self.store.kill_meta.get(fid)) and m[1] and now - m[0] >= hours * 3600]
        for fid in back:
            self.killed.remove(fid)
            self.store.kill_meta.pop(fid, None)
            self.store.revived[fid] = now
            for p in list(self.players.values()):
                self.send(p, KILL, KILL_S.pack(0, KILL_REVIVE, fid))
        # looted containers come back with the enemies (the dungeon reset); a container somebody put things into
        # (a stash) stays as it is
        refill = [ref for ref, items in self.store.containers.items()
                  if items and all(n < 0 for _, n in items) and now - self.store.container_time.get(ref, now) >= hours * 3600]
        for ref in refill:
            del self.store.containers[ref]
            self.store.container_time.pop(ref, None)
            for p in list(self.players.values()):
                self.send(p, WORLD, WORLD_HDR.pack(WK_CONTAINER, 0, ref))
        if refill:
            self.log('[%s] лут вернулся в контейнеры: %d' % (self.stamp(), len(refill)))
        old = [fid for fid, t in self.store.revived.items() if now - t > 10 * 86400]
        for fid in old:
            del self.store.revived[fid]
        if back or old or refill:
            self.store.save_world()
        if back:
            self.log('[%s] вернулись в мир: %d (сброс через %.1f ч)' % (self.stamp(), len(back), hours))
        return back

    MONTH_DAYS = (31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31)

    def clock_now(self):
        """(08.10, the Skyrim Together way) the campaign's game clock as it is now: it runs on the server at its time
        scale (game hours = real seconds * scale / 3600); "fixed" mode: that hour, standing."""
        c = self.store.clock
        st = self.store.settings
        if not c:
            return None
        if st['time_mode'] == 'fixed':
            return (float(st['fixed_hour']), 0.0, c['day'], c['month'], c['year'])
        hour = c['hour'] + (time.time() - c['at']) * c['scale'] / 3600.0
        day, month, year = c['day'], c['month'], c['year']
        while hour >= 24.0:
            hour -= 24.0
            day += 1
            if day > self.MONTH_DAYS[int(month) % 12]:
                day, month = 1, month + 1
                if month > 11:
                    month, year = 0, year + 1
        return (hour, c['scale'], day, month, year)

    def send_clock(self, p):
        now = self.clock_now()
        if now:
            self.send(p, CLOCK, CLOCK_S.pack(*now))

    async def fixed_clock(self):
        """The game clock to everybody every 10 s (and kept in world.json once a minute)."""
        n = 0
        while True:
            await asyncio.sleep(10)
            now = self.clock_now()
            if not now:
                continue
            for p in list(self.players.values()):
                self.send(p, CLOCK, CLOCK_S.pack(*now))
            n += 1
            if n % 6 == 0 and self.store.settings['time_mode'] != 'fixed':
                h, sc, d, m, y = now
                self.store.clock = {'hour': h, 'scale': sc, 'day': d, 'month': m, 'year': y, 'at': time.time()}
                self.store.save_world()

    async def stop(self):
        if getattr(self, 'clock', None):
            self.clock.cancel()
        if getattr(self, 'respawner', None):
            self.respawner.cancel()
        for p in list(self.players.values()):
            p.writer.close()
        for t in self.transports:
            t.close()
        self.transports.clear()
        if self.tcp:
            self.tcp.close()
            await self.tcp.wait_closed()
            self.tcp = None
        self.log('Комната закрыта')


class Discovery(asyncio.DatagramProtocol):
    """Room search: "SKYCOOP?" -> "SKYCOOP!" + u16 TCP port + room name."""

    def __init__(self, srv):
        self.srv = srv

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        if data[:8] == DISCOVER_Q:
            self.transport.sendto(DISCOVER_A + struct.pack('<H', self.srv.port) + self.srv.name.encode('utf-8')[:40], addr)


class Control(asyncio.DatagramProtocol):
    """Local control for tests and tools, 127.0.0.1 only (see the module docstring)."""

    def __init__(self, srv):
        self.srv = srv

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        if addr[0] != '127.0.0.1':
            return
        try:
            reply = self.run(data.decode('utf-8').split())
        except Exception as e:  # noqa: BLE001
            reply = 'error: %s' % e
        self.transport.sendto(reply.encode('utf-8')[:1400], addr)

    def run(self, cmd):
        who = None
        if cmd and cmd[0].startswith('@'):
            who, cmd = cmd[0][1:], cmd[1:]
        srv = self.srv
        if cmd[0] == 'list':
            return '\n'.join('%d %s %s%s' % (p.id, p.name, p.place(), ' курс %.0f' % math.degrees(STATE_S.unpack(p.state)[7])
                                                     if p.state else '') for p in srv.players.values()) or 'пусто'
        if cmd[0] == 'tele':                                       # tele [@who]: the module's picture of the game
            ps = [p for p in srv.targets(who) if p.tele]
            return ps[0].tele if ps else 'нет'
        if cmd[0] == 'state':                                      # state: id name world cell x y z heading flags (tools)
            out = []
            for p in srv.players.values():
                if p.state:
                    f = STATE_S.unpack(p.state)
                    out.append('%d|%s|%08X|%08X|%.0f|%.0f|%.0f|%.1f|%d' % (p.id, p.name, f[2], f[3], f[4], f[5], f[6],
                                                                         math.degrees(f[7]), f[8]))
            return chr(10).join(out) or 'пусто'
        if cmd[0] == 'owner' and len(cmd) > 1:                    # owner <name|id>: that player owns their zone
            ps = srv.targets(' '.join(cmd[1:]))
            if not ps or ps[0].zone is None:
                return 'нет такого игрока в мире'
            srv.zone_owner[ps[0].zone] = ps[0].id
            srv.elect(ps[0].zone)
            return 'хозяин зоны: %s' % ps[0].name
        if cmd[0] == 'npcs':                                       # npcs: the owners' NPC lists (test aid)
            return chr(10).join('%s %08X %s %.0f %.0f %.0f' % (z, n[0], 'мёртв' if n[6] & 4 else 'жив', n[1], n[2], n[3])
                             for z, ns in srv.zone_npcs.items() for n in ns) or 'пусто'
        if cmd[0] == 'respawn':                                    # respawn: due actors come back now (test aid)
            for fid in srv.killed:
                if fid in srv.store.kill_meta:
                    srv.store.kill_meta[fid][0] = 0
            return 'вернулись: %s' % ' '.join('%08X' % f for f in srv.respawn_due())
        if cmd[0] == 'say':
            return 'sent to %d' % srv.say(' '.join(cmd[1:]), who)
        if cmd[0] == 'kick':
            return 'kicked %d' % srv.kick(cmd[1])
        if cmd[0] == 'press':
            mask = 0
            for b in cmd[1].split('+'):
                mask |= 1 << BUTTONS[b]
            ptype, payload = INPUT, INPUT_S.pack(mask, 0, 0, 0, 0, int(cmd[2]))
        elif cmd[0] == 'stick':
            lx, ly, rx, ry, ms = map(int, cmd[1:6])
            ptype, payload = INPUT, INPUT_S.pack(0, lx, ly, rx, ry, ms)
        elif cmd[0] == 'pad':                                       # pad <buttons|-> lx ly rx ry ms: all at once
            mask = 0
            for b in cmd[1].split('+'):
                if b != '-':
                    mask |= 1 << BUTTONS[b]
            lx, ly, rx, ry, ms = map(int, cmd[2:7])
            ptype, payload = INPUT, INPUT_S.pack(mask, lx, ly, rx, ry, ms)
        elif cmd[0] == 'cmd':
            ptype, payload = CMD, ' '.join(cmd[1:]).encode('utf-8')[:199]
        else:
            raise ValueError(cmd[0])
        ps = srv.targets(who)
        for p in ps:
            srv.send(p, ptype, payload)
        return 'sent to %d' % len(ps)


async def run_cli(name, port, mirror, verbose, record, pvp=False):
    import tempfile                                 # tests: a throw-away world, not the real server's
    srv = Server(name, port, mirror, print, verbose, record, store=Store(tempfile.mkdtemp(prefix='skycoop_test_')))
    srv.store.settings['pvp'] = pvp
    await srv.start()
    await asyncio.Event().wait()


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8', line_buffering=True)
    except AttributeError:
        pass
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=PORT)
    ap.add_argument('--name', default='', help='room name shown in the game (empty: its address)')
    ap.add_argument('--mirror', action='store_true', help='test: fake players get the look of the real one')
    ap.add_argument('--quiet', action='store_true', help='do not print positions every 2 s')
    ap.add_argument('--pvp', action='store_true', help='players can hurt each other')
    ap.add_argument('--record', help='append what players send to this file (replay with replay_client.py)')
    a = ap.parse_args()
    asyncio.run(run_cli(a.name, a.port, a.mirror, not a.quiet, a.record, a.pvp))
