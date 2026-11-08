#include "death.hpp"
#include "quests.hpp"
#include "client.hpp"
#include "equip.hpp"
#include "game.hpp"
#include "truehud.hpp"
#include "protocol.hpp"
#include "lib.hpp"
#include <nn/os.hpp>
#include <cstring>
#include <cstdio>

/* Death in the online world (03.10, the user): no save reload. While in a room the player is essential with no
   bleedout recovery (Skyrim Together's respawn mode): "death" is a knock-down; 5 s later the player gets up with full
   health — inside an interior / dungeon at the door they came in through (where they appeared in this cell), in the
   open world at the nearest city. All through the game's own Papyrus natives (1.1.177, signature-checked). */
namespace coop::death {
namespace {

struct Fn { u32 off; u32 sig[4]; };
constexpr Fn IS_BLEEDING{0xa00cf0, {0xB940F848, 0x53156108, 0x51001D08, 0x7100091F}};      // Actor.IsBleedingOut (vm, stack, actor)
constexpr Fn NO_RECOVERY{0xa03874, {0xAA0203E0, 0x12000061, 0x17F1A009, 0xAA0203E0}};     // Actor.SetNoBleedoutRecovery (vm, stack, actor, bool)
constexpr Fn RESTORE_AV{0xa02820, {0xD10783FF, 0xFD00D3E8, 0xF900DFFC, 0xA91AFBFD}};       // Actor.RestoreActorValue (vm, stack, actor, name, v)
constexpr Fn SET_ESSENTIAL{0xa44074, {0xA9BE7BFD, 0xA9014FF4, 0x910003FD, 0x2A0303F4}};    // ActorBase.SetEssential (vm, stack, base, bool)
constexpr Fn UNCONSCIOUS{0xa04420, {0xA9BE7BFD, 0xF9000BF3, 0x910003FD, 0xAA0203F3}};   // Actor.SetUnconscious (vm, stack, actor, bool)
constexpr Fn MOVE_TO{0xa51e00, {0xD10203FF, 0xFD0013EA, 0xF90027F9, 0x6D02A3E9}};          // ObjectReference.MoveTo
constexpr Fn GET_AV{0x9fe0f0, {0xD10783FF, 0xA91A7BFD, 0x910683FD, 0xF900DBFC}};          // Actor.GetActorValue
constexpr Fn DAMAGE_AV{0x9fd2c0, {0xD107C3FF, 0xFD00D3E8, 0xA91B7BFD, 0x9106C3FD}};       // Actor.DamageActorValue
constexpr Fn PUSH_AWAY{0xa54540, {0xD10143FF, 0xFD000BE8, 0xF90017F7, 0xA901FBFD}};       // ObjectReference.PushActorAway (vm, stack, self, actor, force)
constexpr u32 FS_CTOR = 0xae6da8;
struct FixedString { const char* data; };

/* cities: the map marker (in Tamriel: which city is nearest) and where you get up — (07.10, his: "на центральных площадях,
   а не на крышах домов") a marker on the central square / market (Skyrim.esm refs; the walled cities' squares are in
   their own worldspaces, MoveTo takes you there). Raven Rock: its map marker */
struct City { u32 obj; u32 spot; bool dragonborn; const char* name; };
constexpr City CITIES[] = {
    {0x0162CE, 0x02BE1B, false, "Вайтран"},  {0x04D0F4, 0x04DEB7, false, "Солитьюд"},  {0x038436, 0x0E7BF8, false, "Виндхельм"},
    {0x01C390, 0x05EEFF, false, "Рифтен"},   {0x01C38A, 0x036F9D, false, "Маркарт"},   {0x0177B0, 0x02D73E, false, "Морфал"},
    {0x01773A, 0x024B0F, false, "Данстар"},  {0x0177EF, 0x02D743, false, "Винтерхолд"}, {0x017760, 0x02D736, false, "Фолкрит"},
    {0x0162A4, 0x01BDF3, false, "Ривервуд"}, {0x0143DC, 0, true, "Рейвен-Рок"},
};

constexpr u64 DOWN_MS = 90000, DOWN_ALONE_MS = 5000;   // (07.10) 90 s for a friend to come; nobody in the room: 5 s
volatile bool g_isDown, g_giveUp;
volatile u16 g_revivedBy;
uintptr_t g_bleeding, g_noRecovery, g_restore, g_essential, g_moveTo, g_unconscious, g_getAv, g_damageAv, g_pushAway;
bool g_ready, g_modeOn;
u64 g_downSince, g_upAt;
u32 g_entryCell;                   // the interior we are in and where we appeared in it (its door)
float g_entryPos[3];
u32 g_lastCell;
u64 g_cellSince;
FixedString g_health;

bool Resolve() {
    g_bleeding = game::Checked177(IS_BLEEDING.off, IS_BLEEDING.sig);
    g_noRecovery = game::Checked177(NO_RECOVERY.off, NO_RECOVERY.sig);
    g_restore = game::Checked177(RESTORE_AV.off, RESTORE_AV.sig);
    g_essential = game::Checked177(SET_ESSENTIAL.off, SET_ESSENTIAL.sig);
    g_moveTo = game::Checked177(MOVE_TO.off, MOVE_TO.sig);
    g_unconscious = game::Checked177(UNCONSCIOUS.off, UNCONSCIOUS.sig);
    g_getAv = game::Checked177(GET_AV.off, GET_AV.sig);
    g_damageAv = game::Checked177(DAMAGE_AV.off, DAMAGE_AV.sig);
    g_pushAway = game::Checked177(PUSH_AWAY.off, PUSH_AWAY.sig);
    if (!g_pushAway) Log("смерть онлайн: PushActorAway не найден");
    reinterpret_cast<void (*)(FixedString*, const char*)>(game::ModuleAddr(FS_CTOR))(&g_health, "Health");
    Log("смерть онлайн: %s", g_bleeding && g_noRecovery && g_restore && g_essential && g_moveTo ? "есть" : "нет функций");
    return g_bleeding && g_noRecovery && g_restore && g_essential && g_moveTo;
}

void Mode(void* player, bool on) {
    void* base = *reinterpret_cast<void**>(static_cast<u8*>(player) + 0x40);
    reinterpret_cast<void (*)(void*, u32, void*, bool)>(g_essential)(game::FakeVm(), 0, base, on);
    reinterpret_cast<void (*)(void*, u32, void*, bool)>(g_noRecovery)(game::FakeVm(), 0, player, on);
    if (on != g_modeOn) Log("смерть онлайн: режим %s", on ? "включён (без перезагрузки)" : "выключен");
    g_modeOn = on;                                   // (re-set every 10 s: a load resets it; logged on a change only)
}

void* NearestCity(const float* pos) {
    static int db = -2;
    if (db == -2) db = game::PluginIndex("Dragonborn.esm");
    void* best = nullptr; float bd = 0; const char* name = "";
    void* myWorld = game::WorldOf(game::Player());
    for (const auto& c : CITIES) {
        if (c.dragonborn && db < 0) continue;
        void* m = equip::Lookup(c.dragonborn ? ((u32)db << 24) | c.obj : c.obj);
        if (!m || game::WorldOf(m) != myWorld) continue;
        const float* mp = game::RefPos(m);
        float dx = mp[0] - pos[0], dy = mp[1] - pos[1], d = dx * dx + dy * dy;
        if (!best || d < bd) { void* sp = c.spot ? equip::Lookup(c.spot) : nullptr; best = sp ? sp : m; bd = d; name = c.name; }
    }
    if (!best) { best = equip::Lookup(CITIES[0].spot); if (!best) best = equip::Lookup(CITIES[0].obj); name = CITIES[0].name; }   // another worldspace: Whiterun
    Log("смерть онлайн: ближайший город — %s", name);
    return best;
}

/* (07.10, his: "без сознания — падать на землю и ничего не делать, как при смерти") lying on the ground, no control */
void Unconscious(void* player, bool on) {
    if (g_unconscious) reinterpret_cast<void (*)(void*, u32, void*, bool)>(g_unconscious)(game::FakeVm(), 0, player, on);
}

void GetUp(void* player) {
    Unconscious(player, false);
    game::SendAnimEvent(player, "IdleChairExitStart");
    game::SendAnimEvent(player, "GetUpBegin");
    reinterpret_cast<void (*)(void*, u32, void*, bool)>(g_noRecovery)(game::FakeVm(), 0, player, false);
    reinterpret_cast<void (*)(void*, u32, void*, FixedString*, float)>(g_restore)(game::FakeVm(), 0, player, &g_health, 100000.0f);
    u32 cell, world; float pos[3];
    if (!game::WhereAmI(&cell, &world, pos)) return;
    if (!game::WorldOf(player) && g_entryCell) {
        game::TeleportTo(g_entryCell, 0, g_entryPos[0], g_entryPos[1], g_entryPos[2], "Ты пришёл в себя у входа");
        Log("смерть онлайн: подъём у входа в подземелье %08x (умер в %08x)", g_entryCell, cell);
    } else if (void* city = NearestCity(pos)) {
        reinterpret_cast<void (*)(void*, u32, void*, void*, float, float, float, bool)>(g_moveTo)(game::FakeVm(), 0, player, city, 0, 0, 0, true);
        game::Notify("Ты пришёл в себя в ближайшем городе");
    }
}

/* a friend got us up: here, on our feet with half the health */
void GetUpHere(void* player) {
    Unconscious(player, false);
    game::SendAnimEvent(player, "IdleChairExitStart");
    game::SendAnimEvent(player, "GetUpBegin");
    reinterpret_cast<void (*)(void*, u32, void*, bool)>(g_noRecovery)(game::FakeVm(), 0, player, false);
    reinterpret_cast<void (*)(void*, u32, void*, FixedString*, float)>(g_restore)(game::FakeVm(), 0, player, &g_health, 100000.0f);
    /* restored health alone leaves the player in the bleed-out pose (Eden 07.10): a move onto the same spot gets
       them up, as the get-up at a door does */
    reinterpret_cast<void (*)(void*, u32, void*, void*, float, float, float, bool)>(g_moveTo)(game::FakeVm(), 0, player, player, 0, 0, 0, true);
}

/* friends who just went down: a line on our screen (they show it as "без сознания" over their head too) */
void WatchFriends() {
    static u16 down[MAX_REMOTE];
    static int downN;
    RemotePlayer rs[MAX_REMOTE];
    int n = SnapshotRemotes(rs, MAX_REMOTE);
    u16 now[MAX_REMOTE]; int nowN = 0;
    for (int i = 0; i < n; i++) {
        if (!(rs[i].state.flags & proto::FLAG_DOWN)) continue;
        u16 id = rs[i].state.id;
        now[nowN++] = id;
        bool was = false;
        for (int k = 0; k < downN; k++) was |= down[k] == id;
        if (!was) {
            char name[32], msg[160];
            if (!PlayerName(id, name, sizeof name)) snprintf(name, sizeof name, "Товарищ");
            snprintf(msg, sizeof msg, "<font color='#FF5A3C' size='26'>%s без сознания — подойди и нажми A, чтобы поднять</font>", name);
            game::Notify(msg);
        }
    }
    memcpy(down, now, sizeof now);
    downN = nowN;
}

}  // namespace

bool IsDown() { return g_isDown; }
void GiveUp() { g_giveUp = true; }
void Healed(u16 by, float amount) {
    void* player = game::Player();
    if (!player || !g_restore) return;
    if (g_isDown) { RevivedBy(by); return; }               // a heal gets a knocked-down friend up
    reinterpret_cast<void (*)(void*, u32, void*, FixedString*, float)>(g_restore)(game::FakeVm(), 0, player, &g_health, amount);
    char name[32], msg[120];
    if (!PlayerName(by, name, sizeof name)) snprintf(name, sizeof name, "товарищ");
    snprintf(msg, sizeof msg, "%s лечит тебя: +%.0f", name, amount);
    game::Notify(msg);
}
void RevivedBy(u16 playerId) { if (g_isDown) g_revivedBy = playerId ? playerId : 1; }

bool ReviveFriend(u16 playerId) {
    RemotePlayer rs[MAX_REMOTE];
    int n = SnapshotRemotes(rs, MAX_REMOTE);
    for (int i = 0; i < n; i++)
        if (rs[i].state.id == playerId && (rs[i].state.flags & proto::FLAG_DOWN)) {
            char ev[32], name[32], msg[120];
            snprintf(ev, sizeof ev, "cooprevive %u", playerId);
            PushEvent(ev);
            if (!PlayerName(playerId, name, sizeof name)) snprintf(name, sizeof name, "товарища");
            snprintf(msg, sizeof msg, "Ты поднимаешь: %s", name);
            game::Notify(msg);
            Log("нокдаун: поднимаю игрока %u", playerId);
            return true;
        }
    return false;
}

void Pump() {
    void* player = game::Player();
    if (!player || !game::Settled()) return;
    if (!g_ready) { g_ready = true; if (!Resolve()) g_bleeding = 0; }
    if (!g_bleeding) return;
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    /* (04.10) not during the intro: a death in Helgen sent the player to Riverwood and the intro (and with it the main
       quest) never finished — there the game's own death (load the last save) */
    bool online = Status() == NetStatus::Connected && quests::IntroDone();
    static u64 modeCheck;
    /* (08.10, Eden: knocked-down players stood up ~20 s in) not while lying: SetEssential again on a downed player ends the
       game's knock-down */
    if (online != g_modeOn || (online && now - modeCheck > 10000 && !g_downSince)) {   // again now and then: a load resets it
        modeCheck = now;
        Mode(player, online);
    }
    if (!online) { if (g_isDown) { g_isDown = false; truehud::HudText(""); } return; }
    WatchFriends();
    /* the door of the interior we are in: where we stood 3 s after coming in */
    u32 cell, world; float pos[3];
    if (game::WhereAmI(&cell, &world, pos)) {
        if (cell != g_lastCell) { g_lastCell = cell; g_cellSince = now; }
        /* (07.10, his: "помру в подземелье — респавн в его начале") only the FIRST interior after the open world counts:
           a dungeon of several parts sends you back to where you came in from outside, not to the hall you died in */
        else if (!game::WorldOf(player) && !g_entryCell && now - g_cellSince > 3000) {
            g_entryCell = cell;
            memcpy(g_entryPos, pos, sizeof pos);
            Debug("смерть онлайн: вход в %08x запомнен", cell);
        }
        if (game::WorldOf(player)) g_entryCell = 0;
    }
    bool down = reinterpret_cast<bool (*)(void*, u32, void*)>(g_bleeding)(game::FakeVm(), 0, player);
    if (down && !g_downSince && !g_upAt) {               // (03.10: right after getting up it is still "bleeding" for a while)
        g_downSince = now;
        g_isDown = true; g_giveUp = false; g_revivedBy = 0;
        /* (08.10, Eden: the "Ragdoll" graph event drops an actor like a dead one and it stays down; "GetUpBegin" gets
           it up) — ours here and our puppet on the others' screens */
        /* (08.10, Eden: up again by itself after ~13 s) the engine's own bleed-out recovery: off for as long as we lie */
        reinterpret_cast<void (*)(void*, u32, void*, bool)>(g_noRecovery)(game::FakeVm(), 0, player, true);
        game::SendAnimEvent(player, "Ragdoll");            // falls like a dead one (here and on the others' screens)
        PushEvent("Ragdoll");
        Log("смерть онлайн: игрок упал (нокдаун)");
    }
    if (g_downSince) {
        /* (08.10, Eden) after the fall the lying-on-the-ground idle (IdleLayDownEnterInstant, no furniture needed): it
           holds until the get-up (refused while it plays, so sent again every 6 s for the case it was not taken) */
        static u64 lastLay;
        if (now - g_downSince < 300) lastLay = 0;
        if (now - g_downSince > 2500 && (!lastLay || now - lastLay > 6000)) {
            lastLay = now;
            game::SendAnimEvent(player, "IdleLayDownEnterInstant");
            PushEvent("IdleLayDownEnterInstant");
        }
        /* (08.10, Eden: Serana's healing hands stood the player up after ~15 s) nobody's healing lifts a knock-down —
           only a friend's (A / a heal through coopheal, death::RevivedBy / Healed): health stays at zero */
        if (g_getAv && g_damageAv) {
            float h = reinterpret_cast<float (*)(void*, u32, void*, FixedString*)>(g_getAv)(game::FakeVm(), 0, player, &g_health);
            if (h > 0.0f)
                reinterpret_cast<void (*)(void*, u32, void*, FixedString*, float)>(g_damageAv)(game::FakeVm(), 0, player, &g_health, h + 1.0f);
        }
        RemotePlayer rs[MAX_REMOTE];
        u64 limit = SnapshotRemotes(rs, MAX_REMOTE) > 0 ? DOWN_MS : DOWN_ALONE_MS;
        u64 left = now - g_downSince < limit ? (limit - (now - g_downSince) + 999) / 1000 : 0;
        char t[200];
        snprintf(t, sizeof t, "БЕЗ СОЗНАНИЯ — %u<br><font size='22' color='#E8DCC0'>товарищ может поднять тебя (A) · L3+R3 — сдаться</font>", (u32)left);
        truehud::HudText(t);
        static u64 lastSaid;                               // the HUD may be hidden while bleeding out: a line now and then too
        if (left && (left == 90 || left == 60 || left == 30 || left == 10 || left == 5) && lastSaid != left) {
            lastSaid = left;
            char m[160];
            snprintf(m, sizeof m, "<font color='#FF5A3C' size='26'>Без сознания: %u с. Товарищ может поднять (A), L3+R3 — сдаться</font>", (u32)left);
            game::Notify(m);
        }
        u16 by = g_revivedBy;
        if (by) {                                          // a friend's A: up here
            char name[32], msg[120];
            if (!PlayerName(by, name, sizeof name)) snprintf(name, sizeof name, "товарищ");
            snprintf(msg, sizeof msg, "<font color='#FFD24A' size='26'>Тебя поднял(а): %s</font>", name);
            GetUpHere(player);
            game::Notify(msg);
            Log("нокдаун: поднят игроком %u", by);
        } else if (g_giveUp || !left) {                    // gave up / nobody came: the usual get-up
            Log("нокдаун: %s", g_giveUp ? "сдался" : "время вышло");
            GetUp(player);
        }
        if (by || g_giveUp || !left) {
            g_downSince = 0; g_isDown = false; g_giveUp = false; g_revivedBy = 0;
            truehud::HudText("");
            PushEvent("IdleChairExitStart");
            PushEvent("GetUpBegin");
            g_upAt = now;
        }
    }
    if (g_upAt && now - g_upAt > 3000) {                 // back to the knock-down mode
        g_upAt = 0;
        reinterpret_cast<void (*)(void*, u32, void*, bool)>(g_noRecovery)(game::FakeVm(), 0, player, true);
    }
}

}  // namespace coop::death
