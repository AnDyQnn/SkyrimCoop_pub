#include "world.hpp"
#include "party.hpp"
#include "items.hpp"
#include "client.hpp"
#include "quests.hpp"
#include "equip.hpp"
#include "game.hpp"
#include "trade.hpp"
#include "lib.hpp"
#include <nn/os.hpp>
#include <cstring>

/* Containers and corpses, one for the whole world.
   - The player opens a container (ActivateHook in trade.cpp calls Touched): from then on, while it is loaded, its
     changes against the base (ExtraContainerChanges, trade::Changes) are watched; a new state goes to the server
     (WORLD, kind WK_CONTAINER: ref, then {form, delta} pairs).
   - The server keeps the last state of each container and sends it to everybody (and to newcomers). Here each state
     is kept and applied whenever that reference is loaded: the local delta is brought to the server's by adding /
     removing the difference. Only references from the plugins (not 0xFF...) — the same on every console. */
namespace coop::world {
namespace {

using proto::WorldHead;
constexpr int TOUCH_MAX = 16, STATE_MAX = 128, ITEMS_MAX = 120;

struct Item { u32 form; s32 n; };
struct State { u32 ref; u16 count; bool fresh; Item items[ITEMS_MAX]; };

void* g_touched[TOUCH_MAX];
u32 g_touchedId[TOUCH_MAX];
int g_touchedN;
State g_sent[TOUCH_MAX];           // what we last sent per touched container
u64 g_quiet[TOUCH_MAX];            // no sending before this time: the game applies AddItems a frame or two later
State g_server[STATE_MAX];         // the server's states (applied when loaded)
int g_serverN;
nn::os::MutexType g_lock;
bool g_lockReady;

void Lock() { if (!g_lockReady) { nn::os::InitializeMutex(&g_lock, false, 0); g_lockReady = true; } nn::os::LockMutex(&g_lock); }
void Unlock() { nn::os::UnlockMutex(&g_lock); }

u32 Id(const void* f) { return *reinterpret_cast<const u32*>(static_cast<const u8*>(f) + 0x14); }
/* ---- merchants (03.10): a vendor's goods and gold live in its merchant chest (a persistent reference in a hidden
   cell, never loaded) — when a player talks to a vendor, its chest is watched like any container, so what one buys
   or sells is gone / there for the others too */
#include "vendors.inc"
u32 g_vendorChest[16];
int g_vendorChestN;

bool IsVendorChest(u32 id) {
    for (int i = 0; i < g_vendorChestN; i++) if (g_vendorChest[i] == id) return true;
    return false;
}

u32 VendorChestOf(u32 npcBase) {
    static const char* const FILES[5] = {"Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"};
    static int idx[5] = {-2, -2, -2, -2, -2};
    for (const auto& v : VENDORS) {
        if (idx[v.npcFile] == -2) idx[v.npcFile] = game::PluginIndex(FILES[v.npcFile]);
        if (idx[v.npcFile] < 0 || npcBase != (((u32)idx[v.npcFile] << 24) | v.npc)) continue;
        if (idx[v.chestFile] == -2) idx[v.chestFile] = game::PluginIndex(FILES[v.chestFile]);
        return idx[v.chestFile] < 0 ? 0 : (((u32)idx[v.chestFile] << 24) | v.chest);
    }
    return 0;
}

bool Loaded(void* ref) {
    return ref && (*reinterpret_cast<void**>(static_cast<u8*>(ref) + 0x68) || IsVendorChest(Id(ref)));
}

int Snapshot(void* ref, Item* out) {
    static trade::Count c[ITEMS_MAX];
    int n = trade::Changes(ref, c, ITEMS_MAX), k = 0;
    for (int i = 0; i < n; i++) if (c[i].n) out[k++] = Item{c[i].id, c[i].n};
    return k;
}

bool Same(const Item* a, int na, const Item* b, int nb) {
    if (na != nb) return false;
    for (int i = 0; i < na; i++) {
        bool f = false;
        for (int j = 0; j < nb && !f; j++) f = a[i].form == b[j].form && a[i].n == b[j].n;
        if (!f) return false;
    }
    return true;
}

void Send(u32 ref, const Item* it, int n) {
    static u8 buf[sizeof(WorldHead) + ITEMS_MAX * sizeof(Item)];
    WorldHead h{proto::WK_CONTAINER, (u16)n, ref};
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, it, n * sizeof(Item));
    PushWorld(buf, sizeof h + n * sizeof(Item));
}

/* bring this loaded container to the server's state */
void Apply(void* ref, State& s) {
    static Item local[ITEMS_MAX];
    int nl = Snapshot(ref, local);
    int moved = 0;
    for (int i = 0; i < s.count; i++) {                  // what the server has
        s32 have = 0;
        for (int j = 0; j < nl; j++) if (local[j].form == s.items[i].form) have = local[j].n;
        if (have != s.items[i].n) if (void* f = equip::Lookup(s.items[i].form)) { trade::AddItems(ref, f, s.items[i].n - have); moved++; }
    }
    for (int j = 0; j < nl; j++) {                       // what only we have changed
        bool known = false;
        for (int i = 0; i < s.count && !known; i++) known = s.items[i].form == local[j].form;
        if (!known && local[j].n) if (void* f = equip::Lookup(local[j].form)) { trade::AddItems(ref, f, -local[j].n); moved++; }
    }
    s.fresh = false;
    for (int t = 0; t < g_touchedN; t++)                 // what we hold now is the server's: nothing to send back
        if (g_touchedId[t] == s.ref) {
            g_sent[t].count = s.count; memcpy(g_sent[t].items, s.items, s.count * sizeof(Item));
            g_quiet[t] = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds() + 2500;
        }
    if (moved) Debug("world: container %08x brought to the server's state (%d items)", s.ref, moved);
}

/* ---- doors and locks (03.10): open / closed and locked / unlocked are the world's. Not the activation itself
   (a load door would carry the other player away) — the state, through ObjectReference.SetOpen / Lock. */
struct NFn { u32 off; u32 sig[4]; };
constexpr NFn SET_OPEN{0xa562d0, {0xA9BE7BFD, 0xA9014FF4, 0x910003FD, 0xAA0203E0}};
constexpr NFn OPEN_STATE{0xa50b90, {0xAA0203E0, 0x17D9C44B, 0xF9403040, 0xD65F03C0}};
constexpr NFn IS_LOCKED{0xa51c80, {0xA9BF7BFD, 0x910003FD, 0xAA0203E0, 0x97DE753E}};
constexpr NFn LOCK{0xa51d40, {0xA9BD7BFD, 0xA90157F6, 0x910003FD, 0xA9024FF4}};
constexpr u8 FORM_DOOR = 0x1D;
constexpr int DOOR_MAX = 64;
uintptr_t g_setOpen, g_openState, g_isLocked, g_lockFn;
bool g_doorFns;
struct Door { u32 ref; s8 seen; s8 server; };     // bits: 1 open, 2 locked; -1 unknown
Door g_doors[DOOR_MAX];
int g_doorN;

bool DoorFns() {
    if (!g_doorFns) {
        g_doorFns = true;
        g_setOpen = game::Checked177(SET_OPEN.off, SET_OPEN.sig);
        g_openState = game::Checked177(OPEN_STATE.off, OPEN_STATE.sig);
        g_isLocked = game::Checked177(IS_LOCKED.off, IS_LOCKED.sig);
        g_lockFn = game::Checked177(LOCK.off, LOCK.sig);
        Log("двери: %s", g_setOpen && g_openState && g_isLocked && g_lockFn ? "есть" : "нет функций");
    }
    return g_setOpen && g_openState && g_isLocked && g_lockFn;
}

Door* DoorOf(u32 ref, bool add) {
    for (int i = 0; i < g_doorN; i++) if (g_doors[i].ref == ref) return &g_doors[i];
    if (!add) return nullptr;
    Door* d = &g_doors[g_doorN < DOOR_MAX ? g_doorN++ : (int)(ref % DOOR_MAX)];
    *d = Door{ref, -1, -1};
    return d;
}

/* -1 while moving (opening / closing) */
s8 DoorBits(void* ref) {
    s32 st = reinterpret_cast<s32 (*)(void*, u32, void*)>(g_openState)(game::FakeVm(), 0, ref);
    if (st != 1 && st != 3) return -1;
    bool locked = reinterpret_cast<bool (*)(void*, u32, void*)>(g_isLocked)(game::FakeVm(), 0, ref);
    return (s8)((st == 1 ? 1 : 0) | (locked ? 2 : 0));
}

void PumpDoors() {
    if (!DoorFns()) return;
    for (int i = 0; i < g_doorN; i++) {
        Door& d = g_doors[i];
        void* ref = equip::Lookup(d.ref);
        if (!Loaded(ref)) continue;
        s8 now = DoorBits(ref);
        if (now < 0) continue;
        if (d.server >= 0 && now != d.server) {                 /* bring it to the world's state */
            if ((now & 2) != (d.server & 2))
                reinterpret_cast<void (*)(void*, u32, void*, bool, bool)>(g_lockFn)(game::FakeVm(), 0, ref, (d.server & 2) != 0, false);
            if ((now & 1) != (d.server & 1))
                reinterpret_cast<void (*)(void*, u32, void*, bool)>(g_setOpen)(game::FakeVm(), 0, ref, (d.server & 1) != 0);
            d.seen = d.server;
            Debug("world: door %08x set to %d", d.ref, d.server);
            continue;
        }
        if (d.seen >= 0 && now != d.seen) {                      /* we changed it */
            WorldHead h{proto::WK_DOOR, (u16)now, d.ref};
            PushWorld(&h, sizeof h);
            d.server = now;
            Debug("world: door %08x -> %d (to the server)", d.ref, now);
        }
        d.seen = now;
    }
}

/* ---- what NPCs say in a player's dialogue is heard by the players in the same zone (03.10). Read, not hooked, and
   whatever the subtitle settings: SubtitleManager (1.1.177 *GOT 0x21452a8; its update 0x951a74) keeps its entries at
   +0 (count +0xC), an entry = speaker handle +0, text +8; MenuTopicManager (*GOT 0x2147960) +0x99 = in a dialogue,
   +0x60 = the dialogue partner's handle. The first entry spoken by the partner = its line. */
constexpr u32 SUBTITLES_GOT = 0x21452a8, TOPICS_GOT = 0x2147960;
char g_lastSay[200];
/* ---- a dragon soul and a Word of Power are the party's (03.10): a dragon that dies here gives a soul to everybody
   in the zone; a word learned here (Game.TeachWord, word walls) is learned by everybody in the zone. 1.1.177:
   Form.HasKeyword 0xa3d510 (vm, stack, form, keyword); Game.TeachWord 0xa2bdb0 (vm, stack, tag, word);
   Actor.ModActorValue 0xa01770 (vm, stack, actor, BSFixedString* name, value); ActorTypeDragon 00035D59 */
constexpr NFn HAS_KEYWORD{0xa3d510, {0xF9400048, 0xAA0203E0, 0xF9406101, 0xD61F0020}};
constexpr NFn TEACH_WORD{0xa2bdb0, {0xA9BB7BFD, 0xF9000BF9, 0x910003FD, 0xA9025FF8}};
constexpr NFn MOD_AV{0xa01770, {0xD10783FF, 0xFD00D3E8, 0xF900DFFC, 0xA91AFBFD}};
constexpr NFn FS_CTOR_W{0xae6da8, {0xA9BE7BFD, 0xF9000BF3, 0x910003FD, 0xF900001F}};
constexpr u32 KW_DRAGON = 0x35D59;
uintptr_t g_hasKeyword, g_modAv, g_fsCtorW;
volatile bool g_teaching;
int g_souls;                                          // under Lock()
u32 g_words[8]; int g_wordN;                          // under Lock()

HOOK_DEFINE_TRAMPOLINE(TeachWordHook) {
    static void Callback(void* vm, u32 stack, void* tag, void* word) {
        Orig(vm, stack, tag, word);
        if (word && !g_teaching && Status() == NetStatus::Connected) {
            u32 id = Id(word);
            if (id && (id >> 24) != 0xFF) {
                WorldHead h{proto::WK_WORD, 0, id};
                PushWorld(&h, sizeof h);
                Debug("world: word %08x learned here -> the zone", id);
            }
        }
    }
};

struct Heard { u16 from; char text[201]; };
Heard g_heard[4];
int g_heardN;                                        // under Lock()

void Say(const char* raw) {
    char text[200]; int n = 0;
    bool tag = false;
    for (const char* c = raw; *c && n < 190; c++) {                 /* the subtitle's html (speaker colour) off */
        if (*c == '<') { tag = true; continue; }
        if (*c == '>') { tag = false; continue; }
        if (!tag) text[n++] = *c;
    }
    text[n] = 0;
    if (n < 2) return;
    if (!strcmp(text, g_lastSay)) return;                     /* a line is on screen for seconds */
    memcpy(g_lastSay, text, n + 1);
    u8 buf[sizeof(WorldHead) + 200];
    WorldHead h{proto::WK_SAY, (u16)n, 0};
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, text, n);
    PushWorld(buf, sizeof h + n);
    Debug("world: NPC line -> zone: %.60s", text);
}

