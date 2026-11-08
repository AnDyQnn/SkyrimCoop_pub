"""Test aid (03.10): a second 'player' that sets a container's state on the server, like another console would.
    python world_probe.py <container hex> <form hex>:<delta> [...]"""
import asyncio, struct, sys
from protocol import *

async def main():
    r, w = await asyncio.open_connection('127.0.0.1', PORT)
    w.write(pack(HELLO, HELLO_S.pack(VERSION, 'Бот-1'.encode()[:32])))
    await asyncio.sleep(2)
    ref = int(sys.argv[1], 16)
    items = [(int(a, 16), int(b)) for a, b in (x.split(':') for x in sys.argv[2:])]
    w.write(pack(WORLD, WORLD_HDR.pack(WK_CONTAINER, len(items), ref) + b''.join(struct.pack('<Ii', f, n) for f, n in items)))
    await w.drain()
    await asyncio.sleep(3)
    w.close()

asyncio.run(main())
