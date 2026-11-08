"""Test aid (03.10): a bot with a bow <dx> <dy> units away from the first other player, facing them, shooting
every 3 s ("cooparrow" event: the puppet fires a harmless copy). python arrow_probe.py <seconds> <dx> <dy>"""
import asyncio, math, struct, sys, time
from protocol import *

LONG_BOW, IRON_ARROW = 0x0003B562, 0x0001397D

async def main():
    r, w = await asyncio.open_connection('127.0.0.1', PORT)
    w.write(pack(HELLO, HELLO_S.pack(VERSION, 'Бот-6'.encode()[:32])))
    w.write(pack(EQUIP, EQUIP_HDR.pack(0, 2) + struct.pack('<IB3x', LONG_BOW, 0) + struct.pack('<IB3x', IRON_ARROW, 0)))
    await w.drain()
    end, dx, dy = time.time() + int(sys.argv[1]), float(sys.argv[2]), float(sys.argv[3])
    last, sent, shot = None, 0, time.time() + 10
    while time.time() < end:
        try:
            ptype, size = HEADER.unpack(await asyncio.wait_for(r.readexactly(HEADER.size), 0.3))
            body = await r.readexactly(size)
            if ptype == STATE and len(body) == STATE_S.size and last is None:
                last = list(STATE_S.unpack(body))          # where the player stood first: the bot stays there
        except asyncio.TimeoutError:
            pass
        if last and time.time() - sent > 0.5:
            st = list(last); st[4] += dx; st[5] += dy
            st[7] = math.atan2(-dx, -dy)                   # facing the player (Skyrim: 0 = north, clockwise)
            st[8] = 0; st[9] = 0.0; st[10] = LONG_BOW
            w.write(pack(STATE, STATE_S.pack(*st)))
            sent = time.time()
        if last and time.time() > shot:
            ev = ('cooparrow %08x %08x' % (LONG_BOW, IRON_ARROW)).encode()
            w.write(pack(EVENT, EVENT_HDR.pack(0, len(ev)) + ev))
            shot = time.time() + 3
            print('выстрел', flush=True)
        await w.drain()
    w.close()

asyncio.run(main())