void PumpSay() {
    auto sm = static_cast<u8*>(game::Singleton(game::ModuleAddr(SUBTITLES_GOT)));
    auto mtm = static_cast<u8*>(game::Singleton(game::ModuleAddr(TOPICS_GOT)));
    if (!sm || !mtm || !game::Readable(mtm, 0xA0) || !game::Readable(sm, 0x10)) return;
    if (!mtm[0x99]) { g_lastSay[0] = 0; return; }
    if (!*reinterpret_cast<u32*>(sm + 0xC)) return;
    u8* e = *reinterpret_cast<u8**>(sm);
    if (!e || !game::Readable(e, 0x10) || *reinterpret_cast<u32*>(e) != *reinterpret_cast<u32*>(mtm + 0x60)) return;
    const char* t = *reinterpret_cast<const char**>(e + 8);
    if (t && game::Readable(t, 1)) Say(t);
}

}  // namespace

void Install() {
    g_hasKeyword = game::Checked177(HAS_KEYWORD.off, HAS_KEYWORD.sig);
    g_modAv = game::Checked177(MOD_AV.off, MOD_AV.sig);
    g_fsCtorW = game::Checked177(FS_CTOR_W.off, FS_CTOR_W.sig);
    uintptr_t teach = game::Checked177(TEACH_WORD.off, TEACH_WORD.sig);
    if (teach) TeachWordHook::InstallAtPtr(teach);
    Log("души и слова на отряд: %s", g_hasKeyword && g_modAv && g_fsCtorW && teach ? "есть" : "нет функций");
}

