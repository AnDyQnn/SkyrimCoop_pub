"""SkyrimCoop wire protocol (v1). Mirrored in src/skyrimcoop/source/program/coop/protocol.hpp.

Every packet: header <HH> = (type, payload size), little-endian, then the payload.
TCP, TCP_NODELAY. The server assigns player ids and relays states.
"""
import struct

VERSION = 2
PORT = 27015

HELLO, WELCOME, STATE, LEFT, EVENT, CHAT, INPUT, LOG, JOIN, CMD, APPEARANCE, EQUIP, TIME, KILL, ACTION = 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
SAVE_PUT, SAVE_LIST_REQ, SAVE_LIST, SAVE_GET, SAVE_DATA = 16, 17, 18, 19, 20
GIVE, HIT, FOLLOW, SOCIAL, SOCIAL_INFO, QUEST, PLUGINS, NPC_DAMAGE = 21, 22, 23, 24, 25, 26, 27, 28
UPD_LIST_REQ, UPD_LIST, UPD_GET, UPD_DATA = 29, 30, 31, 32
AVATAR_PUT, AVATAR_GET, AVATAR_DATA = 33, 34, 35
ZONE, NPC_STATE, NPC_ACTION = 36, 37, 38
STATS = 39
TELE = 41            # client -> server: the module's text picture of the game (playtest/play.py)

HEADER = struct.Struct('<HH')
# HELLO  (client -> server): version u16, name 32 bytes utf-8
HELLO_S = struct.Struct('<H32s')
# WELCOME (server -> client): your id u16, server version u16
WELCOME_S = struct.Struct('<HH')
# STATE (both ways; server adds the sender's id when relaying):
#   id u16, pad u16, worldspace u32, cell u32, x y z f32, rotZ f32,
#   flags u32 (see FLAG_*), speed f32, rightHand u32, leftHand u32, tick u32
STATE_S = struct.Struct('<HHIIffffIfIII')
# INPUT (server -> clients, testing): buttons u64, lx ly rx ry s32, ms u32 — OR'ed into the pad
INPUT_S = struct.Struct('<QiiiiI4x')   # 32 bytes, as the C++ struct (u64 alignment)
BUTTONS = dict(a=0, b=1, x=2, y=3, l3=4, r3=5, l=6, r=7, zl=8, zr=9, plus=10, minus=11,
               left=12, up=13, right=14, down=15)
