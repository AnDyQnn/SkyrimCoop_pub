#include "npcsync.hpp"
#include "client.hpp"
#include "quests.hpp"
#include "equip.hpp"
#include "game.hpp"
#include <nn/os.hpp>
#include <cstring>
#include "lib.hpp"

/* Shared enemies (and any NPC from the plugins): the damage one player deals to an actor is dealt to the same
   actor on the other consoles, so everybody fights the same wolf and sees its health go down; the kill itself
   is shared by the KILL message (game.cpp). Actors worth watching are the ones that act (every attack, block,
   draw goes through ActorMediator::PerformAction — actions.cpp tells us about them).
   Health: the Papyrus natives GetActorValue / DamageActorValue with the no-op VM, like pvp.cpp. */
namespace coop::npcsync {
namespace {

struct Fn { u32 off; u32 sig[4]; };
constexpr Fn GET_AV{0x9fe0f0, {0xD10783FF, 0xA91A7BFD, 0x910683FD, 0xF900DBFC}};
constexpr Fn DAMAGE_AV{0x9fd2c0, {0xD107C3FF, 0xFD00D3E8, 0xA91B7BFD, 0x9106C3FD}};
constexpr Fn FS_CTOR{0xae6da8, {0xA9BE7BFD, 0xF9000BF3, 0x910003FD, 0xF900001F}};

uintptr_t g_get, g_damage, g_fsCtor;
bool g_ok;
struct FixedString { const char* data; };
FixedString g_health{};

constexpr int MAX = 32;
struct Watched { u32 id; float last; u64 seenMs; };
Watched g_w[MAX];
volatile u32 g_new[16];               // from the action hook (another thread): FormIDs to start watching
volatile int g_newHead;

struct Dmg { u32 id; float amount; };
Dmg g_in[32];
volatile int g_inHead, g_inTail;

float Health(void* a) { return reinterpret_cast<float (*)(void*, u32, void*, FixedString*)>(g_get)(game::FakeVm(), 0, a, &g_health); }
void Damage(void* a, float v) {
    reinterpret_cast<void (*)(void*, u32, void*, FixedString*, float)>(g_damage)(game::FakeVm(), 0, a, &g_health, v);
}

void* Actor(u32 id) {
    auto a = static_cast<u8*>(equip::Lookup(id));
    if (!a || a[0x1A] != 0x3E || !*reinterpret_cast<void**>(a + 0x68)) return nullptr;   // loaded ACHR only
    return a;
}

}  // namespace

bool Resolve() {
    g_get = game::Checked177(GET_AV.off, GET_AV.sig);
    g_damage = game::Checked177(DAMAGE_AV.off, DAMAGE_AV.sig);
    g_fsCtor = game::Checked177(FS_CTOR.off, FS_CTOR.sig);
    g_ok = g_get && g_damage && g_fsCtor;
    return g_ok;
}

void Noticed(u32 formId) {
    if ((formId >> 24) == 0xFF || !formId) return;
    int k = __atomic_fetch_add(&g_newHead, 1, __ATOMIC_ACQ_REL) % 16;
    g_new[k] = formId;
}

void Received(u32 formId, float amount) {
    int next = (g_inHead + 1) % 32;
    if (next == g_inTail) return;
    g_in[g_inHead] = Dmg{formId, amount};
    g_inHead = next;
}

volatile u32 g_testId;
volatile float g_testAmount;
void TestDamage(u32 formId, float amount) { g_testAmount = amount; g_testId = formId; }

/* (08.10) the actor core does the sharing now; the console's "damage" test aid stays (the player too) */
void PumpTest() {
    if (!g_ok) return;
    if (!g_health.data) reinterpret_cast<void (*)(FixedString*, const char*)>(g_fsCtor)(&g_health, "Health");
    if (u32 t = __atomic_exchange_n(&g_testId, 0u, __ATOMIC_ACQ_REL)) {
        void* a = t == 0x14 ? game::Player() : Actor(t);
        if (a) { Damage(a, g_testAmount); Log("тест: урон %08x %.0f -> здоровье %.0f", t, g_testAmount, Health(a)); }
    }
}

void Pump() {
    if (!g_ok || Status() != NetStatus::Connected) return;
    if (!quests::IntroDone()) return;                  // (07.10) damage in our intro is ours alone
    if (!g_health.data) reinterpret_cast<void (*)(FixedString*, const char*)>(g_fsCtor)(&g_health, "Health");
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    void* player = game::Player();
    /* new actors to watch */
    for (int k = 0; k < 16; k++) {
        u32 id = __atomic_exchange_n(&g_new[k], 0u, __ATOMIC_ACQ_REL);
        if (!id || (player && id == *reinterpret_cast<u32*>(static_cast<u8*>(player) + 0x14))) continue;
        Watched* w = nullptr;
        for (auto& x : g_w) if (x.id == id) w = &x;
        if (w) { w->seenMs = now; continue; }
        for (auto& x : g_w) if (!w && (!x.id || now - x.seenMs > 60000)) w = &x;
        void* a = w ? Actor(id) : nullptr;
        if (a) *w = Watched{id, Health(a), now};
    }
    /* damage from the other players first (so it is not taken for ours) */
    while (g_inTail != g_inHead) {
        Dmg d = g_in[g_inTail];
        g_inTail = (g_inTail + 1) % 32;
        void* a = Actor(d.id);
        if (!a || d.amount <= 0 || d.amount > 100000) continue;
        float before = Health(a);
        Damage(a, d.amount);
        float after = Health(a);
        for (auto& x : g_w) if (x.id == d.id) x.last = after;
        Debug("npc %08x hit by another player: %.0f -> %.0f", d.id, before, after);
    }
    if (u32 t = __atomic_exchange_n(&g_testId, 0u, __ATOMIC_ACQ_REL)) {
        if (void* a = Actor(t)) {
            Watched* w = nullptr;
            for (auto& x : g_w) if (x.id == t) w = &x;
            for (auto& x : g_w) if (!w && !x.id) w = &x;
            if (w && w->id != t) *w = Watched{t, Health(a), now};
            Damage(a, g_testAmount);
        }
    }
    /* our damage out, 5 times a second */
    static u64 next;
    if (now < next) return;
    next = now + 200;
    for (auto& w : g_w) {
        if (!w.id) continue;
        void* a = Actor(w.id);
        if (!a) { if (now - w.seenMs > 60000) w = Watched{}; continue; }
        float h = Health(a);
        if (h < w.last - 0.5f) { PushDamage(w.id, w.last - h); Debug("npc %08x hurt here: %.0f -> %.0f", w.id, w.last, h); }
        w.last = h;
    }
}

}  // namespace coop::npcsync
