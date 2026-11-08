#include "party.hpp"
#include "client.hpp"
#include "equip.hpp"
#include "game.hpp"
#include "lib.hpp"
#include "menu.hpp"
#include "quests.hpp"
#include <nn/os.hpp>
#include <cstring>

/* The party on the world map and the compass (03.10). SwitchPatch quest SSW_Party 0x12C0 (start game enabled) has 7
   optional reference aliases Party0..6 and objective 10 + i "<Alias=Party_i>". For every other player in our worldspace
   (or our interior) a beacon — ACTI 0x12C8 + i, an invisible marker named after the player — is placed where the
   player is, forced into alias i, and objective i shown: a quest marker on the map and the compass with their name.
   Beacons are placed anew when the player has moved 400+ units (every 2 s at most).
   1.1.177: ForceRefTo core 0x28a77c(quest, aliasId, ref); Quest.SetObjectiveDisplayed native 0xa70e40 (vm, stack,
   quest, index, displayed, force); the start of a quest that is not running 0x28df8c (quest, bool& queued, bool);
   TESObjectACTI: TESFullName at +0x30, its name (BSFixedString) at +0x38. */
namespace coop::party {
using proto::WorldHead;
namespace {

struct Fn { u32 off; u32 sig[4]; };
constexpr Fn FORCE_REF{0x28a77c, {0xD10203FF, 0xA9037BFD, 0x9100C3FD, 0xF90023F9}};
constexpr Fn SET_OBJ_SHOWN{0xa70e40, {0xA9BB7BFD, 0xF9000BFC, 0x910003FD, 0xA9025FF8}};
constexpr Fn ENSURE_STARTED{0x28df8c, {0xD100C3FF, 0xA9017BFD, 0x910043FD, 0xA9024FF4}};
constexpr Fn SET_ACTIVE{0xa70b58, {0xAA0203E0, 0x12000061, 0x17E06EB5, 0x00000000}};   // Quest.SetActive (vm, stack, quest, on)
constexpr Fn FS_CTOR{0xae6da8, {0xA9BE7BFD, 0xF9000BF3, 0x910003FD, 0xF900001F}};
constexpr u32 QUEST = 0x12C0, BEACON = 0x12C8, ACTI_NAME = 0x38;
constexpr int SLOTS = 7;

struct FixedString { const char* data; };
uintptr_t g_force, g_shown, g_start, g_fs, g_active;
bool g_tried, g_ok;

struct Slot { u16 id; void* ref; float x, y, z; bool shown; char name[32]; void* forced; };
Slot g_slot[SLOTS + 1];                 // + the map-marker slot (Show() keeps its flag here too)

bool Resolve() {
    g_tried = true;
    g_force = game::Checked177(FORCE_REF.off, FORCE_REF.sig);
    g_shown = game::Checked177(SET_OBJ_SHOWN.off, SET_OBJ_SHOWN.sig);
    g_start = game::Checked177(ENSURE_STARTED.off, ENSURE_STARTED.sig);
    g_fs = game::Checked177(FS_CTOR.off, FS_CTOR.sig);
    g_active = game::Checked177(SET_ACTIVE.off, SET_ACTIVE.sig);
    g_ok = g_force && g_shown && g_start && g_fs && g_active;   // (SetStage: through quests.cpp — its hook changed the code)
    Log("отряд на карте: %s", g_ok ? "есть" : "нет функций");
    return g_ok;
}

void Show(void* quest, int i, bool on) {
    reinterpret_cast<void (*)(void*, u32, void*, s32, bool, bool)>(g_shown)(game::FakeVm(), 0, quest, 10 + i, on, true);
    g_slot[i].shown = on;
}

void Clear(void* quest, int i) {
    if (g_slot[i].shown) Show(quest, i, false);
    if (g_slot[i].ref) game::DeleteRef(g_slot[i].ref);
    g_slot[i] = Slot{};
}

/* the beacon base's name = the player's (checked: the plugin's "Товарищ" must be there before it is first replaced) */
bool Name(void* base, const char* name) {
    if (!base || !game::Readable(base, 0xC8)) return false;
    int k = (int)((*reinterpret_cast<u32*>(static_cast<u8*>(base) + 0x14) & 0xFFFFFF) - BEACON);
    if (k < 0 || k >= SLOTS) return false;
    /* where the name lives: found once per beacon by its plugin text "Товарищ" (0x38 by the PC layout was wrong) */
    static int off[SLOTS];
    if (!off[k]) {
        off[k] = -1;
        for (int o = 0x20; o <= 0xC0; o += 8) {
            auto p = *reinterpret_cast<const char**>(static_cast<u8*>(base) + o);
            if (p && game::Readable(p, 16) && !strcmp(p, "Товарищ")) { off[k] = o; break; }
        }
        Log("отряд: имя маяка %d %s +%x", k, off[k] > 0 ? "найдено:" : "не найдено", off[k] > 0 ? off[k] : 0);
    }
    if (off[k] < 0) return false;
    auto fs = reinterpret_cast<FixedString*>(static_cast<u8*>(base) + off[k]);
    if (fs->data && game::Readable(fs->data, 1) && !strcmp(fs->data, name)) return true;
    reinterpret_cast<void (*)(FixedString*, const char*)>(g_fs)(fs, name);
    return true;
}

/* the marker beacon's name: the same field as the others' (found by its plugin text "Метка" once) */
void NameMark(void* base, const char* name) {
    static int off;
    if (!base || !game::Readable(base, 0xC8)) return;
    if (!off) {
        off = -1;
        for (int o = 0x20; o <= 0xC0; o += 8) {
            auto p = *reinterpret_cast<const char**>(static_cast<u8*>(base) + o);
            if (p && game::Readable(p, 8) && !strncmp(p, "Метка", strlen("Метка"))) { off = o; break; }
        }
    }
    if (off > 0) reinterpret_cast<void (*)(FixedString*, const char*)>(g_fs)(reinterpret_cast<FixedString*>(static_cast<u8*>(base) + off), name);
}

/* ---- travelling together (03.10): a friend who stood next to us and is suddenly far away took a carriage, fast
   travelled or went through a door — we are asked "go along?" and are put next to them */
struct Seen { u16 id; u32 cell, world; float x, y, z; bool near; };
Seen g_seen[MAX_REMOTE];
struct Target { u16 id; u32 cell, world; float x, y, z; char name[32]; } g_target;

void GoAlong() {
    char msg[96];
    snprintf(msg, sizeof msg, "Ты рядом с %s", g_target.name);
    static char arrived[96];
    snprintf(arrived, sizeof arrived, "%s", msg);
    game::TeleportTo(g_target.cell, g_target.world, g_target.x + 80.0f, g_target.y + 80.0f, g_target.z, arrived);
}

void WatchTravel(const RemotePlayer* rs, int n, u32 cell, u32 world, const float* me) {
    for (int k = 0; k < n; k++) {
        const auto& s = rs[k].state;
        if (!s.cell) continue;                                     // loading: decide when they are somewhere
        Seen* sn = nullptr;
        for (auto& e : g_seen) if (e.id == s.id) sn = &e;
        for (auto& e : g_seen) if (!sn && !e.id) { sn = &e; *sn = Seen{s.id, s.cell, s.worldspace, s.x, s.y, s.z, false}; }
        if (!sn) continue;
        float dx = s.x - me[0], dy = s.y - me[1];
        bool sameSpace = world ? s.worldspace == world : s.cell == cell;
        bool nearNow = sameSpace && dx * dx + dy * dy < 600.0f * 600.0f;
        float jx = s.x - sn->x, jy = s.y - sn->y;
        bool jumped = s.worldspace != sn->world || (!s.worldspace && s.cell != sn->cell) || jx * jx + jy * jy > 6000.0f * 6000.0f;
        if (sn->near && jumped && !nearNow) {
            g_target = Target{s.id, s.cell, s.worldspace, s.x, s.y, s.z, {}};
            if (!PlayerName(s.id, g_target.name, sizeof g_target.name)) snprintf(g_target.name, sizeof g_target.name, "товарищ");
            char text[200];
            snprintf(text, sizeof text, "%s отправился в путь (повозка, быстрое перемещение или дверь).\nОтправиться вместе?", g_target.name);
            menu::Offer(text, "Вместе", "Остаться", &GoAlong);
            Log("отряд: %s уехал — предложено ехать вместе", g_target.name);
        }
        *sn = Seen{s.id, s.cell, s.worldspace, s.x, s.y, s.z, nearNow};
    }
}

/* ---- a friend's map marker (03.10): the custom marker the player places on the map (its reference handle at
   PlayerCharacter +0x8dc, as the map's remove-marker code reads it) goes to the others; theirs is shown to us in
   slot 7 of SSW_Party (beacon ACTI 0x12CF "Метка: <name>", objective 17) when it is in our worldspace */
constexpr u32 MARK_PLAYER_OFF = 0x8dc;
constexpr int MARK_SLOT = 7;
struct Mark { u16 from; bool set; u32 world; float x, y, z; };
Mark g_mark, g_markIn;                           // g_markIn: from the network thread
volatile bool g_markFresh;
bool g_markSent; float g_sentPos[3];
void* g_markRef; bool g_markShown;

void SendOurMark(void* player) {
    u32 h = *reinterpret_cast<u32*>(static_cast<u8*>(player) + MARK_PLAYER_OFF);
    void* ref = game::RefFromHandle(h);
    if (ref) {
        const float* p = game::RefPos(ref);
        float dx = p[0] - g_sentPos[0], dy = p[1] - g_sentPos[1];
        if (g_markSent && dx * dx + dy * dy < 200.0f * 200.0f) return;
        u8 buf[sizeof(WorldHead) + 12];
        void* w = game::WorldOf(ref);
        WorldHead hd{proto::WK_MARK, 1, w ? *reinterpret_cast<u32*>(static_cast<u8*>(w) + 0x14) : 0u};
        memcpy(buf, &hd, sizeof hd);
        memcpy(buf + sizeof hd, p, 12);
        PushWorld(buf, sizeof buf);
        memcpy(g_sentPos, p, 12);
        g_markSent = true;
        Log("отряд: моя метка на карте -> всем (%.0f %.0f)", p[0], p[1]);
    } else if (g_markSent) {
        WorldHead hd{proto::WK_MARK, 0, 0};
        PushWorld(&hd, sizeof hd);
        g_markSent = false;
    }
}

void ShowTheirMark(void* quest, int patch, u32 world) {
    if (__atomic_exchange_n(&g_markFresh, false, __ATOMIC_ACQ_REL)) {
        g_mark = g_markIn;
        if (g_markRef) { game::DeleteRef(g_markRef); g_markRef = nullptr; }
    }
    bool visible = g_mark.set && g_mark.world && g_mark.world == world;
    if (!visible) {
        if (g_markShown) { Show(quest, MARK_SLOT, false); g_markShown = false; }
        return;
    }
    if (!g_markRef) {
        void* base = equip::Lookup(((u32)patch << 24) | (BEACON + MARK_SLOT));
        if (!base) return;
        char who[32], name[48];
        if (!PlayerName(g_mark.from, who, sizeof who)) snprintf(who, sizeof who, "товарищ");
        snprintf(name, sizeof name, "Метка: %s", who);
        NameMark(base, name);
        g_markRef = game::PlaceObject(base, g_mark.x, g_mark.y, g_mark.z, true);
        if (!g_markRef) return;
        reinterpret_cast<void (*)(void*, u32, void*)>(g_force)(quest, (u32)MARK_SLOT, g_markRef);
    }
    if (!g_markShown) { Show(quest, MARK_SLOT, true); g_markShown = true; }
}

}  // namespace

void MarkReceived(const u8* p, u32 size) {
    if (size < sizeof(WorldHead)) return;
    WorldHead h; memcpy(&h, p, sizeof h);
    Mark m{(u16)(h.count >> 1), (h.count & 1) != 0, h.ref, 0, 0, 0};
    if (m.set && size >= sizeof h + 12) memcpy(&m.x, p + sizeof h, 12);
    g_markIn = m;
    __atomic_store_n(&g_markFresh, true, __ATOMIC_RELEASE);
}

void Pump() {
    if (Status() != NetStatus::Connected) return;
    static u64 at;
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    if (now - at < 2000) return;
    at = now;
    if (!g_tried && !Resolve()) return;
    if (!g_ok) return;
    static int patch = -2;
    if (patch == -2) patch = game::PluginIndex("SwitchPatch.esp");
    if (patch < 0) return;
    auto quest = static_cast<u8*>(equip::Lookup(((u32)patch << 24) | QUEST));
    if (!quest || quest[0x1A] != 0x4D) return;
    if (!(quest[0xC4] & 1)) {                                       // not running yet (an old save): start it
        bool queued = false;
        reinterpret_cast<bool (*)(void*, bool*, bool)>(g_start)(quest, &queued, false);
        return;
    }
    if (*reinterpret_cast<u16*>(quest + 0x1B8) < 10)                 // its stage 10: objectives as in any vanilla quest
        quests::SetStage(quest, 10);
    /* active in the journal (or no markers on the map and the compass); again now and then: a load resets it */
    static u64 activeAt;
    if (now - activeAt > 30000) {
        activeAt = now;
        reinterpret_cast<void (*)(void*, u32, void*, bool)>(g_active)(game::FakeVm(), 0, quest, true);
    }
    u32 cell, world; float pos[3];
    if (!game::WhereAmI(&cell, &world, pos)) return;
    RemotePlayer rs[MAX_REMOTE];
    int n = SnapshotRemotes(rs, MAX_REMOTE);
    WatchTravel(rs, n, cell, world, pos);
    SendOurMark(game::Player());
    ShowTheirMark(quest, patch, world);
    /* the slots of players who left */
    for (int i = 0; i < SLOTS; i++) {
        if (!g_slot[i].id) continue;
        bool present = false;
        for (int k = 0; k < n; k++) present |= rs[k].state.id == g_slot[i].id;
        if (!present) Clear(quest, i);
    }
    for (int k = 0; k < n; k++) {
        const auto& s = rs[k].state;
        int i = -1;
        for (int j = 0; j < SLOTS; j++) if (g_slot[j].id == s.id) i = j;
        for (int j = 0; i < 0 && j < SLOTS; j++) if (!g_slot[j].id) { i = j; g_slot[j].id = s.id; }
        if (i < 0) continue;
        Slot& sl = g_slot[i];
        bool visible = world ? s.worldspace == world : (s.cell == cell && s.cell);
        if (!visible || !s.cell) {                                  // another worldspace / another interior / loading
            if (sl.shown) Show(quest, i, false);
            continue;
        }
        /* near (their puppet is here): the puppet itself is the target — an actor with their name */
        if (void* pup = game::PuppetRef(s.id)) {
            if (sl.forced != pup) {
                reinterpret_cast<void (*)(void*, u32, void*)>(g_force)(quest, (u32)i, pup);
                sl.forced = pup;
            }
            if (!sl.shown) Show(quest, i, true);
            continue;
        }
        float dx = s.x - sl.x, dy = s.y - sl.y, dz = s.z - sl.z;
        if (!sl.ref || sl.forced != sl.ref || dx * dx + dy * dy + dz * dz > 400.0f * 400.0f) {
            void* base = equip::Lookup(((u32)patch << 24) | (BEACON + i));
            if (!base) continue;
            char name[32];
            if (!PlayerName(s.id, name, sizeof name)) snprintf(name, sizeof name, "Игрок %u", s.id);
            if (strcmp(name, sl.name)) { if (Name(base, name)) snprintf(sl.name, sizeof sl.name, "%s", name); }
            if (sl.ref) game::DeleteRef(sl.ref);
            sl.ref = game::PlaceObject(base, s.x, s.y, s.z + 100.0f, true);   // persistent: quest markers need it?
            sl.x = s.x; sl.y = s.y; sl.z = s.z;
            if (!sl.ref) continue;
            reinterpret_cast<void (*)(void*, u32, void*)>(g_force)(quest, (u32)i, sl.ref);
            sl.forced = sl.ref;
        }
        if (!sl.shown) Show(quest, i, true);
    }
}

}  // namespace coop::party
