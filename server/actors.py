"""The actor core of the server, the Skyrim Together way (notes/ST_РАЗБОР.md, 08.10.2026).

Every actor a console has loaded (an NPC of the plugins, a creature, a temporary actor from a leveled list or a random
encounter) is an entry here with exactly one OWNER: the console where it is "alive". The owner runs its AI and sends
its state ten times a second; on every other console that has it loaded it is REMOTE — its own AI, walking and actions
are held back there and everything comes from the owner. An ownership EPOCH grows with every change of owner, so late
packets of an old owner are dropped.

  ACT_ASSIGN   c->s  a console loaded an actor: "who owns it?" (plugin ref by FormID; a temporary one by its base and
                     the leveled pick, it gets a new server id and the asker owns it)
  ACT_ASSIGNED s->c  the answer: server id, owner, epoch, the actor's state as the owner knows it
  ACT_SPAWN    s->c  a temporary actor someone owns, near you: make one like it
  ACT_MOVES    both  owner's snapshot of its actors (position, heading, animation, health); relayed to the interested
  ACT_ACTION   both  an action the owner's actor performed (ActorMediator::PerformAction), replayed on the others
  ACT_RELEASE  c->s  the actor was unloaded here
  ACT_OWNER    s->c  a new owner (the old one left / unloaded it)
  ACT_HEALTH   both  damage a non-owner did to it, applied by the owner
  ACT_DEATH    both  died / came back
  ACT_REMOVE   s->c  a temporary actor is gone
"""
import os, struct, time

ACT_ASSIGN, ACT_ASSIGNED, ACT_SPAWN, ACT_MOVES, ACT_ACTION, ACT_RELEASE, ACT_OWNER, ACT_HEALTH, ACT_DEATH, ACT_REMOVE = \
    range(50, 60)
ACT_PROJ = 61                               # both: a projectile launch (84 bytes, sid first); a player's to everybody
ACT_EQUIP = 60                              # both: sid u32, count u16, pad u16, then count x (form u32, slot u8, pad 3)

# cookie, ref, base, pick, cell, world, x, y, z, rot, dead, temp, pad
ASSIGN_S = struct.Struct('<IIIIIIffffBBH')
# cookie, sid, owner pid, you-own u8, dead u8, epoch, x, y, z, rot, health
ASSIGNED_S = struct.Struct('<IIHBBIfffff')
# sid, epoch, owner pid, dead u8, pad, base, pick, cell, world, x, y, z, rot
SPAWN_S = struct.Struct('<IIHBBIIIIffff')
# tick, count, pad — then count x MOVE_S
MOVES_HDR = struct.Struct('<IHH')
# sid, x, y, z, rot, cell, world, health, state1, state2, 8 graph floats, 8 graph ints, bools, bools present, has,
# magicka, stamina
MOVE_S = struct.Struct('<IffffIIfII8f8hIIIff')
ACT_PLAYER = 0x80000000
ROWS = 18
ACTION_SIZE = 64
SID_S = struct.Struct('<I')
RELEASE_S = struct.Struct('<II')            # sid, epoch
OWNER_S = struct.Struct('<IIHH')            # sid, epoch, owner pid, pad
HEALTH_S = struct.Struct('<If')             # sid, delta (to the owner) / new value
DEATH_S = struct.Struct('<IBBH')            # sid, dead, pad

SPAWN_RANGE = 12000.0                        # a temporary actor is made for players within this (units, same space)


class Actor:
    __slots__ = ('sid', 'ref', 'base', 'pick', 'cell', 'world', 'pos', 'rot', 'dead', 'health', 'owner', 'epoch',
                 'interest', 'temp', 'last_move', 'last_action', 'seen', 'equip')

    def __init__(self, sid, ref, base, pick, cell, world, pos, rot, dead, temp, owner):
        self.sid, self.ref, self.base, self.pick, self.cell, self.world = sid, ref, base, pick, cell, world
        self.pos, self.rot, self.dead, self.health, self.temp = pos, rot, dead, -1.0, temp
        self.owner, self.epoch, self.interest = owner, 1, {owner}
        self.last_move, self.last_action, self.seen, self.equip = None, None, time.time(), None


