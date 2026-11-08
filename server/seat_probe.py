"""Test aid (03.10): a bot sitting next to the first other player: its STATE shifted <side> units to the left
(and <fwd> forward) in that player's own frame. python seat_probe.py <seconds> <side> <fwd>"""
import asyncio, sys, time, math
from protocol import *

async def main():
    r, w = await asyncio.open_connection('127.0.0.1', PORT)
    w.write(pack(HELLO, HELLO_S.pack(VERSION, 'Бот-10'.encode()[:32])))
    await w.drain()
    end = time.time() + int(sys.argv[1]); side = float(sys.argv[2]); fwd = float(sys.argv[3])
    while time.time() < end:
        try:
            ptype, size = HEADER.unpack(await asyncio.wait_for(r.readexactly(HEADER.size), 0.3))
            body = await r.readexactly(size)
            if ptype == STATE and len(body) == STATE_S.size:
                st = list(STATE_S.unpack(body)); rot = st[7]
                st[4] += fwd * math.sin(rot) - side * math.cos(rot)
                st[5] += fwd * math.cos(rot) + side * math.sin(rot)
                w.write(pack(STATE, STATE_S.pack(*st))); await w.drain()
        except asyncio.TimeoutError:
            pass
    w.close()

asyncio.run(main())
