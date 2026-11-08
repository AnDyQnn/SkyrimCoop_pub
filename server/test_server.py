"""Automated checks of the server with several simulated consoles (run: python test_server.py).

Covers what a session of 2+ Switches relies on: join/leave, names, states relayed, looks and gear kept
for newcomers, shared kills, follower orders, host clock, chat, gifts, hits (PvE / PvP), clans, saves,
kick/refusal, reconnects.
"""
import asyncio, os, shutil, struct, sys, tempfile, time, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ['SKYCOOP_DATA'] = tempfile.mkdtemp(prefix='skycoop_t_')
from protocol import *
from coop_server import Server
from storage import Store

PORT_T = 27400
fails = []


def check(cond, what):
    print(('  ok   ' if cond else '  FAIL ') + what)
    if not cond:
        fails.append(what)


class Console:
    def __init__(self, name):
        self.name, self.got, self.id = name, [], None

    async def connect(self):
        self.r, self.w = await asyncio.open_connection('127.0.0.1', PORT_T)
        self.w.write(pack(HELLO, HELLO_S.pack(VERSION, self.name.encode('utf-8'))))
        await self.w.drain()
        self.task = asyncio.create_task(self.listen())
        for _ in range(50):
            if self.id is not None:
                return self
            await asyncio.sleep(0.02)
        return self

    async def listen(self):
        try:
            while True:
                t, p = await read_packet(self.r)
                self.got.append((t, p))
                if t == WELCOME:
                    self.id = WELCOME_S.unpack(p)[0]
        except Exception:  # noqa: BLE001
            self.got.append(('closed', b''))

    def send(self, t, p=b''):
        self.w.write(pack(t, p))

    def of(self, t):
        return [p for tt, p in self.got if tt == t]

    def close(self):
        self.w.close()


def b4_quest(c):
    return c.of(QUEST)


def state(x, cell=0x1A27A, world=0x1A26F, tick=1):
    return STATE_S.pack(0, 0, world, cell, x, 0.0, 0.0, 0.0, 0, 0.0, 0, 0, tick)


