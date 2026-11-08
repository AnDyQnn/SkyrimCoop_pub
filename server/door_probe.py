"""Test aid: a bot sets a door's state on the server. python door_probe.py <door hex> <bits: 1 open, 2 locked>"""
import asyncio, sys
from protocol import *
async def main():
    r, w = await asyncio.open_connection('127.0.0.1', PORT)
    w.write(pack(HELLO, HELLO_S.pack(VERSION, 'Бот-3'.encode()[:32])))
    await asyncio.sleep(2)
    w.write(pack(WORLD, WORLD_HDR.pack(WK_DOOR, int(sys.argv[2]), int(sys.argv[1], 16))))
    await w.drain(); await asyncio.sleep(3); w.close()
asyncio.run(main())
