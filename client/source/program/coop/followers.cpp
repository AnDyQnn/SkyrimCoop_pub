#include "followers.hpp"
#include "client.hpp"
#include "equip.hpp"
#include "game.hpp"
#include "world.hpp"
#include "lib.hpp"
#include <nn/os.hpp>
#include <cstdio>
#include <cstring>
#include <cmath>

/* Shared followers. Lydia & co. exist on every console (same FormID, from the game's plugins); the server keeps
   who each one follows. On the leader's console she follows its player, on the others she follows that
   player's puppet — so everybody sees her next to the same person, and any player can tell her whom to follow.
   Your followers are noticed through SetPlayerTeammate (every recruited follower goes through it).
   1.1.177: SetPlayerTeammate core 0x63a854(actor, teammate, canDoFavor);
            Actor.KeepOffsetFromActor native 0xa01290(vm, stack, actor, target, s0..s2 offset, s3..s5 angle,
            s6 catch-up radius, s7 follow radius), Actor.ClearKeepOffsetFromActor native 0x9fd248 (no VM use);
            TESFullName in TESNPC at +0xC8 (secondary vtable with its slots at top -200), name +0xD0. */
namespace coop::followers {
namespace {

struct Fn { u32 off; u32 sig[4]; };
constexpr Fn TEAMMATE{0x63a854, {0xA9BE7BFD, 0xA9014FF4, 0x910003FD, 0xD000D848}};
constexpr Fn KEEP_OFFSET{0xa01290, {0xD10143FF, 0x6D0223E9, 0xF90023F3, 0xA9037BFD}};
constexpr Fn CLEAR_OFFSET{0x9fd248, {0xAA0203E0, 0x17F154C5, 0xD100C3FF, 0xA9017BFD}};
constexpr Fn MOVE_TO{0xa51e00, {0xD10203FF, 0xFD0013EA, 0xF90027F9, 0x6D02A3E9}};
constexpr Fn MOVE_HOME{0xa52410, {0xD10143FF, 0xA9017BFD, 0x910043FD, 0xF90013F7}};   // ObjectReference.MoveToMyEditorLocation
constexpr Fn IS_TEAMMATE{0xa01190, {0x39446C48, 0x53020900, 0xD65F03C0, 0xAA0203E0}};   // Actor.IsPlayerTeammate
constexpr u16 HOME = 0xFFFF;      // from the server: its owner is offline, it waits at home

uintptr_t g_teammate, g_keep, g_clear, g_moveTo, g_moveHome, g_isTeammate;
volatile bool g_applying;          // our own SetPlayerTeammate calls (not the player's recruiting)
bool g_ok;

constexpr int MAX = 16;
u32 g_mine[MAX];                  // FormIDs of your followers (teammates) seen this session
int g_mineN;
nn::os::MutexType g_lock;
bool g_lockReady;
void Lock() { if (!g_lockReady) { nn::os::InitializeMutex(&g_lock, false, 0); g_lockReady = true; } nn::os::LockMutex(&g_lock); }
void Unlock() { nn::os::UnlockMutex(&g_lock); }

struct Assign { u32 formId; u16 leader; bool applied; u16 appliedLeader; };
Assign g_assign[MAX];             // from the server: who follows whom
int g_assignN;

HOOK_DEFINE_TRAMPOLINE(TeammateHook) {
    static void Callback(void* actor, bool teammate, bool favor) {
        Orig(actor, teammate, favor);
        if (!actor) return;
        u32 id = *reinterpret_cast<u32*>(static_cast<u8*>(actor) + 0x14);
        if ((id >> 24) == 0xFF) return;          // puppets and other runtime actors
        Lock();
        int k = -1;
        for (int i = 0; i < g_mineN; i++) if (g_mine[i] == id) k = i;
        if (teammate && k < 0 && g_mineN < MAX) g_mine[g_mineN++] = id;
        /* companions are one player's (03.10): recruiting / dismissing goes to the server, which says no when it is
           someone else's — then it is taken back off this console in Pump */
        u16 cur = 0xFFFE;
        for (int i = 0; i < g_assignN; i++) if (g_assign[i].formId == id) cur = g_assign[i].leader;
        Unlock();
        if (!g_applying && Status() == NetStatus::Connected) {
            if (teammate && cur != MyId()) PushFollow(id, MyId());
            else if (!teammate && cur == MyId()) PushFollow(id, 0);
        }
    }
};

void KeepNear(void* actor, void* target) {
    reinterpret_cast<void (*)(void*, u32, void*, void*, float, float, float, float, float, float, float, float)>(g_keep)(
        nullptr, 0, actor, target, 0.0f, -120.0f, 0.0f, 0.0f, 0.0f, 0.0f, 300.0f, 150.0f);
}

}  // namespace

bool Resolve() {
    g_teammate = game::Checked177(TEAMMATE.off, TEAMMATE.sig);
    g_keep = game::Checked177(KEEP_OFFSET.off, KEEP_OFFSET.sig);
    g_clear = game::Checked177(CLEAR_OFFSET.off, CLEAR_OFFSET.sig);
    g_moveTo = game::Checked177(MOVE_TO.off, MOVE_TO.sig);
    g_moveHome = game::Checked177(MOVE_HOME.off, MOVE_HOME.sig);
    g_isTeammate = game::Checked177(IS_TEAMMATE.off, IS_TEAMMATE.sig);
    g_ok = g_teammate && g_keep && g_clear && g_moveTo && g_moveHome;
    if (g_ok) TeammateHook::InstallAtPtr(g_teammate);
    return g_ok;
}

bool Available() { return g_ok; }

int List(u32* ids, char (*names)[40], u16* leaders, int max) {
    Lock();
    int n = 0;
    for (int i = 0; i < g_mineN && n < max; i++) ids[n++] = g_mine[i];
    for (int i = 0; i < g_assignN && n < max; i++) {            // followers other players share with you
        bool have = false;
        for (int k = 0; k < n; k++) have |= ids[k] == g_assign[i].formId;
        if (!have) ids[n++] = g_assign[i].formId;
    }
    for (int i = 0; i < n; i++) {
        leaders[i] = 0;
        for (int k = 0; k < g_assignN; k++) if (g_assign[k].formId == ids[i]) leaders[i] = g_assign[k].leader;
        snprintf(names[i], 40, "%08x", ids[i]);
        auto ref = static_cast<u8*>(equip::Lookup(ids[i]));
        void* base = ref ? *reinterpret_cast<void**>(ref + 0x40) : nullptr;
        const char* nm = base ? *reinterpret_cast<const char**>(static_cast<u8*>(base) + 0xD0) : nullptr;
        if (nm && nm[0]) snprintf(names[i], 40, "%s", nm);
    }
    Unlock();
    return n;
}

bool OwnedByOther(u32 formId) {
    if (Status() != NetStatus::Connected) return false;
    Lock();
    bool other = false;
    for (int i = 0; i < g_assignN; i++)
        if (g_assign[i].formId == formId) other = g_assign[i].leader && g_assign[i].leader != MyId();
    Unlock();
    return other;
}

void Claim(u32 formId) {
    if (Status() != NetStatus::Connected || !formId || (formId >> 24) == 0xFF) return;
    Lock();
    bool mine = false, known = false;
    for (int i = 0; i < g_assignN; i++)
        if (g_assign[i].formId == formId) { known = true; mine = g_assign[i].leader == MyId(); }
    Unlock();
    if (mine || (known && OwnedByOther(formId))) return;
    PushFollow(formId, MyId());
    Received(formId, MyId());
    Debug("horse %08x claimed", formId);
}

void Command(u32 formId, u16 leader) { PushFollow(formId, leader); Received(formId, leader); }

void Received(u32 formId, u16 leader) {
    Lock();
    int k = -1;
    for (int i = 0; i < g_assignN; i++) if (g_assign[i].formId == formId) k = i;
    if (k < 0 && g_assignN < MAX) k = g_assignN++;
    if (k >= 0) g_assign[k] = Assign{formId, leader, false, 0};
    Unlock();
    Debug("followers: %08x -> leader %u", formId, leader);
}

void Pump() {
    if (!g_ok) return;
    static u64 next;
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    if (now < next) return;
    next = now + 2000;
    void* player = game::Player();
    if (!player) return;
    /* followers hired before we came online (in the save): claimed once the server's list has arrived (10 s after
       connecting), unless the campaign already gives them to someone */
    static u64 onlineSince;
    if (Status() != NetStatus::Connected) onlineSince = 0;
    else if (!onlineSince) onlineSince = now;
    static u64 claimAt;
    if (g_isTeammate && onlineSince && now - onlineSince > 10000 && now - claimAt > 10000) {
        claimAt = now;
        void* actors[256];
        int na = game::HighActors(actors, 256);
        for (int i = 0; i < na; i++) {
            auto a = static_cast<u8*>(actors[i]);
            if (!a || a == player) continue;
            u32 id = *reinterpret_cast<u32*>(a + 0x14);
            if ((id >> 24) == 0xFF) continue;
            if (!reinterpret_cast<bool (*)(void*, u32, void*)>(g_isTeammate)(game::FakeVm(), 0, a)) continue;
            bool known = false;
            Lock();
            for (int k = 0; k < g_assignN; k++) known |= g_assign[k].formId == id;
            Unlock();
            if (known) continue;
            PushFollow(id, MyId());
            Received(id, MyId());
            Debug("follower %08x (hired before) claimed", id);
        }
    }
    Assign todo[MAX];
    Lock();
    int n = g_assignN;
    memcpy(todo, g_assign, n * sizeof(Assign));
    Unlock();
    const float* me = reinterpret_cast<const float*>(static_cast<u8*>(player) + game::Get().offPos);
    static u64 homeAt[MAX];
    for (int i = 0; i < n; i++) {
        auto actor = static_cast<u8*>(equip::Lookup(todo[i].formId));
        if (!actor || actor[0x1A] != 0x3E) continue;
        void* race = world::RaceOf(actor);                       // a horse: only owned (trade.cpp blocks the others)
        if (race && *reinterpret_cast<u32*>(static_cast<u8*>(race) + 0x14) == 0x000131FD) continue;
        u16 leader = todo[i].leader;
        bool loaded = *reinterpret_cast<void**>(actor + 0x68) != nullptr;
        if (leader == MyId() && !loaded) {                        // ours, somewhere else (we just came online): to us
            reinterpret_cast<bool (*)(void*, u32, void*, void*, float, float, float, bool)>(g_moveTo)(
                game::FakeVm(), 0, actor, player, -100.0f, -100.0f, 0.0f, true);
            Debug("follower %08x brought to its owner", todo[i].formId);
            continue;
        }
        if (!loaded) continue;                                     // not loaded here
        void* target = !leader || leader == HOME ? nullptr : leader == MyId() ? player : game::PuppetRef(leader);
        if (!leader) {                                            // released: back to its own life
            reinterpret_cast<void (*)(void*, u32, void*)>(g_clear)(nullptr, 0, actor);
            continue;
        }
        if (!target) {
            /* its owner is offline (HOME) or elsewhere: it does not go with us — if it is next to us (our console's
               follower package, a refused recruiting), it walks home; every 15 s at most */
            const float* a = reinterpret_cast<const float*>(actor + game::Get().offPos);
            float dx = a[0] - me[0], dy = a[1] - me[1];
            if (dx * dx + dy * dy < 2500.0f * 2500.0f && now - homeAt[i] > 15000) {
                homeAt[i] = now;
                reinterpret_cast<void (*)(void*, u32, void*)>(g_clear)(nullptr, 0, actor);
                g_applying = true;
                reinterpret_cast<void (*)(void*, bool, bool)>(g_teammate)(actor, false, false);
                g_applying = false;
                reinterpret_cast<void (*)(void*, u32, void*)>(g_moveHome)(game::FakeVm(), 0, actor);
                Debug("follower %08x: not ours, sent home (leader %u)", todo[i].formId, leader);
            }
            continue;
        }
        const float* a = reinterpret_cast<const float*>(actor + game::Get().offPos);
        const float* t = reinterpret_cast<const float*>(static_cast<u8*>(target) + game::Get().offPos);
        float dx = a[0] - t[0], dy = a[1] - t[1];
        if (dx * dx + dy * dy > 1500.0f * 1500.0f) Debug("follower %08x: %.0f from its leader %u", todo[i].formId, sqrtf(dx * dx + dy * dy), leader);
        if (dx * dx + dy * dy > 3000.0f * 3000.0f)                 // far away (another place): bring it over
            reinterpret_cast<bool (*)(void*, u32, void*, void*, float, float, float, bool)>(g_moveTo)(
                game::FakeVm(), 0, actor, target, -100.0f, -100.0f, 0.0f, true);
        KeepNear(actor, target);
        if (!todo[i].applied || todo[i].appliedLeader != leader) {
            g_applying = true;
            reinterpret_cast<void (*)(void*, bool, bool)>(g_teammate)(actor, true, leader == MyId());
            g_applying = false;
            Lock();
            for (int k = 0; k < g_assignN; k++)
                if (g_assign[k].formId == todo[i].formId) { g_assign[k].applied = true; g_assign[k].appliedLeader = leader; }
            Unlock();
            Debug("follower %08x now follows %u", todo[i].formId, leader);
        }
        (void)me;
    }
}

}  // namespace coop::followers