/* the race: a pointer to a RACE form (type 0x0E) in the NPC base, its offset found once by looking (the Papyrus
   GetRace address from the scan was wrong: it crashed, 03.10) */
void* RaceOf(void* actor) {
    static int raceOff;
    void* race = nullptr;
    auto base = actor ? *reinterpret_cast<u8**>(static_cast<u8*>(actor) + 0x40) : nullptr;
    if (base && game::Readable(base, 0x200)) {
        auto isRace = [](void* p) { return p && game::Readable(p, 0x20) && static_cast<u8*>(p)[0x1A] == 0x0E; };
        if (raceOff > 0 && isRace(*reinterpret_cast<void**>(base + raceOff))) race = *reinterpret_cast<void**>(base + raceOff);
        for (int o = 0x40; !race && o < 0x200; o += 8) {
            void* p = *reinterpret_cast<void**>(base + o);
            if (isRace(p)) { race = p; raceOff = o; Log("world: race in NPC at +%x", o); }
        }
    }
    return race;
}

void DragonDied(void* actor) {
    Debug("world: DragonDied %08x", actor ? Id(actor) : 0u);
    if (!actor || !g_hasKeyword || Status() != NetStatus::Connected) return;
    /* a placed dragon dies on every console (shared kills) and each absorbs it the vanilla way; only a dragon the game
       made at runtime (random attacks: FormID FF..) exists here alone — its soul goes to the zone */
    if ((Id(actor) >> 24) != 0xFF) return;
    void* kw = equip::Lookup(KW_DRAGON);
    /* ActorTypeDragon is a keyword of the dragon's race, not of its NPC record */
    void* race = RaceOf(actor);
    /* a dragon with a soul: DragonRace, or Dragonborn's black dragons (undead and spectral dragons give none; the
       Form.HasKeyword address from the scan did not answer for the race, 03.10) */
    static int db = -2;
    if (db == -2) db = game::PluginIndex("Dragonborn.esm");
    u32 rid = race ? Id(race) : 0;
    bool dragon = rid == 0x00012E82 ||
                  (db >= 0 && (rid == (((u32)db << 24) | 0x02C88C) || rid == (((u32)db << 24) | 0x02C88B)));
    (void)kw;
    Debug("world: died %08x race %08x dragon %d", Id(actor), race ? Id(race) : 0u, dragon);
    if (!dragon) return;
    WorldHead h{proto::WK_SOUL, 0, Id(actor)};
    PushWorld(&h, sizeof h);
    Log("дракон убит рядом — душа всему отряду в зоне");
}

