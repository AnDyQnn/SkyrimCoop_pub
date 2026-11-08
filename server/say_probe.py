"""Test aid (03.10): a bot that stands where the first other player stands (mirrors its STATE), prints the NPC lines
it hears (WORLD / WK_SAY) and says one line itself. python say_probe.py <seconds> [line]"""
import asyncio, sys, time
from protocol import *

async def main():
    r, w = await asyncio.open_connection('127.0.0.1', PORT)
    w.write(pack(HELLO, HELLO_S.pack(VERSION, 'Бот-3'.encode()[:32])))
    await w.drain()
    end, said, last = time.time() + int(sys.argv[1]), False, None
    while time.time() < end:
        try:
            hdr = await asyncio.wait_for(r.readexactly(HEADER.size), 1.0)
        except asyncio.TimeoutError:
            hdr = None
        if hdr:
            ptype, size = HEADER.unpack(hdr)
            body = await r.readexactly(size)
            if ptype == STATE and len(body) == STATE_S.size:
                last = body
            elif ptype == WORLD and len(body) >= WORLD_HDR.size:
                kind, count, ref = WORLD_HDR.unpack_from(body)
                if kind == WK_SAY:
                    print('слышу от игрока %d: %s' % (ref, body[WORLD_HDR.size:WORLD_HDR.size + count].decode('utf-8', 'replace')), flush=True)
        if last:
            w.write(pack(STATE, last))
            if not said and len(sys.argv) > 2:
                await asyncio.sleep(3)
                t = sys.argv[2].encode('utf-8')
                w.write(pack(WORLD, WORLD_HDR.pack(WK_SAY, len(t), 0) + t))
                said = True
            await w.drain()
    w.close()

asyncio.run(main())
