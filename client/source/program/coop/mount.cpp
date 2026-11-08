#include "mount.hpp"
#include "followers.hpp"
#include "client.hpp"
#include "equip.hpp"
#include "game.hpp"
#include "lib.hpp"
#include <nn/os.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

/* Riding. Ours goes out as "coophorse <horse base FormID>" (0 = on foot) when it changes and every 10 s while
   riding (for players who come later); on the other consoles a horse of that base is placed under the puppet and
   the puppet activates it (an NPC activating a horse mounts it), then the horse is what follows the player's
   positions (game.cpp asks Carrier()). On foot again: the puppet dismounts and the horse is deleted.
   1.1.177 Papyrus natives: Actor.IsOnMount 0xa010d4 (vm, stack, actor) -> bool (extra data 0xA9 on the actor);
   Game.GetPlayersLastRiddenHorse 0xa2ac50 (vm, stack) -> Actor* (player vtable +0x970 -> handle);
   Actor.Dismount 0x9fd454 -> Actor::Dismount 0x633bd8(actor). */
namespace coop::mount {
namespace {

struct Fn { u32 off; u32 sig[4]; };
constexpr Fn IS_ON_MOUNT{0xa010d4, {0xA9BE7BFD, 0xA9014FF4, 0x910003FD, 0x3948D048}};
constexpr Fn LAST_HORSE{0xa2ac50, {0xD100C3FF, 0xA9017BFD, 0x910043FD, 0xF90013F3}};
constexpr Fn DISMOUNT{0x633bd8, {0xD10243FF, 0xA9057BFD, 0x910143FD, 0xA9065FF8}};
constexpr u32 ACTIVATE = 0x1deeb8;    // TESObjectREFR::ActivateRef (console.cpp, trade.cpp)
constexpr u32 HORSE_RACE = 0x000131FD, OFF_ACTOR_RACE = 0x228, FORM_NPC = 0x2B;

uintptr_t g_isOnMount, g_lastHorse, g_dismount, g_activate;
bool g_ok;

template <class T> T At(const void* p, u32 off) { return *reinterpret_cast<const T*>(static_cast<const u8*>(p) + off); }
u32 Id(const void* f) { return f ? At<u32>(f, 0x14) : 0; }
u64 Now() { return nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds(); }

bool OnMount(void* actor) {
    return actor && reinterpret_cast<bool (*)(void*, u32, void*)>(g_isOnMount)(game::FakeVm(), 0, actor);
}
void* LastHorse() { return reinterpret_cast<void* (*)(void*, u32)>(g_lastHorse)(game::FakeVm(), 0); }
void Activate(void* target, void* who) {
    reinterpret_cast<bool (*)(void*, void*, u8, void*, s32, bool)>(g_activate)(target, who, 0, nullptr, 1, false);
}

/* any horse NPC base (the fallback when a player's own horse base is unknown here): the race field of TESNPC is
   found as the player's base's pointer equal to the player's race */
void* AnyHorseBase() {
    static void* found;
    static bool tried;
    if (found || tried) return found;
    void* player = game::Player();
    auto dh = static_cast<u8*>(game::DataHandler());
    if (!player || !dh) return nullptr;
    tried = true;
    void* base = At<void*>(player, 0x40);
    void* race = At<void*>(player, OFF_ACTOR_RACE);
    u32 off = 0;
    for (u32 o = 0x30; o < 0x300 && !off; o += 8)
        if (At<void*>(base, o) == race) off = o;
    u32 o = 8 + FORM_NPC * 0x10;
    auto data = At<void**>(dh, o);
    u32 n = At<u32>(dh, o + 0xC);
    Debug("mount: TESNPC race at +%x, %u NPC bases", off, n);
    if (!off || !data || n > 200000) return nullptr;
    for (u32 i = 0; i < n; i++)
        if (data[i] && Id(At<void*>(data[i], off)) == HORSE_RACE) { found = data[i]; break; }
    Debug("mount: horse base %08x", Id(found));
    return found;
}

/* ours: set by the game's HorseEnter / HorseExit actions (actions.cpp), never polled — IsOnMount takes the actor's
   extra-data lock, and polling it during a load froze the game */
volatile bool g_localOn;
u32 g_sentBase;
bool g_sentMounted;
u64 g_nextCheck, g_nextResend;

/* theirs, by player id */
struct Rider { u16 id; u32 base, placedFor; void* horse; bool mounted, aiOff; u64 placedMs, triedMs; };
Rider g_riders[MAX_REMOTE];

Rider* Slot(u16 id, bool make) {
    for (auto& r : g_riders)
        if (r.id == id) return &r;
    if (!make) return nullptr;
    for (auto& r : g_riders)
        if (!r.id) { r = Rider{id, 0, 0, nullptr, false, false, 0, 0}; return &r; }
    return nullptr;
}

void DropHorse(Rider& r, void* puppet) {
    if (puppet && r.mounted) reinterpret_cast<void (*)(void*)>(g_dismount)(puppet);
    if (r.horse) game::DeleteRef(r.horse);
    r.horse = nullptr;
    r.mounted = false;
}

}  // namespace

bool Resolve() {
    g_isOnMount = game::Checked177(IS_ON_MOUNT.off, IS_ON_MOUNT.sig);
    g_lastHorse = game::Checked177(LAST_HORSE.off, LAST_HORSE.sig);
    g_dismount = game::Checked177(DISMOUNT.off, DISMOUNT.sig);
    g_activate = game::ModuleAddr(ACTIVATE);
    g_ok = g_isOnMount && g_lastHorse && g_dismount;
    return g_ok;
}

void Received(u16 playerId, u32 horseBase) {
    if (Rider* r = Slot(playerId, horseBase != 0)) r->base = horseBase;
}

void ActorAction(void* actor, const char* name) {
    bool on = !strcmp(name, "HorseEnter");
    if (actor == game::Player()) { g_localOn = on; return; }
    u16 id = game::PuppetOwner(actor);
    if (Rider* r = id ? Slot(id, false) : nullptr) {
        r->mounted = on && r->horse;                     // the game thread does the rest (Pump)
        r->aiOff = false;
    }
}

void* Carrier(void* puppet) {
    if (!g_ok || !puppet) return puppet;
    u16 id = game::PuppetOwner(puppet);
    Rider* r = id ? Slot(id, false) : nullptr;
    return r && r->horse && r->mounted ? r->horse : puppet;
}

void Pump() {
    void* player = game::Player();
    if (!g_ok || !player || !game::Settled()) return;
    u64 now = Now();
    if (now < g_nextCheck) return;
    g_nextCheck = now + 300;

    /* ours out */
    if (Status() == NetStatus::Connected) {
        bool on = g_localOn;
        void* horse = on && !g_sentMounted ? LastHorse() : nullptr;
        if (horse) followers::Claim(Id(horse));               // (03.10) mounted = yours: nobody else rides it
        u32 base = horse ? Id(At<void*>(horse, 0x40)) : (on ? g_sentBase : 0);
        if (on && !base) base = Id(AnyHorseBase());
        if (on != g_sentMounted || base != g_sentBase || (on && now >= g_nextResend)) {
            char ev[40];
            snprintf(ev, sizeof ev, "coophorse %08x", on ? base : 0);
            PushEvent(ev);
            if (on != g_sentMounted) Log("верхом: %s (%08x)", on ? "да" : "нет", base);
            g_sentMounted = on; g_sentBase = base; g_nextResend = now + 10000;
        }
    }

    /* theirs */
    for (auto& r : g_riders) {
        if (!r.id) continue;
        void* puppet = game::PuppetRef(r.id);
        if (!r.base || !puppet) {                                   // on foot, or no puppet (gone, reloading)
            if (r.horse) DropHorse(r, puppet);
            if (!r.base) r.id = 0;
            continue;
        }
        if (r.horse && r.placedFor != r.base) DropHorse(r, puppet);             // they got on another horse
        if (!r.horse) {
            if (r.placedMs && now - r.placedMs < 2000) continue;      // never a stream of horses
            void* base = equip::Lookup(r.base);
            if (!base || At<u8>(base, 0x1A) != FORM_NPC) base = AnyHorseBase();
            const float* p = game::RefPos(puppet);
            r.horse = base ? game::PlaceObject(base, p[0], p[1], p[2]) : nullptr;
            r.placedMs = now; r.triedMs = 0; r.mounted = false; r.placedFor = r.base;
            Log("кукла %u: лошадь %08x -> %08x", r.id, Id(base), Id(r.horse));
            continue;
        }
        if (r.mounted && !r.aiOff) {                           // it sat down: the horse is ours to move
            r.aiOff = true;
            game::PuppetAi(r.horse, false);
            Log("кукла %u верхом", r.id);
        }
        if (!r.mounted) {                                      // HorseEnter (ActorAction) says when it sits
            if (now - r.placedMs > 500 && now - r.triedMs > 3000) {         // 3D ready: it gets on (again)
                r.triedMs = now;
                Activate(r.horse, puppet);
            }
        }
    }
}

bool Command(const char* command) {
    if (!g_ok) return false;
    void* player = game::Player();
    if (!strcasecmp(command, "horse")) {                          // a horse in front of the player
        void* base = AnyHorseBase();
        const float* p = game::RefPos(player);
        void* h = base ? game::PlaceObject(base, p[0] + 150.0f, p[1], p[2]) : nullptr;
        Log("консоль: лошадь %08x", Id(h));
        return true;
    }
    if (!strncasecmp(command, "ride ", 5)) {                     // ride <rider FormID> <horse FormID>
        char* end;
        void* rider = equip::Lookup(strtoul(command + 5, &end, 16));
        void* horse = equip::Lookup(strtoul(end, nullptr, 16));
        if (rider && horse) Activate(horse, rider);
        Log("консоль: ride %p %p", rider, horse);
        return true;
    }
    if (!strncasecmp(command, "dismount ", 9)) {
        if (void* a = equip::Lookup(strtoul(command + 9, nullptr, 16))) reinterpret_cast<void (*)(void*)>(g_dismount)(a);
        return true;
    }
    if (!strcasecmp(command, "mounted")) {
        void* h = LastHorse();
        Log("консоль: игрок верхом %d, последняя лошадь %08x (база %08x)", OnMount(player), Id(h), h ? Id(At<void*>(h, 0x40)) : 0);
        for (auto& r : g_riders)
            if (r.id) {
                void* pup = game::PuppetRef(r.id);
                const float* hp = r.horse ? game::RefPos(r.horse) : nullptr;
                const float* pp = pup ? game::RefPos(pup) : nullptr;
                Log("  кукла %u: база %08x лошадь %08x верхом %d (IsOnMount %d), лошадь %.0f %.0f %.0f, кукла %.0f %.0f %.0f",
                    r.id, r.base, Id(r.horse), r.mounted, pup ? OnMount(pup) : -1, hp ? hp[0] : 0, hp ? hp[1] : 0, hp ? hp[2] : 0,
                    pp ? pp[0] : 0, pp ? pp[1] : 0, pp ? pp[2] : 0);
            }
        return true;
    }
    return false;
}

}  // namespace coop::mount