/* (08.10, Skyrim Together's ActivateRequest) an activator (ACTI: levers, pull chains, buttons, pressure plates) the
   player uses is used on the other consoles too — by that player's puppet, so it opens their gate / drops their bridge */
constexpr u8 FORM_ACTI = 0x18;
struct Act { u32 ref; u16 from; };
Act g_actIn[16];
volatile u32 g_actHead, g_actTail;

void Activated(void* ref) {
    if (!ref || !quests::IntroDone() || Status() != NetStatus::Connected) return;
    void* base = *reinterpret_cast<void**>(static_cast<u8*>(ref) + 0x40);
    u32 id = Id(ref);
    if (!base || static_cast<u8*>(base)[0x1A] != FORM_ACTI || !id || (id >> 24) == 0xFF) return;
    WorldHead h{proto::WK_ACTIVATE, 0, id};
    PushWorld(&h, sizeof h);
    Debug("world: активатор %08x — всем", id);
}

void PumpActivations() {
    while (g_actTail != __atomic_load_n(&g_actHead, __ATOMIC_ACQUIRE)) {
        Act a = g_actIn[g_actTail % 16];
        __atomic_store_n(&g_actTail, g_actTail + 1, __ATOMIC_RELEASE);
        void* ref = equip::Lookup(a.ref);
        void* by = game::PuppetRef(a.from);
        if (!ref || !Loaded(ref) || !by) { Debug("world: активатор %08x от %u — не здесь", a.ref, a.from); continue; }
        trade::ActivateAs(ref, by);
        Log("world: активатор %08x использован игроком %u", a.ref, a.from);
    }
}

