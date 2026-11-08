"""Fake second player for testing with one console.

Connects to the server and follows the first real player it sees:
  --mode circle  walks in a circle of --radius units around the player (default)
  --mode follow  walks after the player along the player's own track (real ground height),
                 --radius units behind, stops when you stop
  --mode front   stays --radius units in front of the player's camera and walks when the
                 player walks, so the puppet is always in view (videos, animation checks)
    python fake_client.py [--host 127.0.0.1] [--radius 200] [--mode circle|front]
"""
import argparse, asyncio, collections, math, struct, time
from protocol import *


async def main(host, port, radius, name, mode, equip, echo=False, face=False, hour=None, kill='', every='', die=0.0, look='', anchor='', npc='', npc_print=False, npc_replay=False):
    anchor_name = anchor
    reader, writer = await asyncio.open_connection(host, port)
    writer.write(pack(HELLO, HELLO_S.pack(VERSION, name.encode()[:32])))
    if hour is not None:                       # as the room host: this time of day for everybody
        async def clock():
            while True:
                writer.write(pack(TIME, TIME_S.pack(0, 0, hour)))
                await asyncio.sleep(5)
        asyncio.create_task(clock())
    if kill:                                   # "killed" these actors (hex FormIDs) a few seconds in
        async def killer():
            await asyncio.sleep(8)
            for fid in kill.split(','):
                writer.write(pack(KILL, KILL_S.pack(0, 0, int(fid, 16))))
                print('убил %s' % fid)
        asyncio.create_task(killer())
    if every:                                  # an animation event again and again ("coopcast 1 0001C789")
        async def repeat():
            await asyncio.sleep(10)
            while True:
                ev = every.encode()
                writer.write(pack(EVENT, EVENT_HDR.pack(0, len(ev)) + ev))
                print('событие: %s' % every)
                await asyncio.sleep(6)
        asyncio.create_task(repeat())
    if look:                                   # a real player's appearance from a recording (replay_client format)
        data, i = open(look, 'rb').read(), 0
        while i + REC_S.size <= len(data):
            t, pid, ptype, size = REC_S.unpack_from(data, i)
            i += REC_S.size
            if ptype == APPEARANCE:
                writer.write(pack(APPEARANCE, data[i:i + size]))
                print('внешность из записи: %d байт' % size)
                break
            i += size
    if equip:                                  # what this fake player wears (FormIDs, hex)
        # id[:kind] — kind 0 worn / right hand, 1 left hand, 2 spell left, 3 spell right, 4 voice
        items = [(int(x.split(':')[0], 16), int(x.split(':')[1]) if ':' in x else 0) for x in equip.split(',')]
        writer.write(pack(EQUIP, EQUIP_HDR.pack(0, len(items)) + b''.join(struct.pack('<IB3x', i, k) for i, k in items)))
    await writer.drain()
    ptype, payload = await read_packet(reader)
    my_id, _ = WELCOME_S.unpack(payload)
    print('подключён как id %d, режим %s' % (my_id, mode))

    anchor = None   # (world, cell, x, y, z, rotZ, flags, speed) of the first real player

    anchor_id = [None]
    names = {}                                  # id -> name (JOIN): with --anchor only that player is followed
    recorded = []                               # --npc-replay: the owner's NPC actions, sent back once we own the zone

    async def listen():
        nonlocal anchor
        while True:
            ptype, payload = await read_packet(reader)
            if ptype == ZONE:
                print('зона: хозяин id %d' % ZONE_S.unpack(payload)[0])
            if ptype == NPC_STATE and npc_print:
                n = NPC_HDR.unpack_from(payload)[0]
                ents = [struct.unpack_from('<I5fHH', payload, NPC_HDR.size + i * NPC_SIZE) for i in range(n)]
                print('NPC от хозяина: %d  %s' % (n, ' '.join('%08X@%.0f,%.0f%s' % (e[0], e[1], e[2], '*' if e[6] & 1 else '') for e in ents[:6])))
            if ptype == NPC_ACTION and npc_replay and len(recorded) < 40:
                recorded.append(payload)
            if ptype == ZONE and npc_replay and ZONE_S.unpack(payload)[0] == my_id and recorded:
                async def replay():
                    for _ in range(10):
                        for pl in recorded:
                            writer.write(pack(NPC_ACTION, pl))
                        print('повторил %d действий NPC как хозяин' % len(recorded))
                        await asyncio.sleep(3)
                asyncio.create_task(replay())
            if ptype == NPC_ACTION and npc_print:
                fid, _, typ, ln, act, tgt = struct.unpack_from('<IHBBII', payload)
                nm = payload[4 + 24:4 + 24 + ln].decode('latin-1', 'replace')
                print('NPC %08X действие %08X %s' % (fid, act, nm))
            if ptype == JOIN:
                jid, raw = JOIN_S.unpack(payload)
                names[jid] = raw.split(b'\0', 1)[0].decode('utf-8', 'replace')
            if ptype == STATE:
                f = STATE_S.unpack(payload)
                if anchor_name and names.get(f[0]) != anchor_name:
                    continue
                if anchor is None:
                    print('вижу игрока id %d: мир %08X ячейка %08X (%.0f, %.0f, %.0f)' % (f[0], f[2], f[3], f[4], f[5], f[6]))
                if anchor is None or f[0] == anchor_id[0]:      # follow one player only (the first seen)
                    anchor_id[0] = f[0]
                    anchor = (f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9])
            elif ptype in (EVENT, ACTION) and echo:       # repeat your attacks, blocks, jumps
                writer.write(pack(ptype, payload))
            elif ptype == HIT:                             # hit back, a bit weaker (tests both ways)
                attacker, _, amount = HIT_S.unpack(payload)
                print('получил удар %.0f от %d' % (amount, attacker))
                writer.write(pack(HIT, HIT_S.pack(attacker, 0, amount / 2)))
            elif ptype == GIVE:                            # thanks, and back it goes (tests both ways)
                giver, _, fid, cnt = GIVE_S.unpack(payload)
                print('получил %d x %08X от %d' % (cnt, fid, giver))
                await asyncio.sleep(3)
                writer.write(pack(GIVE, GIVE_S.pack(giver, 0, fid, cnt)))
            elif ptype == LEFT:
                gone = LEFT_S.unpack(payload)[0]
                print('игрок id %d ушёл' % gone)
                if gone == anchor_id[0]:                   # reconnects with a new id: follow whoever comes next
                    anchor_id[0], anchor = None, None

    asyncio.create_task(listen())
    t0, tick = time.time(), 0
    trail = collections.deque(maxlen=400)      # follow mode: the player's recent positions (20 s)
    pos = None
    while True:
        await asyncio.sleep(0.05)            # 20 Hz
        if anchor is None:
            continue
        world, cell, ax, ay, az, arot, aflags, aspeed = anchor
        if mode == 'follow':
            if trail and (trail[-1][3] != world or math.hypot(ax - trail[-1][0], ay - trail[-1][1]) > 3000):
                trail.clear()                            # the player teleported: start the track again
            if not trail or math.hypot(ax - trail[-1][0], ay - trail[-1][1]) > 5:
                trail.append((ax, ay, az, world))
            # the newest trail point at least `radius` behind the player
            target = None
            for p in reversed(trail):
                if math.hypot(p[0] - ax, p[1] - ay) >= radius:
                    target = p
                    break
            if pos is None or pos[3] != world or math.hypot(pos[0] - ax, pos[1] - ay) > radius * 6:
                # new place / too far: appear behind the player (the player's height: the ground there)
                pos = target or (ax - radius * math.sin(arot), ay - radius * math.cos(arot), az, world)
            if target is None:
                target = pos                             # nothing far enough yet: stay
            step = min(math.hypot(target[0] - pos[0], target[1] - pos[1]), max(aspeed, 120.0) * 0.05 * 1.2)
            d = math.hypot(target[0] - pos[0], target[1] - pos[1])
            moving = d > 3
            if moving:
                k = step / d
                heading = math.atan2(target[0] - pos[0], target[1] - pos[1])
                pos = (pos[0] + (target[0] - pos[0]) * k, pos[1] + (target[1] - pos[1]) * k,
                       pos[2] + (target[2] - pos[2]) * k, world)
            else:
                heading = math.atan2(ax - pos[0], ay - pos[1])    # stands looking at you
            x, y, az = pos[0], pos[1], pos[2]
            flags, speed = (FLAG_MOVING if moving else 0), (step / 0.05 if moving else 0.0)
        elif mode == 'front':
            # Skyrim heading: 0 = north (+Y), clockwise; forward = (sin, cos)
            x, y = ax + radius * math.sin(arot), ay + radius * math.cos(arot)
            heading, flags, speed = arot + (math.pi if face else 0), aflags & FLAG_MOVING, aspeed
        else:
            a = (time.time() - t0) * 0.5
            x, y = ax + radius * math.cos(a), ay + radius * math.sin(a)
            heading = math.pi / 2 - (a + math.pi / 2)   # tangent of the circle in Skyrim's heading convention
            flags, speed = FLAG_MOVING, radius * 0.5
        flags |= aflags & FLAG_WEAPON_DRAWN           # draws / sheathes together with you
        if die and die <= time.time() - t0 < die + 20:    # dead for 20 s, then "loaded a save"
            flags = FLAG_DEAD
        st = STATE_S.pack(my_id, 0, world, cell, x, y, az, heading, flags, speed, 0, 0, tick)
        if npc:                                   # as the zone owner: walk this NPC in a circle round us
            fid, _, rest = npc.partition(':')
            a = (time.time() - t0) * 0.4
            nx, ny = x + 300 * math.cos(a), y + 300 * math.sin(a)
            ent = struct.pack('<I5fHH', int(fid, 16), nx, ny, az, math.pi / 2 - (a + math.pi / 2), 120.0, 1, 0)
            if tick % 4 == 0:
                writer.write(pack(NPC_STATE, NPC_HDR.pack(1, 0) + ent))
        writer.write(pack(STATE, st))
        await writer.drain()
        tick += 1


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=PORT)
    ap.add_argument('--radius', type=float, default=200.0)
    ap.add_argument('--name', default='Fake')
    ap.add_argument('--mode', choices=['circle', 'front', 'follow'], default='circle')
    ap.add_argument('--kill', default='', help='report these actors (hex FormIDs) as killed')
    ap.add_argument('--hour', type=float, help='act as the room host with this game hour')
    ap.add_argument('--face', action='store_true', help='front mode: turned towards you')
    ap.add_argument('--echo', action='store_true', help='repeat the real player animation events (attacks...)')
    ap.add_argument('--equip', default='', help='worn items, hex FormIDs separated by commas (e.g. iron armour set)')
    ap.add_argument('--every', default='', help='send this animation event every 6 s (magic test: "coopcast 1 0001C789")')
    ap.add_argument('--die', type=float, default=0, help='be dead from this second for 20 s')
    ap.add_argument('--look', default='', help='send the appearance found in this recording (emu/rec_argonian.bin)')
    ap.add_argument('--anchor', default='', help='follow / face only the player with this name')
    ap.add_argument('--npc', default='', help='as the zone owner walk this NPC (hex FormID) in a circle round the bot')
    ap.add_argument('--npc-print', action='store_true', help='print the NPC states the zone owner sends')
    ap.add_argument('--npc-replay', action='store_true', help='record the owner NPC actions, replay them once we own the zone')
    a = ap.parse_args()
    asyncio.run(main(a.host, a.port, a.radius, a.name, a.mode, a.equip, a.echo, a.face, a.hour, a.kill, a.every, a.die, a.look, a.anchor, a.npc, a.npc_print, a.npc_replay))
