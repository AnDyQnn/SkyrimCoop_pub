#include "plates.hpp"
#include "appearance.hpp"
#include "client.hpp"
#include "game.hpp"
#include "lib.hpp"
#include <nn/os.hpp>
#include <cstdio>
#include <cstring>

/* MMO nameplates. Ours: level and health / magicka / stamina in percent go out as STATS (every 0.5 s when they
   changed, every 5 s anyway). Theirs: the puppet's name becomes "[34] AnDy" (shown when the crosshair is on it;
   the HUD layer draws the bars from the same numbers).
   1.1.177 Papyrus natives: Actor.GetLevel 0x9ff89c (vm, stack, actor) -> Actor::GetLevel 0x608c74;
   Actor.GetActorValuePercentage 0x9fe2f0 (vm, stack, actor, BSFixedString* name) -> float 0..1. */
namespace coop::plates {
namespace {

struct Fn { u32 off; u32 sig[4]; };
constexpr Fn GET_LEVEL{0x9ff89c, {0xA9BF7BFD, 0x910003FD, 0xAA0203E0, 0x97F024F3}};
constexpr Fn GET_AV_PCT{0x9fe2f0, {0xD10783FF, 0xA91A7BFD, 0x910683FD, 0xF900DBFC}};
constexpr Fn FS_CTOR{0xae6da8, {0xA9BE7BFD, 0xF9000BF3, 0x910003FD, 0xF900001F}};

struct FixedString { const char* data; };
uintptr_t g_level, g_pct, g_fsCtor;
bool g_ok;
FixedString g_names[3];                 // Health, Magicka, Stamina (made on the game thread)
u64 g_next, g_lastSent;
proto::StatsMsg g_last;
struct Shown { u16 id; u16 level; char name[32]; };
Shown g_shown[MAX_REMOTE];

u8 Pct(void* actor, int i) {
    float f = reinterpret_cast<float (*)(void*, u32, void*, FixedString*)>(g_pct)(game::FakeVm(), 0, actor, &g_names[i]);
    if (!(f >= 0.0f)) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return (u8)(f * 100.0f + 0.5f);
}

}  // namespace

bool Resolve() {
    g_level = game::Checked177(GET_LEVEL.off, GET_LEVEL.sig);
    g_pct = game::Checked177(GET_AV_PCT.off, GET_AV_PCT.sig);
    g_fsCtor = game::Checked177(FS_CTOR.off, FS_CTOR.sig);
    g_ok = g_level && g_pct && g_fsCtor;
    return g_ok;
}

void Pump() {
    void* player = game::Player();
    if (!g_ok || !player || !game::Settled()) return;
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    if (now < g_next) return;
    g_next = now + 1000;
    if (!g_names[0].data) {
        const char* n[3] = {"Health", "Magicka", "Stamina"};
        for (int i = 0; i < 3; i++) reinterpret_cast<void (*)(FixedString*, const char*)>(g_fsCtor)(&g_names[i], n[i]);
    }

    /* ours out */
    if (Status() == NetStatus::Connected) {
        proto::StatsMsg m{0, game::RealLevel(g_level, player),
                   Pct(player, 0), Pct(player, 1), Pct(player, 2), 0};
        bool changed = m.level != g_last.level || m.hp != g_last.hp || m.mp != g_last.mp || m.sp != g_last.sp;
        if (changed || now - g_lastSent > 5000) {
            PublishStats(m.level, m.hp, m.mp, m.sp);
            if (m.level != g_last.level) Log("табличка: уровень %u", m.level);
            g_last = m; g_lastSent = now;
        }
    }

    /* theirs: "[level] name" over the puppet, again when the level or name changes or the puppet is rebuilt */
    RemotePlayer rs[MAX_REMOTE];
    int n = SnapshotRemotes(rs, MAX_REMOTE);
    for (int i = 0; i < n; i++) {
        u16 id = rs[i].state.id;
        proto::StatsMsg st;
        char name[32];
        if (!StatsOf(id, &st) || !PlayerName(id, name, sizeof name)) continue;
        Shown* s = nullptr;
        for (auto& x : g_shown) if (x.id == id) s = &x;
        for (auto& x : g_shown) if (!s && !x.id) s = &x;
        if (!s) continue;
        void* base = appearance::BaseFor(id);
        static void* lastBase[MAX_REMOTE];
        int k = s - g_shown;
        if (s->id == id && s->level == st.level && !strcmp(s->name, name) && lastBase[k] == base) continue;
        char title[48];
        snprintf(title, sizeof title, "[%u] %s", st.level, name);
        if (appearance::SetTitle(id, title)) {
            s->id = id; s->level = st.level; lastBase[k] = base;
            snprintf(s->name, sizeof s->name, "%s", name);
            Debug("табличка игрока %u: %s", id, title);
        }
    }
    for (auto& x : g_shown) {                                     // gone players free their slot
        bool here = false;
        for (int i = 0; i < n; i++) if (rs[i].state.id == x.id) here = true;
        if (x.id && !here) x = Shown{};
    }
}

}  // namespace coop::plates