void Touched(void* ref) {
    if (!quests::IntroDone()) return;                    // (07.10) our intro is ours: its chests and doors stay out of the world
    if (ref && Status() == NetStatus::Connected) {
        void* base = *reinterpret_cast<void**>(static_cast<u8*>(ref) + 0x40);
        u32 id = Id(ref);
        if (base && *reinterpret_cast<u8*>(static_cast<u8*>(base) + 0x1A) == FORM_DOOR && id && (id >> 24) != 0xFF) {
            DoorOf(id, true);
            return;
        }
    }
    if (!ref || Status() != NetStatus::Connected) return;
    u32 id = Id(ref);
    if (!id || (id >> 24) == 0xFF) return;
    if (*reinterpret_cast<u8*>(static_cast<u8*>(ref) + 0x1A) == 0x3E) {          /* an actor: a vendor? its chest too */
        void* base = *reinterpret_cast<void**>(static_cast<u8*>(ref) + 0x40);
        u32 chestId = base ? VendorChestOf(Id(base)) : 0;
        void* chest = chestId ? equip::Lookup(chestId) : nullptr;
        if (chest && !IsVendorChest(chestId)) {
            g_vendorChest[g_vendorChestN < 16 ? g_vendorChestN++ : (int)(chestId % 16)] = chestId;
            Debug("world: vendor %08x — its chest %08x is the world's", id, chestId);
        }
        if (chest) Touched(chest);
    }
    /* a world boss's or mini-boss's body (SwitchPatch 0x1240..0x12A7): the loot on it is each player's own, not the world's */
    static int patch = -2;
    if (patch == -2) patch = game::PluginIndex("SwitchPatch.esp");
    if (patch >= 0 && (id >> 24) == (u32)patch && (id & 0xFFFFFF) >= 0x1240 && (id & 0xFFFFFF) < 0x12A8) return;
    for (int i = 0; i < g_touchedN; i++) if (g_touchedId[i] == id) return;
    int k = g_touchedN < TOUCH_MAX ? g_touchedN++ : (int)(id % TOUCH_MAX);
    g_touched[k] = ref;
    g_touchedId[k] = id;
    g_sent[k].ref = id;
    g_sent[k].count = (u16)Snapshot(ref, g_sent[k].items);       // the state when opened: not news yet
    Debug("world: watching container %08x", id);
}

