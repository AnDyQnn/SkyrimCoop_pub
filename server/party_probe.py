"""Test aid (03.10): a bot standing <dx> <dy> units away from the first other player (its STATE shifted).
python party_probe.py <seconds> <dx> <dy>"""
import asyncio, sys, time
from protocol import *

async def main():
    r, w = await asyncio.open_connection('127.0.0.1', PORT)
    w.write(pack(HELLO, HELLO_S.pack(VERSION, (sys.argv[4] if len(sys.argv) > 4 else 'Бот-5').encode()[:32])))
    await w.drain()
    end, dx, dy = time.time() + int(sys.argv[1]), float(sys.argv[2]), float(sys.argv[3])
    last, sent = None, 0
    while time.time() < end:
        try:
            ptype, size = HEADER.unpack(await asyncio.wait_for(r.readexactly(HEADER.size), 0.5))
            body = await r.readexactly(size)
            if ptype == STATE and len(body) == STATE_S.size:
                last = list(STATE_S.unpack(body))
        except asyncio.TimeoutError:
            pass
        if last and time.time() - sent > 0.5:
            st = list(last); st[4] += dx; st[5] += dy
            w.write(pack(STATE, STATE_S.pack(*st)))
            await w.drain()
            sent = time.time()
            hp = 100 - int((time.time() * 10) % 60)       # the nameplate test: level 25, health sliding 100 -> 41
            w.write(pack(STATS, STATS_S.pack(0, 25, hp, 80, 60, 0)))
            if len(sys.argv) > 5 and not getattr(main, 'gift', False) and time.time() > end - int(sys.argv[1]) + 15:
                main.gift = True                          # test: a dragon soul and a word from this bot
                w.write(pack(WORLD, WORLD_HDR.pack(WK_SOUL, 0, 0xFF000001)))
                w.write(pack(WORLD, WORLD_HDR.pack(WK_WORD, 0, int(sys.argv[5], 16))))
    w.close()

asyncio.run(main())
