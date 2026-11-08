#include "pvp.hpp"
#include "client.hpp"
#include "game.hpp"
#include <nn/os.hpp>
#include <cstdio>

/* Players hitting each other. A puppet is essential (appearance.cpp) and your teammate (it does not turn on you
   when hit by accident). Damage it takes here is measured from its health, given back to it at once, and — in
   PvP rooms (the server decides) — dealt to its player on their console.
   1.1.177 Papyrus natives (vm, stack, actor, BSFixedString* av [, float]): GetActorValue 0x9fe0f0,
   DamageActorValue 0x9fd2c0, RestoreActorValue 0xa02820 (the VM only for an unknown value name);
   SetPlayerTeammate native -> 0x63a854(actor, teammate, canDoFavor). */
namespace coop::pvp {
namespace {

struct Fn { u32 off; u32 sig[4]; };
constexpr Fn GET_AV{0x9fe0f0, {0xD10783FF, 0xA91A7BFD, 0x910683FD, 0xF900DBFC}};
constexpr Fn DAMAGE_AV{0x9fd2c0, {0xD107C3FF, 0xFD00D3E8, 0xA91B7BFD, 0x9106C3FD}};
constexpr Fn RESTORE_AV{0xa02820, {0xD10783FF, 0xFD00D3E8, 0xF900DFFC, 0xA91AFBFD}};
constexpr Fn TEAMMATE{0x63a854, {0xA9BE7BFD, 0xA9014FF4, 0x910003FD, 0xD000D848}};
constexpr Fn FS_CTOR{0xae6da8, {0xA9BE7BFD, 0xF9000BF3, 0x910003FD, 0xF900001F}};
constexpr u32 FS_DTOR = 0xae6ab0;

uintptr_t g_get, g_damage, g_restore, g_teammate, g_fsCtor, g_fsDtor;
bool g_ok;
struct FixedString { const char* data; };
FixedString g_health{};

struct Seen { u16 id; void* ref; float full; u64 quietUntil; };
Seen g_seen[MAX_REMOTE];

struct Hit { u16 from; float amount; };
Hit g_in[16];
volatile int g_inHead, g_inTail;

float Get(void* actor) { return reinterpret_cast<float (*)(void*, u32, void*, FixedString*)>(g_get)(game::FakeVm(), 0, actor, &g_health); }
void Damage(void* actor, float v) { reinterpret_cast<void (*)(void*, u32, void*, FixedString*, float)>(g_damage)(game::FakeVm(), 0, actor, &g_health, v); }
void Restore(void* actor, float v) { reinterpret_cast<void (*)(void*, u32, void*, FixedString*, float)>(g_restore)(game::FakeVm(), 0, actor, &g_health, v); }

}  // namespace

bool Resolve() {
    g_get = game::Checked177(GET_AV.off, GET_AV.sig);
    g_damage = game::Checked177(DAMAGE_AV.off, DAMAGE_AV.sig);
    g_restore = game::Checked177(RESTORE_AV.off, RESTORE_AV.sig);
    g_fsCtor = game::Checked177(FS_CTOR.off, FS_CTOR.sig);
    g_teammate = game::Checked177(TEAMMATE.off, TEAMMATE.sig);
    g_ok = g_get && g_damage && g_restore && g_fsCtor && g_teammate;
    if (g_ok) g_fsDtor = game::ModuleAddr(FS_DTOR);
    /* the name is made later, in the world: at boot the game's string pool does not exist yet (it crashed) */
    return g_ok;
}

void HealPlayer() {
    void* player = game::Player();
    if (!g_ok || !player) return;
    if (!g_health.data) reinterpret_cast<void (*)(FixedString*, const char*)>(g_fsCtor)(&g_health, "Health");
    Restore(player, 100000.0f);
}

void Received(u16 fromId, float amount) {
    int next = (g_inHead + 1) % 16;
    if (next == g_inTail) return;
    g_in[g_inHead] = Hit{fromId, amount};
    g_inHead = next;
}

void Pump() {
    if (!g_ok) return;
    if (!g_health.data) reinterpret_cast<void (*)(FixedString*, const char*)>(g_fsCtor)(&g_health, "Health");   // kept for good
    void* player = game::Player();
    while (g_inTail != g_inHead && player) {             // hits from other players
        Hit h = g_in[g_inTail];
        g_inTail = (g_inTail + 1) % 16;
        Damage(player, h.amount);
        char who[32], msg[80];
        if (!PlayerName(h.from, who, sizeof who)) snprintf(who, sizeof who, "Игрок %u", h.from);
        snprintf(msg, sizeof msg, "%s: -%d здоровья", who, (int)(h.amount + 0.5f));
        game::Notify(msg);
    }
    static u64 next;
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    if (now < next) return;
    next = now + 150;
    RemotePlayer rs[MAX_REMOTE];
    int n = SnapshotRemotes(rs, MAX_REMOTE);
    for (int i = 0; i < n; i++) {
        u16 id = rs[i].state.id;
        void* ref = game::PuppetRef(id);
        if (!ref) continue;
        Seen* s = nullptr;
        for (auto& x : g_seen) if (x.id == id) s = &x;
        for (auto& x : g_seen) if (!s && !x.id) s = &x;
        if (!s) continue;
        float hp = Get(ref);
        if (s->id != id || s->ref != ref) {                // new puppet: its full health, and it is on your side
            *s = Seen{id, ref, hp, 0};
            reinterpret_cast<void (*)(void*, bool, bool)>(g_teammate)(ref, true, false);
            continue;
        }
        if (hp > s->full) s->full = hp;
        if (now < s->quietUntil) continue;                 // the restore of the last hit has not landed yet
        float d = s->full - hp;
        if (d < 1.0f) continue;
        Restore(ref, d);                                   // the puppet itself never goes down
        s->quietUntil = now + 500;
        PushHit(id, d);                                    // the server drops it in PvE rooms
        Debug("hit player %u for %.0f", id, d);
    }
}

}  // namespace coop::pvp
