"""Test aid (03.10): a bot that drops an iron sword <dx> <dy> from the first other player and prints WK_ITEM it hears.
python item_probe.py <seconds> <dx> <dy>"""
import asyncio, sys, time, struct
from protocol import *

async def main():
    r, w = await asyncio.open_connection('127.0.0.1', PORT)
    w.write(pack(HELLO, HELLO_S.pack(VERSION, 'Бот-11'.encode()[:32])))
    await w.drain()
    end = time.time() + int(sys.argv[1]); done = False; me = 0
    while time.time() < end:
        try:
            ptype, size = HEADER.unpack(await asyncio.wait_for(r.readexactly(HEADER.size), 0.5))
            body = await r.readexactly(size)
            if ptype == WELCOME:
                me = struct.unpack_from('<H', body)[0]
            if ptype == STATE and len(body) == STATE_S.size and not done:
                st = STATE_S.unpack(body)
                if st[3]:
                    b = struct.pack('<IIII3f', 0x00012EB7, 1, st[2], 0 if st[2] else st[3], st[4] + float(sys.argv[2]), st[5] + float(sys.argv[3]), st[6] + 20)
                    w.write(pack(WORLD, WORLD_HDR.pack(WK_ITEM, 1, (me << 16) | 1) + b)); await w.drain(); done = True
                    print('бот бросил меч', flush=True)
            if ptype == WORLD and len(body) >= WORLD_HDR.size and WORLD_HDR.unpack_from(body)[0] == WK_ITEM:
                k, c, ref = WORLD_HDR.unpack_from(body)
                print('предмет %08X %s' % (ref, 'брошен' if c else 'поднят'), flush=True)
        except asyncio.TimeoutError:
            pass
    w.close()

asyncio.run(main())
