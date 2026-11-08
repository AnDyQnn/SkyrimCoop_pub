"""Test aid: a bot player of a given level stays in the room for N seconds. python level_probe.py <level> <seconds>"""
import asyncio, sys
from protocol import *
async def main():
    r, w = await asyncio.open_connection('127.0.0.1', PORT)
    w.write(pack(HELLO, HELLO_S.pack(VERSION, 'Бот-2'.encode()[:32])))
    for _ in range(int(sys.argv[2]) // 2):
        w.write(pack(STATS, STATS_S.pack(0, int(sys.argv[1]), 100, 100, 100, 0)))
        await w.drain()
        await asyncio.sleep(2)
    w.close()
asyncio.run(main())
