"""Test aid (03.10): a bot that hires a companion first (FOLLOW to itself) and stays N seconds.
python companion_probe.py <companion hex FormID> <seconds>"""
import asyncio, sys
from protocol import *

async def main():
    r, w = await asyncio.open_connection('127.0.0.1', PORT)
    w.write(pack(HELLO, HELLO_S.pack(VERSION, 'Бот-4'.encode()[:32])))
    await w.drain()
    my = None
    while my is None:
        ptype, size = HEADER.unpack(await r.readexactly(HEADER.size))
        body = await r.readexactly(size)
        if ptype == WELCOME:
            my = struct.unpack_from('<H', body)[0]
    w.write(pack(FOLLOW, FOLLOW_S.pack(int(sys.argv[1], 16), my, 0)))
    await w.drain()
    print('бот', my, 'нанял', sys.argv[1], flush=True)
    end = asyncio.get_event_loop().time() + int(sys.argv[2])
    while asyncio.get_event_loop().time() < end:
        try:
            ptype, size = HEADER.unpack(await asyncio.wait_for(r.readexactly(HEADER.size), 1.0))
            await r.readexactly(size)
        except asyncio.TimeoutError:
            pass
    w.close()

asyncio.run(main())
