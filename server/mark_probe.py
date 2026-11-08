"""Test aid (03.10): a bot that puts a map marker <dx> <dy> from the first other player and stays.
python mark_probe.py <seconds> <dx> <dy>"""
import asyncio, sys, time, struct
from protocol import *

async def main():
    r, w = await asyncio.open_connection('127.0.0.1', PORT)
    w.write(pack(HELLO, HELLO_S.pack(VERSION, 'Бот-9'.encode()[:32])))
    await w.drain()
    end = time.time() + int(sys.argv[1]); done = False
    while time.time() < end:
        try:
            ptype, size = HEADER.unpack(await asyncio.wait_for(r.readexactly(HEADER.size), 0.5))
            body = await r.readexactly(size)
            if ptype == STATE and len(body) == STATE_S.size and not done:
                st = STATE_S.unpack(body)
                if st[2] and st[3]:
                    w.write(pack(WORLD, WORLD_HDR.pack(WK_MARK, 1, st[2]) + struct.pack('<3f', st[4] + float(sys.argv[2]), st[5] + float(sys.argv[3]), st[6])))
                    await w.drain(); done = True
                    print('метка поставлена', flush=True)
        except asyncio.TimeoutError:
            pass
    w.close()

asyncio.run(main())