# JOIN (server -> clients): id u16, name 32 bytes utf-8 — sent for every player, old and new
JOIN_S = struct.Struct('<H32s')
# LEFT (server -> clients): id u16
LEFT_S = struct.Struct('<H')
# APPEARANCE (both ways): id u16, pad u16, change flags u32, then the player's NPC base as
#   TESForm::SaveGame writes it (FormIDs raw). Server stamps the sender id and keeps the last one.
APPEAR_S = struct.Struct('<HHI')
# EQUIP (both ways): id u16, count u16, then count x (formId u32, left u8, pad 3) — worn armour and
#   held items. Server stamps the sender id and keeps the last one.
EQUIP_HDR = struct.Struct('<HH')
# TIME (client -> server -> others): id u16, pad u16, game hour f32; only the host's (lowest id) is relayed
TIME_S = struct.Struct('<HHf')
# CLOCK (08.10, the Skyrim Together way: the server keeps the game clock): hour, time scale, day, month, year (f32).
#   server -> clients: the clock now (on join and every 10 s); client -> server: its own clock — taken only while the
#   campaign has none yet (the first player's)
CLOCK = 42
CLOCK_S = struct.Struct('<fffff')
# KILL (both ways): id u16, pad u16, FormID u32 of a plugin actor that died; the server keeps the list
KILL_S = struct.Struct('<HHI')
KILL_RESPAWNS, KILL_REVIVE = 1, 2         # KillMsg.flags (03.10)
# ACTION (both ways, 64 bytes): id u16, type u8, len u8, action/target/idle/state1/state2 u32, name 40 bytes —
#   what ActorMediator::PerformAction did for a player (Skyrim Together style); replayed on their puppet
ACTION_SIZE = 64
# Online saves (the player's characters kept on the server, by player name):
#   SAVE_PUT  client->server, SAVE_DATA server->client: SAVE_HDR + chunk (<= SAVE_CHUNK bytes)
#   SAVE_LIST_REQ (empty) -> SAVE_LIST: utf-8 lines "file	size	date"; SAVE_GET: file name (utf-8)
SAVE_HDR = struct.Struct('<II96s')      # total size, offset, file name
SAVE_CHUNK = 12288
# GIVE: items for another player: id u16 (to the server: receiver; from the server: giver), pad, FormID u32, count i32
GIVE_S = struct.Struct('<HHIi')
# HIT: damage to another player: id u16 (to the server: victim; from the server: attacker), pad, amount f32
HIT_S = struct.Struct('<HHf')
# FOLLOW (both ways): follower FormID u32, leader player id u16 (0 = released), pad — kept by the server
FOLLOW_S = struct.Struct('<IHH')
FOLLOW_HOME = 0xFFFF   # server -> clients: its owner is offline, the companion waits at home
# SOCIAL (client -> server): utf-8 command (see social.py); SOCIAL_INFO (server -> client): utf-8 answer
# QUEST (both ways): quest FormID u32, stage u16, player id u16 — relayed only when settings shared_quests
QUEST_S = struct.Struct('<IHH')
# NPC_DAMAGE (both ways): actor FormID u32, damage f32 — a player hurt a plugin actor; relayed to the others
DAMAGE_S = struct.Struct('<If')
# Updates (updates.py): UPD_LIST_REQ (empty) -> UPD_LIST utf-8 lines "path<TAB>size<TAB>crc32hex";
#   UPD_GET: path (utf-8) -> UPD_DATA: UPD_HDR + chunk (<= SAVE_CHUNK bytes)
UPD_HDR = struct.Struct('<II160s')     # total size, offset, path
# Avatar (the character as its own thing, see avatar.cpp): AVATAR_PUT / AVATAR_DATA = UPD_HDR (path "avatar") + chunk;
#   AVATAR_GET (empty) -> AVATAR_DATA chunks, or one AVATAR_DATA with total 0 when the player has none
AVATAR_MAX = 256 << 10
# Zones (MMO-style world ownership): ZONE (server -> client): owner id u16, pad; NPC_STATE (owner -> server ->
#   others in the zone): count u16, pad, then count x (formId u32, x y z rot speed f32, flags u16, pad u16)
ZONE_S = struct.Struct('<HH')
NPC_HDR = struct.Struct('<HH')
NPC_SIZE = 28
ZONE_OUTDOOR = 16384.0                 # outdoor zone: a square this wide (units) of the worldspace
# STATS (both ways): id u16, level u16, health / magicka / stamina percent u8 x3, flags u8 — the nameplate
STATS_S = struct.Struct('<HHBBBB')
# CHAT (both ways): id u16 (0 = the server), then utf-8 text; shown on the consoles as a notification
CHAT_HDR = struct.Struct('<H')
# EVENT (both ways): id u16, len u16, animation event name (ascii, e.g. "attackStart")
EVENT_HDR = struct.Struct('<HH')

# Recording (coop_server.py --record): per packet <d time, H player id, H type, H size> + payload
REC_S = struct.Struct('<dHHH')

# Room search over UDP on the same port: console broadcasts DISCOVER_Q,
# the server answers DISCOVER_A + <H tcp port> + room name (utf-8).
DISCOVER_Q, DISCOVER_A = b'SKYCOOP?', b'SKYCOOP!'

FLAG_MOVING, FLAG_RUNNING, FLAG_SPRINTING, FLAG_SNEAKING = 1, 2, 4, 8
FLAG_WEAPON_DRAWN, FLAG_IN_COMBAT, FLAG_JUMPING, FLAG_DEAD = 16, 32, 64, 128


def pack(ptype, payload=b''):
    return HEADER.pack(ptype, len(payload)) + payload


async def read_packet(reader):
    head = await reader.readexactly(HEADER.size)
    ptype, size = HEADER.unpack(head)
    return ptype, (await reader.readexactly(size) if size else b'')

# WORLD (03.10, the server owns the world): WorldHead(kind, count, ref) + count * (form u32, delta s32)
WORLD = 40
WORLD_HDR = struct.Struct('<HHI')
WK_CONTAINER = 1
WK_DOOR = 2          # count = bit 0 open, bit 1 locked
WK_CAMPAIGN = 8      # server -> client on join: ref = this server's campaign number (a save of it carries the number)
WK_ITEM = 7          # ref = item id (player id << 16 | n); count 1 dropped + base n world cell x y z / 0 gone
WK_MARK = 6          # count 1 set / 0 removed, ref = worldspace, then x y z floats; relayed with count |= id << 1
WK_SOUL = 4          # ref = the dragon that died near this player: everybody in the zone absorbs a soul
WK_WORD = 5          # ref = a Word of Power this player learned: everybody in the zone learns it
WK_ACTIVATE = 9      # (08.10, ST ActivateRequest) ref = an activator (lever, chain, button) the player used; relayed with count = player id
WK_SAY = 3           # count = text bytes, ref = the speaking player's id; then utf-8: an NPC's line in that player's dialogue
