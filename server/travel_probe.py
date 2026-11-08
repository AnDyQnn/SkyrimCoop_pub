"""Test aid (03.10): a bot next to the first other player (its STATE + 100) that after <near> seconds 'travels'
20000 units away (as a carriage / fast travel would). python travel_probe.py <seconds> <near>"""
import asyncio, sys, time
from protocol import *

async def main():
    r, w = await asyncio.open_connection('127.0.0.1', PORT)
    w.write(pack(HELLO, HELLO_S.pack(VERSION, 'Бот-8'.encode()[:32])))
    await w.drain()
    t0 = time.time(); end = t0 + int(sys.argv[1]); near = int(sys.argv[2])
    last, sent, far = None, 0, None
    while time.time() < end:
        try:
            ptype, size = HEADER.unpack(await asyncio.wait_for(r.readexactly(HEADER.size), 0.5))
            body = await r.readexactly(size)
            if ptype == STATE and len(body) == STATE_S.size:
                last = list(STATE_S.unpack(body))
        except asyncio.TimeoutError:
            pass
        if last and time.time() - sent > 0.5:
            st = list(last)
            if time.time() - t0 < near:
                st[4] += 100; st[5] += 100
            else:
                if far is None:
                    far = list(st); far[4] += 20000; far[5] += 3000
                    print('уехал', flush=True)
                st = far
            w.write(pack(STATE, STATE_S.pack(*st)))
            await w.drain()
            sent = time.time()
    w.close()

asyncio.run(main())