class Actors:
    def __init__(self, server):
        self.srv = server
        self.by_sid = {}
        self.by_ref = {}
        self.next_sid = 1

    # -------------------------------------------------------------- helpers
    def send(self, pid, ptype, payload):
        p = self.srv.players.get(pid)
        if p:
            self.srv.send(p, ptype, payload)

    def relay(self, a, ptype, payload, skip):
        for pid in list(a.interest):
            if pid != skip:
                self.send(pid, ptype, payload)

    def near(self, player, a):
        st = player.state
        if not st:
            return False
        _, _, world, cell, x, y, *_ = struct.unpack('<HHIIff', st[:20])
        if a.world == 0 or world == 0:
            return a.cell == cell
        if a.world != world:
            return False
        dx, dy = a.pos[0] - x, a.pos[1] - y
        return dx * dx + dy * dy < SPAWN_RANGE * SPAWN_RANGE

    def spawn_payload(self, a):
        return SPAWN_S.pack(a.sid, a.epoch, a.owner or 0, int(a.dead), 0, a.base, a.pick, a.cell, a.world, *a.pos, a.rot)

    def set_owner(self, a, pid, why):
        a.owner = pid
        a.epoch += 1
        payload = OWNER_S.pack(a.sid, a.epoch, pid or 0, 0)
        for q in list(a.interest):
            self.send(q, ACT_OWNER, payload)

    # -------------------------------------------------------------- packets
    def assign(self, me, payload):
        cookie, ref, base, pick, cell, world, x, y, z, rot, dead, temp, _ = ASSIGN_S.unpack(payload)
        claim = temp == 2 and os.environ.get('SKYCOOP_TEST') == '1'     # test tools: take it over
        temp = temp == 1
        a = self.by_sid.get(self.by_ref.get(ref)) if ref and not temp else None
        if a is None:
            a = Actor(self.next_sid, 0 if temp else ref, base, pick, cell, world, (x, y, z), rot, bool(dead), bool(temp),
                      me.id)
            self.next_sid += 1
            self.by_sid[a.sid] = a
            if a.ref:
                self.by_ref[a.ref] = a.sid
            if a.temp:                                      # made only here: the others near get one like it
                for p in self.srv.players.values():
                    if p is not me and self.near(p, a):
                        a.interest.add(p.id)
                        self.send(p.id, ACT_SPAWN, self.spawn_payload(a))
        else:
            a.interest.add(me.id)
            if a.owner is None or a.owner not in self.srv.players or (claim and a.owner != me.id):
                self.set_owner(a, me.id, 'новый')
        mine = a.owner == me.id
        self.send(me.id, ACT_ASSIGNED, ASSIGNED_S.pack(cookie, a.sid, a.owner or 0, int(mine), int(a.dead), a.epoch,
                                                       *a.pos, a.rot, a.health))
        if not mine and a.last_move:                        # where it is now, without waiting for the next snapshot
            self.send(me.id, ACT_MOVES, MOVES_HDR.pack(0, 1, 0) + a.last_move)
        if not mine and a.equip:                            # and what it wears
            self.send(me.id, ACT_EQUIP, a.equip)

    def moves(self, me, payload):
        if len(payload) < MOVES_HDR.size:
            return
        tick, count, _ = MOVES_HDR.unpack_from(payload)
        body = payload[MOVES_HDR.size:]
        if len(body) != count * MOVE_S.size:
            return
        out = {}
        for k in range(count):
            raw = body[k * MOVE_S.size:(k + 1) * MOVE_S.size]
            sid = SID_S.unpack_from(raw)[0]
            if sid & ACT_PLAYER:                          # the player's own body: to every other player
                if sid & 0xFFFF == me.id:
                    for p in self.srv.players.values():
                        if p is not me:
                            out.setdefault(p.id, []).append(raw)
                continue
            a = self.by_sid.get(sid)
            if not a or a.owner != me.id:
                continue
            v = MOVE_S.unpack(raw)
            a.pos, a.rot, a.cell, a.world, a.health, a.seen = (v[1], v[2], v[3]), v[4], v[5], v[6], v[7], time.time()
            a.last_move = raw
            for pid in a.interest:
                if pid != me.id:
                    out.setdefault(pid, []).append(raw)
            if a.temp:                                     # a player came near: one like it for them too
                for p in self.srv.players.values():
                    if p.id not in a.interest and self.near(p, a):
                        a.interest.add(p.id)
                        self.send(p.id, ACT_SPAWN, self.spawn_payload(a))
        for pid, rows in out.items():
            for i in range(0, len(rows), ROWS):            # the client's packet slots are 2 KB
                part = rows[i:i + ROWS]
                self.send(pid, ACT_MOVES, MOVES_HDR.pack(tick, len(part), 0) + b''.join(part))

    def action(self, me, payload):
        if len(payload) != 4 + ACTION_SIZE:
            return
        a = self.by_sid.get(SID_S.unpack_from(payload)[0])
        if a and a.owner == me.id:
            a.last_action = payload
            self.relay(a, ACT_ACTION, payload, me.id)

    def release(self, me, payload):
        if len(payload) != RELEASE_S.size:
            return
        sid, epoch = RELEASE_S.unpack(payload)
        a = self.by_sid.get(sid)
        if not a:
            return
        a.interest.discard(me.id)
        if a.owner == me.id:
            self.hand_over(a)

    def hand_over(self, a):
        others = [pid for pid in a.interest if pid in self.srv.players]
        if others:
            self.set_owner(a, others[0], 'ушёл владелец')
        elif a.temp:
            self.drop(a)
        else:
            a.owner = None

    def drop(self, a):
        for pid in list(a.interest):
            self.send(pid, ACT_REMOVE, SID_S.pack(a.sid))
        self.by_sid.pop(a.sid, None)
        if a.ref:
            self.by_ref.pop(a.ref, None)

    def health(self, me, payload):
        if len(payload) != HEALTH_S.size:
            return
        sid, delta = HEALTH_S.unpack(payload)
        a = self.by_sid.get(sid)
        if a and a.owner and a.owner != me.id:
            self.send(a.owner, ACT_HEALTH, payload)       # the owner applies it; its next snapshot carries the result

    def death(self, me, payload):
        if len(payload) != DEATH_S.size:
            return
        sid, dead, _, _ = DEATH_S.unpack(payload)
        a = self.by_sid.get(sid)
        if a:
            a.dead = bool(dead)
            self.relay(a, ACT_DEATH, payload, me.id)

    def equip(self, me, payload):
        if len(payload) < 8:
            return
        sid, count, _ = struct.unpack_from('<IHH', payload)
        a = self.by_sid.get(sid)
        if a and a.owner == me.id and len(payload) == 8 + 8 * count:
            a.equip = payload
            self.relay(a, ACT_EQUIP, payload, me.id)

    def proj(self, me, payload):
        if len(payload) != 84:
            return
        sid = SID_S.unpack_from(payload)[0]
        if sid & ACT_PLAYER:
            if sid & 0xFFFF == me.id:
                for p in list(self.srv.players.values()):
                    if p is not me:
                        self.send(p.id, ACT_PROJ, payload)
            return
        a = self.by_sid.get(sid)
        if a and a.owner == me.id:
            self.relay(a, ACT_PROJ, payload, me.id)

    def player_left(self, me):
        for a in list(self.by_sid.values()):
            a.interest.discard(me.id)
            if a.owner == me.id:
                self.hand_over(a)

    def handle(self, me, ptype, payload):
        """True if the packet was ours"""
        fn = {ACT_ASSIGN: self.assign, ACT_MOVES: self.moves, ACT_ACTION: self.action, ACT_RELEASE: self.release,
              ACT_HEALTH: self.health, ACT_DEATH: self.death, ACT_EQUIP: self.equip, ACT_PROJ: self.proj}.get(ptype)
        if fn is None:
            return False
        if ptype == ACT_ASSIGN and len(payload) != ASSIGN_S.size:
            return True
        fn(me, payload)
        return True

    def stats(self):
        return {'actors': len(self.by_sid), 'temp': sum(a.temp for a in self.by_sid.values()),
                'unowned': sum(a.owner is None for a in self.by_sid.values())}