void Received(const u8* p, u32 size) {
    if (size < sizeof(WorldHead)) return;
    WorldHead h; memcpy(&h, p, sizeof h);
    if (h.kind == proto::WK_DOOR) {
        Lock();
        if (Door* d = DoorOf(h.ref, true)) d->server = (s8)(h.count & 3);
        Unlock();
        return;
    }
    if (h.kind == proto::WK_MARK) { party::MarkReceived(p, size); return; }
    if (h.kind == proto::WK_ACTIVATE) {                  // network thread: one producer
        u32 hd = g_actHead;
        if (hd - g_actTail < 16) { g_actIn[hd % 16] = Act{h.ref, h.count}; __atomic_store_n(&g_actHead, hd + 1, __ATOMIC_RELEASE); }
        return;
    }
    if (h.kind == proto::WK_ITEM) { items::Received(p, size); return; }
    if (h.kind == proto::WK_SOUL || h.kind == proto::WK_WORD) {
        Lock();
        if (h.kind == proto::WK_SOUL) g_souls++;
        else if (g_wordN < 8) g_words[g_wordN++] = h.ref;
        Unlock();
        return;
    }
    if (h.kind == proto::WK_SAY) {
        if (h.count == 0 || h.count > 200 || sizeof h + h.count > size) return;
        Lock();
        if (g_heardN < 4) {
            Heard& e = g_heard[g_heardN++];
            e.from = (u16)h.ref;
            memcpy(e.text, p + sizeof h, h.count);
            e.text[h.count] = 0;
        }
        Unlock();
        return;
    }
    if (h.kind != proto::WK_CONTAINER || h.count > ITEMS_MAX || sizeof h + h.count * sizeof(Item) > size) return;
    Lock();
    int k = -1;
    for (int i = 0; i < g_serverN; i++) if (g_server[i].ref == h.ref) k = i;
    if (k < 0) k = g_serverN < STATE_MAX ? g_serverN++ : (int)(h.ref % STATE_MAX);
    g_server[k].ref = h.ref;
    g_server[k].count = h.count;
    memcpy(g_server[k].items, p + sizeof h, h.count * sizeof(Item));
    g_server[k].fresh = true;
    Unlock();
}

