#pragma once
/* Wire protocol v1 — must match server/protocol.py byte for byte. Little-endian. */
#include "common.hpp"

namespace coop::proto {

constexpr u16 VERSION = 2;
constexpr u16 PORT = 27015;

enum Type : u16 { HELLO = 1, WELCOME = 2, STATE = 3, LEFT = 4, EVENT = 5, CHAT = 6, INPUT = 7, LOG = 8, JOIN = 9, CMD = 10, APPEARANCE = 11, EQUIP = 12, TIME = 13, KILL = 14, ACTION = 15,
                 SAVE_PUT = 16, SAVE_LIST_REQ = 17, SAVE_LIST = 18, SAVE_GET = 19, SAVE_DATA = 20, GIVE = 21, HIT = 22, FOLLOW = 23, SOCIAL = 24, SOCIAL_INFO = 25, QUEST = 26, PLUGINS = 27, NPC_DAMAGE = 28,
                 UPD_LIST_REQ = 29, UPD_LIST = 30, UPD_GET = 31, UPD_DATA = 32,
                 AVATAR_PUT = 33, AVATAR_GET = 34, AVATAR_DATA = 35, ZONE = 36, NPC_STATE = 37, NPC_ACTION = 38, STATS = 39, WORLD = 40, TELE = 41 };   // TELE: client -> server, a text picture of the game for the test tools (playtest/play.py)   // NPC_DAMAGE: damage dealt to a plugin actor   // PLUGINS: client -> server, load order text   // SOCIAL: text commands (clans, marriage)   // ACTION: ActionMsg, a player's action (Skyrim Together style)   // KILL: a persistent actor died on someone's console   // TIME: TimeSync, host -> others (server picks the host)   // EQUIP: both ways, EquipHead + {u32 formId, u8 left, pad3}[count]   // APPEARANCE: both ways, AppearanceHead + TESNPC save bytes   // CMD: server -> client, console command text (local control)   // JOIN: server -> clients, id + name of a player   // LOG: client -> server, utf-8 text

struct Header { u16 type; u16 size; };
static_assert(sizeof(Header) == 4);

struct Hello { u16 version; char name[32]; } __attribute__((packed));
static_assert(sizeof(Hello) == 34);

struct Welcome { u16 id; u16 version; };
static_assert(sizeof(Welcome) == 4);

enum Flags : u32 {
    FLAG_MOVING = 1, FLAG_RUNNING = 2, FLAG_SPRINTING = 4, FLAG_SNEAKING = 8,
    FLAG_WEAPON_DRAWN = 16, FLAG_IN_COMBAT = 32, FLAG_JUMPING = 64, FLAG_DEAD = 128,
    FLAG_INTRO = 256,   // (03.10) still in the intro (MQ101): two such players ride the cart side by side
    FLAG_DOWN = 512,    // (07.10) knocked down online: a friend can get them up (death.cpp)
};

struct State {
    u16 id; u16 pad;
    u32 worldspace;   // FormID of TESWorldSpace, 0 in interiors
    u32 cell;         // FormID of TESObjectCELL
    f32 x, y, z;
    f32 rotZ;         // heading, radians
    u32 flags;
    f32 speed;
    u32 rightHand, leftHand;   // equipped item FormIDs, 0 = none
    u32 tick;
};
static_assert(sizeof(State) == 48);

struct Left { u16 id; };

struct Join { u16 id; char name[32]; } __attribute__((packed));
static_assert(sizeof(Join) == 34);

/* Test input from the server's local control port (server -> client): OR'ed into the
   controller state for `ms` milliseconds; sticks override when non-zero. */
struct Input { u64 buttons; s32 lx, ly, rx, ry; u32 ms; };
static_assert(sizeof(Input) == 32);

/* The player's NPC base as TESForm::SaveGame writes it (change flags below), FormIDs raw.
   Client -> server: id = 0; server -> clients: id of the player it belongs to. */
struct AppearanceHead { u16 id; u16 pad; u32 flags; };
static_assert(sizeof(AppearanceHead) == 8);
constexpr u32 APPEARANCE_MAX = 16384;

struct EquipHead { u16 id; u16 count; };
static_assert(sizeof(EquipHead) == 4);

/* CHAT: u16 id (0 = the server) + utf-8 text, both ways. */
struct ChatHead { u16 id; };

/* EVENT: u16 id, u16 length + animation graph event name ("attackStart", "blockStart", ...). */
struct EventHead { u16 id; u16 len; };

/* TIME: the room host's game clock (GameHour global 0x38, hours 0..24). */
struct TimeSync { u16 id; u16 pad; f32 hour; };
/* STATS (both ways, 2 Hz while it changes, 5 s otherwise): what the nameplate over a player shows — level and
   health / magicka / stamina in percent (0..100); the server stamps the id and keeps the last one */
struct StatsMsg { u16 id; u16 level; u8 hp, mp, sp, flags; };
static_assert(sizeof(StatsMsg) == 8);
static_assert(sizeof(TimeSync) == 8);

/* KILL: FormID of an actor from the game's plugins (not 0xFF...) that died; everybody kills their copy. */
/* flags (03.10, the server owns the world): client -> server KILL_RESPAWNS = not a unique actor (comes back by the
   server's timer); server -> client KILL_REVIVE = the server brought it back: resurrect your copy */
enum KillFlags : u16 { KILL_RESPAWNS = 1, KILL_REVIVE = 2 };
/* WORLD (both ways): the server-owned world state (world.cpp). WK_CONTAINER: ref = container / corpse FormID,
   count {u32 form, s32 delta against the base} pairs follow */
enum WorldKind : u16 { WK_CONTAINER = 1, WK_DOOR = 2, WK_SAY = 3, WK_SOUL = 4, WK_WORD = 5, WK_MARK = 6, WK_ITEM = 7, WK_CAMPAIGN = 8, WK_ACTIVATE = 9 };   // WK_ACTIVATE (08.10): ref = a lever / chain / button used; from the server count = player id   // WK_CAMPAIGN: server -> client on join, ref = its campaign number   // WK_ITEM: ref = item id, count 1 dropped (+ base, n, world, cell, x y z) / 0 gone   // WK_MARK: count 1 set / 0 removed, ref = worldspace, then x y z floats; from the server ref stays, count |= player id << 1   // WK_SOUL / WK_WORD: ref = the dragon / the word   // WK_SAY: count = text bytes, ref = player id (from the server); utf-8 follows   // WK_DOOR: count = bit 0 open, bit 1 locked; no pairs
struct WorldHead { u16 kind; u16 count; u32 ref; };
static_assert(sizeof(WorldHead) == 8);
struct KillMsg { u16 id; u16 flags; u32 formId; };
static_assert(sizeof(KillMsg) == 8);

/* ACTION: what ActorMediator::PerformAction did for a player; replayed on their puppet. */
struct ActionMsg { u16 id; u8 type; u8 len; u32 action, target, idle, state1, state2; char name[40]; };
static_assert(sizeof(ActionMsg) == 64);

/* Online saves: SAVE_PUT / SAVE_DATA = SaveHead + up to SAVE_CHUNK bytes of the file. */
struct SaveHead { u32 total, offset; char name[96]; };     // Skyrim's save names are ~65 characters
static_assert(sizeof(SaveHead) == 104);
constexpr u32 SAVE_CHUNK = 12288;

/* GIVE: items for another player. Client -> server: id = receiver; server -> receiver: id = giver. */
struct GiveMsg { u16 id; u16 pad; u32 formId; s32 count; };
static_assert(sizeof(GiveMsg) == 12);

/* HIT: damage to another player (PvP rooms only). Client -> server: id = victim; server -> victim: id = attacker. */
struct HitMsg { u16 id; u16 pad; f32 amount; };
static_assert(sizeof(HitMsg) == 8);

/* FOLLOW: a shared follower (plugin FormID) now follows player `leader` (0 = released). Kept by the server. */
struct FollowMsg { u32 formId; u16 leader; u16 pad; };
static_assert(sizeof(FollowMsg) == 8);

/* QUEST: a quest stage set on a console (relayed only when the server shares quests). */
struct QuestMsg { u32 quest; u16 stage; u16 id; };
static_assert(sizeof(QuestMsg) == 8);

/* Updates: UPD_LIST text "path<TAB>size<TAB>crc32hex" lines; UPD_DATA = UpdHead + chunk (SAVE_CHUNK max). */
struct UpdHead { u32 total, offset; char path[160]; };
static_assert(sizeof(UpdHead) == 168);
/* Avatar (avatar.cpp): AVATAR_PUT / AVATAR_DATA = UpdHead (path "avatar") + chunk; AVATAR_GET empty;
   AVATAR_DATA with total 0 = this player has no avatar on the server. */
constexpr u32 AVATAR_MAX = 64 * 1024;

/* The world around players, MMO style: the server splits it into zones (an interior cell; outdoors a square of
   the worldspace) and names one console the zone's owner (ZONE: owner player id). The owner's NPCs are the real
   ones: it sends their state (NPC_STATE: u16 count, u16 pad, then NpcState[count]); in the same zone the others
   move their copies of those NPCs the same way (their own AI does not walk them). */
struct ZoneMsg { u16 owner; u16 pad; };
struct NpcState { u32 formId; f32 x, y, z, rot, speed; u16 flags; u8 pad[2]; };
static_assert(sizeof(NpcState) == 28);
constexpr int NPC_MAX = 48;
enum NpcFlags : u16 { NPC_MOVING = 1, NPC_DRAWN = 2, NPC_DEAD = 4 };
/* NPC_ACTION (zone owner -> others in the zone): what an NPC of the owner did (attack, block, draw, cast...),
   replayed on the others' copy like a player's action on a puppet. */
struct NpcActionMsg { u32 formId; ActionMsg a; };
static_assert(sizeof(NpcActionMsg) == 68);

struct DamageMsg { u32 actor; f32 amount; };
static_assert(sizeof(DamageMsg) == 8);


/* ---- (08.10) the actor core, the Skyrim Together way (server/actors.py, actors.cpp). Every loaded actor has one
   owner console (its AI is real there); everywhere else it is remote: its own AI, movement and actions are held back
   and everything comes from the owner. */
enum ActType : u16 { ACT_ASSIGN = 50, ACT_ASSIGNED = 51, ACT_SPAWN = 52, ACT_MOVES = 53, ACT_ACTION = 54, ACT_RELEASE = 55,
                     ACT_OWNER = 56, ACT_HEALTH = 57, ACT_DEATH = 58, ACT_REMOVE = 59, ACT_EQUIP = 60, ACT_PROJ = 61, ACT_RESET = 0 };   // ACT_EQUIP: sid, count u16, pad, count x equip::Item   // ACT_RESET: local only (new connection)
struct ActAssign { u32 cookie, ref, base, pick, cell, world; f32 x, y, z, rot; u8 dead, temp; u16 pad; };
static_assert(sizeof(ActAssign) == 44);
struct ActAssigned { u32 cookie, sid; u16 owner; u8 mine, dead; u32 epoch; f32 x, y, z, rot, health; } __attribute__((packed));
static_assert(sizeof(ActAssigned) == 36);
struct ActSpawn { u32 sid, epoch; u16 owner; u8 dead, pad; u32 base, pick, cell, world; f32 x, y, z, rot; };
static_assert(sizeof(ActSpawn) == 44);
struct ActMovesHead { u32 tick; u16 count, pad; };
constexpr int ACT_FLOATS = 8, ACT_INTS = 8;
/* has: bits 0..7 floats present, 8..15 ints present, 16 bools present, 31 dead. A player's own row: sid = ACT_PLAYER | id */
struct ActMove { u32 sid; f32 x, y, z, rot; u32 cell, world; f32 health; u32 state1, state2; f32 f[ACT_FLOATS];
                 s16 i[ACT_INTS]; u32 bools, boolHas, has; f32 magicka, stamina; };
static_assert(sizeof(ActMove) == 108);
constexpr u32 ACT_PLAYER = 0x80000000u;
constexpr int ACT_ROWS = 18;                 // 18 x 108 + 8 fits the 2 KB packet slots
struct ActAction { u32 sid; ActionMsg a; };
static_assert(sizeof(ActAction) == 68);
struct ActSid { u32 sid, epoch; };
struct ActOwner { u32 sid, epoch; u16 owner, pad; };
struct ActHealth { u32 sid; f32 value; };
struct ActDeath { u32 sid; u8 dead, pad; u16 pad2; };
/* ACT_PROJ (08.10): a projectile launch of the shooter (sid, ACT_PLAYER | id for a player), see projectiles.cpp */
struct ProjMsg { u32 sid; f32 ox, oy, oz, a48, a4C, f60, f64; u32 base, weapon, ammo, cell, spell, poison, source;
                 u8 b7C, pad1[3]; s32 area; f32 power, scale; u8 flags[7], pad2; };
static_assert(sizeof(ProjMsg) == 84);

/* CLOCK (08.10, the Skyrim Together way): the campaign's game clock kept by the server — hour, time scale, day,
   month (0..11), year. Server -> client on join and every 10 s; client -> server: ours (taken while the campaign has none) */
constexpr u16 CLOCK = 42;
struct ClockMsg { f32 hour, scale, day, month, year; };
static_assert(sizeof(ClockMsg) == 20);

}  // namespace coop::proto
