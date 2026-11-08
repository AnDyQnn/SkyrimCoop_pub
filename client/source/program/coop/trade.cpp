#include "trade.hpp"
#include "death.hpp"
#include "followers.hpp"
#include "items.hpp"
#include "world.hpp"
#include "client.hpp"
#include "equip.hpp"
#include "game.hpp"
#include "lib.hpp"
#include <nn/os.hpp>
#include <cstdio>
#include <cstring>

namespace coop::trade {
namespace {

struct Fn { u32 off; u32 sig[4]; };
/* 1.1.177:
     Actor.OpenInventory native -> 0x8d3528(ref, 3): ContainerMenu in "exchange" mode (the follower one)
     ObjectReference.AddItem native 0xa4e540(vm, stack, ref, form, count, silent): queues a task (fake VM)
     GetInventoryChanges 0x3521c(ref + 0x70) -> list {entry, next}; entry {object, extraLists, s32 countDelta +0x10} */
constexpr Fn OPEN_CONTAINER{0x8d3528, {0xD10183FF, 0xA9017BFD, 0x910043FD, 0xA90267FA}};
constexpr Fn ADD_ITEM{0xa4e540, {0xD10183FF, 0xA9017BFD, 0x910043FD, 0xA90267FA}};
constexpr Fn GET_CHANGES{0x3521c, {0xA9BD7BFD, 0xA90157F6, 0x910003FD, 0xA9024FF4}};
constexpr Fn REMOVE_ITEM{0xa54bb0, {0xD101C3FF, 0xA9017BFD, 0x910043FD, 0xF90013FB}};

uintptr_t g_open, g_add, g_changes, g_remove;
bool g_ok;

template <class T> T At(const void* p, u32 off) { return *reinterpret_cast<const T*>(static_cast<const u8*>(p) + off); }

constexpr int MAX = 1024;          // a well-played character carries 200+ kinds

int Snapshot(void* ref, Count* out) {
    void* ch = reinterpret_cast<void* (*)(void*)>(g_changes)(static_cast<u8*>(ref) + 0x70);
    int n = 0;
    for (auto node = ch ? At<const u8*>(ch, 0) : nullptr; node && n < MAX; node = At<const u8*>(node, 8)) {
        const u8* e = At<const u8*>(node, 0);
        if (!e || !At<const void*>(e, 0)) continue;
        out[n++] = Count{At<u32>(At<const void*>(e, 0), 0x14), At<s32>(e, 0x10)};
    }
    return n;
}

s32 CountOf(const Count* c, int n, u32 id) {
    for (int i = 0; i < n; i++) if (c[i].id == id) return c[i].n;
    return 0;
}

volatile u16 g_want;            // open the exchange with this player
u16 g_with;                     // exchange in progress with ...
u64 g_until;                    // ... until (ms): the menu is watched this long
Count g_before[MAX];
int g_beforeN;

struct Gift { u16 from; u32 id; s32 n; };
Gift g_in[32];
volatile int g_inHead, g_inTail;

void AddTo(void* ref, void* form, s32 n, bool silent = false) {
    if (n > 0) reinterpret_cast<void (*)(void*, u32, void*, void*, s32, bool)>(g_add)(game::FakeVm(), 0, ref, form, n, silent);
    else if (n < 0) reinterpret_cast<void (*)(void*, u32, void*, void*, s32, bool, void*)>(g_remove)(game::FakeVm(), 0, ref, form, -n, true, nullptr);
}

/* Activating another player (the "Говорить" prompt over them) opens the exchange instead of a conversation:
   TESObjectREFR::ActivateRef (1.1.177: 0x1deeb8, ref, activator, arg, object, count, defaultOnly). */
constexpr Fn ACTIVATE{0x1deeb8, {0xD102C3FF, 0xA9057BFD, 0x910143FD, 0xA9066FFC}};
HOOK_DEFINE_TRAMPOLINE(ActivateHook) {
    static bool Callback(void* ref, void* activator, u8 arg, void* object, s32 count, bool defaultOnly) {
        if (activator && activator == game::Player()) {
            if (u16 id = game::PuppetOwner(ref)) {
                if (death::ReviveFriend(id)) return true;          // (07.10) knocked down: A gets them up
                Start(id);
                return true;
            }
            /* somebody else's horse (03.10): it is theirs, it does not carry another rider */
            auto r = static_cast<u8*>(ref);
            if (r && r[0x1A] == 0x3E) {
                void* race = world::RaceOf(ref);
                Debug("activate actor %08x race %08x", *reinterpret_cast<u32*>(r + 0x14), race ? *reinterpret_cast<u32*>(static_cast<u8*>(race) + 0x14) : 0u);
                if (race && *reinterpret_cast<u32*>(static_cast<u8*>(race) + 0x14) == 0x000131FD &&
                    followers::OwnedByOther(*reinterpret_cast<u32*>(r + 0x14))) {
                    game::Notify("Это лошадь другого игрока");
                    return true;
                }
            }
            items::PickedUp(ref);                      // a thing on the ground of the party's: gone for everybody
            world::Touched(ref);                       // a container / corpse: its contents are the world's (world.cpp)
            world::Activated(ref);                     // (08.10) a lever / chain / button: used for the others too
        }
        return Orig(ref, activator, arg, object, count, defaultOnly);
    }
};

uintptr_t g_activate;

}  // namespace

void AddItems(void* ref, void* form, s32 n, bool silent) { if (g_ok && ref && form) AddTo(ref, form, n, silent); }
int Changes(void* ref, Count* out, int max) {
    if (!g_ok || !ref) return 0;
    static Count buf[MAX];
    int n = Snapshot(ref, buf);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = buf[i];
    return n;
}
void ActivateAs(void* ref, void* activator) {
    if (g_activate && ref && activator) reinterpret_cast<bool (*)(void*, void*, u8, void*, s32, bool)>(g_activate)(ref, activator, 0, nullptr, 1, false);
}

void ActivateAsPlayer(void* ref) {
    if (g_activate && ref) reinterpret_cast<bool (*)(void*, void*, u8, void*, s32, bool)>(g_activate)(ref, game::Player(), 0, nullptr, 1, false);
}

bool Resolve() {
    g_open = game::Checked177(OPEN_CONTAINER.off, OPEN_CONTAINER.sig);
    g_add = game::Checked177(ADD_ITEM.off, ADD_ITEM.sig);
    g_changes = game::Checked177(GET_CHANGES.off, GET_CHANGES.sig);
    g_remove = game::Checked177(REMOVE_ITEM.off, REMOVE_ITEM.sig);
    g_ok = g_open && g_add && g_changes && g_remove;
#ifndef SSW_OFFLINE      // activating another player's puppet: multiplayer only
    if (g_ok)
        if (uintptr_t a = game::Checked177(ACTIVATE.off, ACTIVATE.sig)) { g_activate = a; ActivateHook::InstallAtPtr(a); }
#else
    g_activate = game::Checked177(ACTIVATE.off, ACTIVATE.sig);   // offline: ActivateAsPlayer only (the scroll's page)
#endif
    return g_ok;
}

bool Available() { return g_ok; }

void Start(u16 playerId) { g_want = playerId; }

volatile u32 g_testForm;
volatile s32 g_testCount;
void TestPut(u32 formId, s32 count) { g_testCount = count; g_testForm = formId; }

void Received(u16 fromId, u32 formId, s32 count) {
    int next = (g_inHead + 1) % 32;
    if (next == g_inTail) return;
    g_in[g_inHead] = Gift{fromId, formId, count};
    g_inHead = next;
}

void Pump() {
    if (!g_ok) return;
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    void* player = game::Player();
    /* gifts from the others */
    while (g_inTail != g_inHead && player) {
        Gift g = g_in[g_inTail];
        g_inTail = (g_inTail + 1) % 32;
        void* form = equip::Lookup(g.id);
        if (!form) continue;
        AddTo(player, form, g.n);
        char who[32], msg[96];
        if (!PlayerName(g.from, who, sizeof who)) snprintf(who, sizeof who, "Игрок %u", g.from);
        snprintf(msg, sizeof msg, g.n > 0 ? "Кооп: %s передал тебе предметы" : "Кооп: %s забрал предметы", who);
        game::Notify(msg);
    }
    /* open */
    if (u16 id = __atomic_exchange_n(&g_want, (u16)0, __ATOMIC_ACQ_REL)) {
        void* puppet = game::PuppetRef(id);
        if (!puppet) { game::Notify("Кооп: игрок далеко — подойди к нему для обмена"); return; }
        g_with = id;
        g_until = now + 180000;
        g_beforeN = Snapshot(puppet, g_before);
        Debug("trade with %u: puppet holds %d kinds, gold delta %d", id, g_beforeN, CountOf(g_before, g_beforeN, 0xF));
        reinterpret_cast<void (*)(void*, u32)>(g_open)(puppet, 3);
        return;
    }
    if (u32 f = __atomic_exchange_n(&g_testForm, 0u, __ATOMIC_ACQ_REL)) {
        void* puppet = g_with ? game::PuppetRef(g_with) : nullptr;
        if (puppet && equip::Lookup(f)) AddTo(puppet, equip::Lookup(f), g_testCount);
    }
    /* watch: what moved between you and the puppet goes to its player / comes back from you */
    if (!g_with) return;
    void* puppet = game::PuppetRef(g_with);
    if (!puppet || now > g_until) { g_with = 0; return; }
    static u64 next;
    if (now < next) return;
    next = now + 500;
    static Count cur[MAX];
    int n = Snapshot(puppet, cur);
    bool moved = false;
    static int lastN = -1, lastGold = -1;
    if (n != lastN || CountOf(cur, n, 0xF) != lastGold) {
        lastN = n; lastGold = CountOf(cur, n, 0xF);
        Debug("trade watch: %d kinds, gold delta %d", n, lastGold);
    }
    for (int i = 0; i < n; i++) {                      // put into the puppet: send to them
        s32 d = cur[i].n - CountOf(g_before, g_beforeN, cur[i].id);
        if (d <= 0) continue;
        void* form = equip::Lookup(cur[i].id);
        if (!form) continue;
        PushGive(g_with, cur[i].id, d);
        AddTo(puppet, form, -d);                       // the puppet keeps only what the player wears
        moved = true;
        Debug("gave %d x %08x to player %u", d, cur[i].id, g_with);
    }
    for (int i = 0; i < g_beforeN; i++) {              // taken out of the puppet: not yours, put it back
        s32 d = g_before[i].n - CountOf(cur, n, g_before[i].id);
        if (d <= 0) continue;
        void* form = equip::Lookup(g_before[i].id);
        if (!form || !player) continue;
        AddTo(player, form, -d);
        AddTo(puppet, form, d);
        moved = true;
        game::Notify("Кооп: вещи другого игрока взять нельзя — он может передать их сам");
    }
    if (moved) next = now + 2000;                     // the queued add / remove tasks run first
}

}  // namespace coop::trade