void Pump() {
    if (Status() != NetStatus::Connected || !game::Player()) return;
    if (!quests::IntroDone()) return;                    // (07.10) the world's states wait until we are out of Helgen
    static u64 at;
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    if (now - at < 700) return;
    at = now;
    /* the server's states: applied once they arrive and again whenever the container is loaded anew */
    Lock();
    for (int i = 0; i < g_serverN; i++) {
        State& s = g_server[i];
        void* ref = equip::Lookup(s.ref);
        static void* lastLoaded[STATE_MAX];
        bool loaded = Loaded(ref);
        if (loaded && (s.fresh || lastLoaded[i] != ref)) Apply(ref, s);
        lastLoaded[i] = loaded ? ref : nullptr;
    }
    PumpDoors();
    PumpActivations();
    PumpSay();
    int souls = g_souls; g_souls = 0;
    u32 words[8]; int nw = g_wordN; memcpy(words, g_words, sizeof words); g_wordN = 0;
    Heard heard[4];
    int nh = g_heardN;
    memcpy(heard, g_heard, sizeof heard);
    g_heardN = 0;
    Unlock();
    for (int i = 0; i < souls && g_modAv && g_fsCtorW; i++) {   /* a dragon died by a friend's hand next to us */
        struct FS { const char* data; };
        static FS fs;
        if (!fs.data) reinterpret_cast<void (*)(FS*, const char*)>(g_fsCtorW)(&fs, "DragonSouls");
        reinterpret_cast<void (*)(void*, u32, void*, FS*, float)>(g_modAv)(game::FakeVm(), 0, game::Player(), &fs, 1.0f);
        game::Notify("<font color='#FFD24A' size='26'>Душа дракона поглощена — вместе с отрядом</font>");
    }
    for (int i = 0; i < nw; i++) {                               /* a word a friend learned next to us */
        void* word = equip::Lookup(words[i]);
        if (!word) continue;
        g_teaching = true;
        TeachWordHook::Orig(game::FakeVm(), 0, nullptr, word);
        g_teaching = false;
        game::Notify("<font color='#9AD0FF' size='26'>Слово силы выучено — вместе с отрядом</font>");
    }
    for (int i = 0; i < nh; i++) {                               /* what an NPC told another player near here */
        char who[32] = "", msg[300];
        if (!PlayerName(heard[i].from, who, sizeof who)) snprintf(who, sizeof who, "игрок %u", heard[i].from);
        for (char* c = heard[i].text; *c; c++) if (*c == '<' || *c == '>') *c = ' ';
        snprintf(msg, sizeof msg, "<font color='#BEBEBE'>[%s] %s</font>", who, heard[i].text);
        game::Notify(msg);
    }
    /* our changes */
    static Item now_[ITEMS_MAX];
    for (int t = 0; t < g_touchedN; t++) {
        void* ref = equip::Lookup(g_touchedId[t]);
        if (!Loaded(ref) || now < g_quiet[t]) continue;
        int n = Snapshot(ref, now_);
        if (Same(now_, n, g_sent[t].items, g_sent[t].count)) continue;
        g_sent[t].count = (u16)n;
        memcpy(g_sent[t].items, now_, n * sizeof(Item));
        Send(g_touchedId[t], now_, n);
        Debug("world: container %08x changed here (%d entries) -> server", g_touchedId[t], n);
    }
}

}  // namespace coop::world