async def main():
    srv = Server('', PORT_T, log=lambda s: None, store=Store())
    await srv.start(control=False)
    pause = lambda: asyncio.sleep(0.3)

    print('join / names / states')
    a = await Console('Аня').connect()
    b = await Console('Боря').connect()
    check(a.id and b.id and a.id != b.id, 'разные id у двух консолей')
    await pause()
    names_a = {JOIN_S.unpack(p)[0]: JOIN_S.unpack(p)[1].split(b'\0')[0].decode() for p in a.of(JOIN)}
    check(names_a.get(b.id) == 'Боря', 'первая консоль знает имя второй')
    a.send(STATE, state(100.0)); await pause()
    st = [STATE_S.unpack(p) for p in b.of(STATE)]
    check(any(s[0] == a.id and s[4] == 100.0 for s in st), 'положение первой дошло до второй с её id')
    check(not any(STATE_S.unpack(p)[0] == a.id for p in a.of(STATE)), 'своё положение не возвращается себе')

    print('look / gear / kills / followers kept for newcomers')
    a.send(APPEARANCE, APPEAR_S.pack(0, 0, 0x22) + b'X' * 100)
    a.send(EQUIP, EQUIP_HDR.pack(0, 1) + struct.pack('<IB3x', 0x12EB7, 0))
    a.send(KILL, KILL_S.pack(0, 0, 0x3707B))
    a.send(FOLLOW, FOLLOW_S.pack(0xA2C94, a.id, 0))
    await pause()
    check(b.of(APPEARANCE) and APPEAR_S.unpack_from(b.of(APPEARANCE)[-1])[0] == a.id, 'внешность дошла до второй')
    check(b.of(KILL) and KILL_S.unpack(b.of(KILL)[-1])[2] == 0x3707B, 'смерть в общем мире дошла')
    check(b.of(FOLLOW) and FOLLOW_S.unpack(b.of(FOLLOW)[-1])[:2] == (0xA2C94, a.id), 'приказ спутнику дошёл')
    c = await Console('Вика').connect(); await pause()
    check(any(APPEAR_S.unpack_from(p)[0] == a.id for p in c.of(APPEARANCE)), 'новичок получил внешность')
    check(any(EQUIP_HDR.unpack_from(p)[0] == a.id for p in c.of(EQUIP)), 'новичок получил снаряжение')
    check(any(KILL_S.unpack(p)[2] == 0x3707B for p in c.of(KILL)), 'новичок получил список убитых')
    check(any(FOLLOW_S.unpack(p)[0] == 0xA2C94 for p in c.of(FOLLOW)), 'новичок получил приказы спутникам')
    check(any(STATE_S.unpack(p)[0] == a.id for p in c.of(STATE)), 'новичок сразу знает, где остальные')
    c.send(FOLLOW, FOLLOW_S.pack(0xA2C94, c.id, 0)); await pause()
    check(any(FOLLOW_S.unpack(p)[:2] == (0xA2C94, a.id) for p in c.of(FOLLOW)), 'чужой спутник не идёт за другим')
    check(any('уже в отряде' in p[2:].decode('utf-8', 'replace') for p in c.of(CHAT)), 'другому сказали, что спутник занят')

    print('clock / chat / gifts / hits')
    b.send(TIME, TIME_S.pack(0, 0, 3.0)); a.send(TIME, TIME_S.pack(0, 0, 21.0)); await pause()
    hours = [TIME_S.unpack(p)[2] for p in c.of(TIME)]
    check(21.0 in hours and 3.0 not in hours, 'время суток берётся только у первого (хозяина)')
    b.send(CHAT, CHAT_HDR.pack(0) + 'привет'.encode()); await pause()
    check(any(p[2:].decode() == 'привет' and CHAT_HDR.unpack_from(p)[0] == b.id for p in a.of(CHAT)), 'чат дошёл с именем')
    a.send(GIVE, GIVE_S.pack(b.id, 0, 0xF, 25)); await pause()
    check(any(GIVE_S.unpack(p) == (a.id, 0, 0xF, 25) for p in b.of(GIVE)), 'подарок дошёл получателю')
    check(not c.of(GIVE), 'подарок не ушёл посторонним')
    a.send(HIT, HIT_S.pack(b.id, 0, 30.0)); await pause()
    check(not b.of(HIT), 'в PvE удар не проходит')
    srv.store.settings['pvp'] = True
    a.send(HIT, HIT_S.pack(b.id, 0, 30.0)); await pause()
    check(any(HIT_S.unpack(p)[0] == a.id for p in b.of(HIT)), 'в PvP удар проходит')

    print('quests (the campaign: always shared)')
    a.send(QUEST, QUEST_S.pack(0x3372B, 20, 0)); a.send(QUEST, QUEST_S.pack(0x3372B, 15, 0)); await pause()
    got = [QUEST_S.unpack(p)[:2] for p in b.of(QUEST)]
    check(got == [(0x3372B, 20)], 'квесты: общий прогресс кампании, более ранняя стадия не шлётся')

    print('npc damage')
    a.send(NPC_DAMAGE, DAMAGE_S.pack(0x1A67B, 12.5)); await pause()
    check(any(DAMAGE_S.unpack(p) == (0x1A67B, 12.5) for p in b.of(NPC_DAMAGE)), 'урон врагу дошёл до остальных')
    check(not a.of(NPC_DAMAGE), 'урон не возвращается нанёсшему')
    a.send(NPC_DAMAGE, DAMAGE_S.pack(0x3707B, 5.0)); await pause()
    check(not any(DAMAGE_S.unpack(p)[0] == 0x3707B for p in b.of(NPC_DAMAGE)), 'по убитому урон не шлётся')

    print('souls / words for the zone')
    a.send(WORLD, WORLD_HDR.pack(WK_SOUL, 0, 0x1CA03)); await pause()
    print('   (без зон — не шлётся)')
    print('things on the ground')
    a.send(WORLD, WORLD_HDR.pack(WK_ITEM, 1, (a.id << 16) | 1) + struct.pack('<IIII3f', 0x12E49, 1, 0x3C, 0, 1.0, 2.0, 3.0)); await pause()
    check(any(WORLD_HDR.unpack_from(p)[0] == WK_ITEM and WORLD_HDR.unpack_from(p)[1] == 1 for p in b.of(WORLD)), 'брошенный предмет дошёл до второго')
    b.send(WORLD, WORLD_HDR.pack(WK_ITEM, 0, (a.id << 16) | 1)); await pause()
    check(any(WORLD_HDR.unpack_from(p)[0] == WK_ITEM and WORLD_HDR.unpack_from(p)[1] == 0 for p in a.of(WORLD)), 'поднятый предмет исчез у первого')
    check(not srv.store.ground, 'сервер забыл поднятый предмет')
    print('mods')
    NL = chr(10)
    a.send(PLUGINS, NL.join(['Skyrim.esm', 'Update.esm', 'SDA.esp']).encode()); await pause()
    b.send(PLUGINS, NL.join(['Skyrim.esm', 'Update.esm']).encode()); await pause()
    check(any('Моды не совпадают' in p[2:].decode() for p in b.of(CHAT)), 'разные моды: игрок получает предупреждение')
    c.send(PLUGINS, NL.join(['Skyrim.esm', 'Update.esm', 'SDA.esp']).encode()); await pause()
    check(not any('Моды не совпадают' in p[2:].decode() for p in c.of(CHAT)), 'одинаковые моды: без предупреждения')

    print('campaign (no guilds)')
    a.send(SOCIAL, 'clan create Волки'.encode()); await pause()
    check('Гильдий нет' in a.of(SOCIAL_INFO)[-1].decode(), 'гильдии не создаются: сервер — одна кампания')
    a.send(QUEST, QUEST_S.pack(0x1234, 50, 0)); await pause()
    check(any(QUEST_S.unpack(p)[:2] == (0x1234, 50) for p in b.of(QUEST)), 'кампания: квест одного дошёл до второго')
    check(any(QUEST_S.unpack(p)[:2] == (0x1234, 50) for p in c.of(QUEST)), 'кампания: и до третьего')

    print('saves')
    data = os.urandom(30000)
    for off in range(0, len(data), SAVE_CHUNK):
        a.send(SAVE_PUT, SAVE_HDR.pack(len(data), off, b'20260101000000_Save1_T.sav0') + data[off:off + SAVE_CHUNK])
    await pause()
    a.send(SAVE_LIST_REQ); await pause()
    check(b'20260101000000_Save1_T.sav0' in a.of(SAVE_LIST)[-1], 'сохранение появилось на сервере')
    a.send(SAVE_GET, b'20260101000000_Save1_T.sav0'); await asyncio.sleep(0.6)
    got = bytearray(len(data))
    for p in a.of(SAVE_DATA):
        total, off, _ = SAVE_HDR.unpack_from(p)
        chunk = p[SAVE_HDR.size:]
        got[off:off + len(chunk)] = chunk
    check(bytes(got) == data, 'скачанное сохранение совпадает побайтно')

    print('zones')
    def st(x, y, cell=0x1a27a, world=0x1a26f):
        return STATE_S.pack(0, 0, world, cell, x, y, 0.0, 0.0, 0, 0.0, 0, 0, 1)
    a.send(STATE, st(100.0, 100.0)); await pause()
    b.send(STATE, st(300.0, 200.0)); await pause()
    zb = [ZONE_S.unpack(p)[0] for p in b.of(ZONE)]
    check(zb and zb[-1] == a.id, 'второй в зоне получает хозяина — первого')
    npc = NPC_HDR.pack(1, 0) + struct.pack('<I5fHH', 0x1a67c, 1.0, 2.0, 3.0, 0.0, 0.0, 1, 0)
    b.send(NPC_STATE, npc); await pause()
    check(not a.of(NPC_STATE), 'NPC от не-хозяина не пересылаются')
    a.send(NPC_STATE, npc); await pause()
    check(b.of(NPC_STATE) and b.of(NPC_STATE)[-1] == npc, 'NPC хозяина зоны доходят до остальных в зоне')
    c.send(STATE, st(100000.0, 100000.0)); await pause()
    check(not c.of(NPC_STATE), 'в другую зону NPC не идут')

    print('weather')
    host = min(srv.players)
    hostc = next(x for x in (a, b, c) if x.id == host)
    other = next(x for x in (a, b, c) if x.id != host)
    for sender, fid in ((other, b'coopweather 0000012e'), (hostc, b'coopweather 0010a241')):
        sender.send(EVENT, EVENT_HDR.pack(0, len(fid)) + fid)
    await pause()
    seen = [p[EVENT_HDR.size:] for x in (a, b, c) for p in x.of(EVENT) if p[EVENT_HDR.size:].startswith(b'coopweather')]
    check(b'coopweather 0010a241' in seen and b'coopweather 0000012e' not in seen, 'погода только от хозяина комнаты')

    print('campaign chest')
    a.send(SOCIAL, b'chest put 12e49 2'); await pause()
    a.send(SOCIAL, b'chest get'); await pause()
    check(a.of(SOCIAL_INFO)[-1].decode() == '#chest 00012E49:2', 'в сундук кампании кладётся')
    a.send(SOCIAL, b'chest take 12e49 3'); await pause()
    check(a.of(SOCIAL_INFO)[-1].decode() == '#chestdeny 00012E49 1', 'лишнее из сундука не отдаётся')
    a.send(SOCIAL, b'chest get'); await pause()
    check(a.of(SOCIAL_INFO)[-1].decode() == '#chest ', 'сундук пуст после выемки')
    a.send(SOCIAL, b'chest delta 12e49:+3 12e4d:-1'); await pause()
    check(a.of(SOCIAL_INFO)[-1].decode() == '#chestdeny 00012E4D:1', 'пакетные изменения сундука')
    a.send(SOCIAL, b'chest get'); await pause()
    check(a.of(SOCIAL_INFO)[-1].decode() == '#chest 00012E49:3', 'сундук после пакетных изменений')
    b.send(SOCIAL, b'chest get'); await pause()
    check(b.of(SOCIAL_INFO)[-1].decode() == '#chest 00012E49:3', 'сундук кампании у второго тот же')

    print('updates')
    upd = os.path.join(srv.store.dir, 'updates', 'zz_test', 'mod.bin')
    os.makedirs(os.path.dirname(upd), exist_ok=True)
    blob = os.urandom(40000)
    with open(upd, 'wb') as f:
        f.write(blob)
    a.send(UPD_LIST_REQ); await pause()
    line = '%s\t%d\t%08x' % ('zz_test/mod.bin', len(blob), zlib.crc32(blob) & 0xFFFFFFFF)
    check(line in a.of(UPD_LIST)[-1].decode('utf-8'), 'список обновлений с размером и CRC')
    a.send(UPD_GET, b'zz_test/mod.bin'); await asyncio.sleep(0.6)
    got = bytearray(len(blob))
    for p in a.of(UPD_DATA):
        total, off, _ = UPD_HDR.unpack_from(p)
        chunk = p[UPD_HDR.size:]
        got[off:off + len(chunk)] = chunk
    check(bytes(got) == blob, 'файл обновления скачивается побайтно')
    n = len(a.of(UPD_DATA))
    a.send(UPD_GET, b'../storage.py'); await pause()
    check(len(a.of(UPD_DATA)) == n, 'за пределы папки updates не выпускает')
    shutil.rmtree(os.path.join(srv.store.dir, 'updates', 'zz_test'))

    print('avatar')
    a.send(AVATAR_GET); await pause()
    check(UPD_HDR.unpack_from(a.of(AVATAR_DATA)[-1])[0] == 0, 'персонажа ещё нет — пустой ответ')
    av = os.urandom(30000)
    for off in range(0, len(av), SAVE_CHUNK):
        a.send(AVATAR_PUT, UPD_HDR.pack(len(av), off, b'avatar') + av[off:off + SAVE_CHUNK])
    await pause()
    n = len(a.of(AVATAR_DATA))
    a.send(AVATAR_GET); await asyncio.sleep(0.6)
    got = bytearray(len(av))
    for p in a.of(AVATAR_DATA)[n:]:
        total, off, _ = UPD_HDR.unpack_from(p)
        chunk = p[UPD_HDR.size:]
        got[off:off + len(chunk)] = chunk
    check(bytes(got) == av, 'персонаж сохраняется и скачивается побайтно')

    print('leave / reconnect / kick')
    b.close(); await asyncio.sleep(0.5)
    check(any(LEFT_S.unpack(p)[0] == b.id for p in a.of(LEFT)), 'уход игрока виден остальным')
    b2 = await Console('Боря').connect(); await pause()
    check(b2.id and b2.id != b.id, 'переподключение с тем же именем пускает')
    names = [p.name for p in srv.players.values()]
    check(names.count('Боря') == 1, 'после переподключения нет двойника')
    srv.kick('Боря'); await asyncio.sleep(0.5)
    b3 = await Console('Боря').connect(); await asyncio.sleep(0.5)
    check(b3.id == 0, 'выгнанный получает отказ')
    srv.unban('Боря')
    b4 = await Console('Боря').connect(); await pause()
    check(b4.id and b4.id > 0, 'после «вернуть» снова пускает')

    for x in (a, c, b4):
        x.close()
    await srv.stop()
    print('\n%s' % ('ВСЁ ХОРОШО' if not fails else 'ОШИБКИ: %d' % len(fails)))
    return not fails


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except AttributeError:
        pass
    ok = asyncio.run(main())
    sys.exit(0 if ok else 1)
