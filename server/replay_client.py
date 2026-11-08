"""Second player from a recording: plays back what a real console sent (coop_server.py --record),
with the original timing, looped. Test aid for one PC that cannot run two emulators.

    python replay_client.py rec.bin [--name Второй] [--player 1] [--speed 1.0]
"""
import argparse, asyncio, time
from protocol import *


def load(path, player):
    out = []
    with open(path, 'rb') as f:
        data = f.read()
    i = 0
    while i + REC_S.size <= len(data):
        t, pid, ptype, size = REC_S.unpack_from(data, i)
        i += REC_S.size
        payload = data[i:i + size]
        i += size
        if player is None or pid == player:
            out.append((t, pid, ptype, payload))
    return out


async def main(a):
    rec = load(a.file, a.player)
    if not rec:
        raise SystemExit('в записи нет пакетов')
    pids = sorted({r[1] for r in rec})
    rec = [r for r in rec if r[1] == pids[0]]
    looks = [r for r in rec if r[2] in (APPEARANCE, EQUIP)]
    first = next((k for k, r in enumerate(rec) if r[2] == STATE and STATE_S.unpack(r[3])[3]), 0)
    rec = rec[first:]                            # from the moment the recorded player is in the world
    print('запись: %d пакетов, %.0f с' % (len(rec), rec[-1][0] - rec[0][0]))
    reader, writer = await asyncio.open_connection(a.host, a.port)
    writer.write(pack(HELLO, HELLO_S.pack(VERSION, a.name.encode('utf-8')[:32])))
    await writer.drain()

    async def drain_input():
        while True:
            await read_packet(reader)
    asyncio.create_task(drain_input())
    for ptype in (APPEARANCE, EQUIP):           # the last look first, so the puppet spawns right away
        last = [r[3] for r in looks if r[2] == ptype]
        if last:
            writer.write(pack(ptype, last[-1]))
    loop = 0
    while True:
        t0, start = rec[0][0], time.time()
        for t, pid, ptype, payload in rec:
            delay = (t - t0) / a.speed - (time.time() - start)
            if delay > 0:
                await asyncio.sleep(delay)
            if ptype == STATE:
                f = list(STATE_S.unpack(payload))
                f[12] = (f[12] + loop * 100000) & 0xFFFFFFFF    # tick keeps growing across loops
                payload = STATE_S.pack(*f)
            writer.write(pack(ptype, payload))
            await writer.drain()
        loop += 1
        print('круг %d' % loop)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=PORT)
    ap.add_argument('--name', default='Второй')
    ap.add_argument('--player', type=int, help='player id in the recording (default: the first one)')
    ap.add_argument('--speed', type=float, default=1.0)
    asyncio.run(main(ap.parse_args()))
