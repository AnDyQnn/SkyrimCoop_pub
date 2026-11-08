#include "weather.hpp"
#include "quests.hpp"
#include "client.hpp"
#include "equip.hpp"
#include "game.hpp"
#include "lib.hpp"
#include <nn/os.hpp>
#include <cstdio>

/* One sky for the room: every console reports its weather when it changes ("coopweather <FormID>" as an
   animation event); the server passes only the room host's on, and the others force the same weather.
   1.1.177 Papyrus natives: Weather.GetCurrentWeather 0xa75028 (vm, stack) -> TESWeather*;
   Weather.ForceActive 0xa756d0 (vm, stack, weather, override) -> 0x5fc3fc(*GOT 0x2144d60, weather, override). */
namespace coop::weather {
namespace {

struct Fn { u32 off; u32 sig[4]; };
constexpr Fn GET_CURRENT{0xa75028, {0xA9BC7BFD, 0xF9000BF7, 0x910003FD, 0xA90257F6}};
constexpr Fn FORCE_ACTIVE{0xa756d0, {0xF000B668, 0xF946B108, 0xF9400100, 0xAA0203E1}};

uintptr_t g_get, g_force;
bool g_ok;
u32 g_sent, g_wanted;
volatile u32 g_in;
u64 g_nextCheck, g_nextForce;

void* Current() { return reinterpret_cast<void* (*)(void*, u32, void*)>(g_get)(game::FakeVm(), 0, nullptr); }
u32 Id(const void* f) { return f ? *reinterpret_cast<const u32*>(static_cast<const u8*>(f) + 0x14) : 0; }

}  // namespace

bool Resolve() {
    g_get = game::Checked177(GET_CURRENT.off, GET_CURRENT.sig);
    g_force = game::Checked177(FORCE_ACTIVE.off, FORCE_ACTIVE.sig);
    g_ok = g_get && g_force;
    return g_ok;
}

void Received(u32 weatherId) { g_in = weatherId; }

void Pump() {
    if (!g_ok || !game::Player() || Status() != NetStatus::Connected) return;
    if (!quests::IntroDone()) return;                    // (07.10) Helgen's own sky in our intro
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    if (u32 w = __atomic_exchange_n(&g_in, 0u, __ATOMIC_ACQ_REL)) { g_wanted = w; g_nextForce = 0; }
    if (now < g_nextCheck) return;
    g_nextCheck = now + 2000;
    void* cur = Current();
    u32 id = Id(cur);
    static int probes;
    if (probes < 6) { probes++; Debug("weather: current %p id %08x wanted %08x", cur, id, g_wanted); }
    /* ours out when it changes (only the host's is passed on by the server) */
    if (id && id != g_sent && id != g_wanted) {
        g_sent = id;
        char ev[40];
        snprintf(ev, sizeof ev, "coopweather %08x", id);
        PushEvent(ev);
        Debug("weather: ours is %08x (type %02x)", id, *(static_cast<u8*>(cur) + 0x1A));
    }
    /* the host's in: forced, and again every 30 s while this region's own weather takes over */
    if (g_wanted && id != g_wanted && now >= g_nextForce) {
        g_nextForce = now + 30000;
        void* w = equip::Lookup(g_wanted);
        if (!w) return;
        reinterpret_cast<void (*)(void*, u32, void*, bool)>(g_force)(game::FakeVm(), 0, w, false);
        g_sent = g_wanted;
        Debug("weather: the host's %08x (ours was %08x)", g_wanted, id);
    }
}

}  // namespace coop::weather
