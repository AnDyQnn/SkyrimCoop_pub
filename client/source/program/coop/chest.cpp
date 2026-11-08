#include "chest.hpp"
#include "client.hpp"
#include "equip.hpp"
#include "game.hpp"
#include "lib.hpp"
#include "trade.hpp"
#include <nn/os.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* The clan's shared chest: its contents live on the server (social.py, "chest get / delta"). Opening it makes
   a hidden container under the player (any chest base, emptied), fills it with the clan's things and activates
   it as the player: the ordinary container menu. What moves in and out goes to the server as deltas; a take
   the server refuses (another member took it first) is taken back from the player. The container is deleted
   after the session (5 minutes, or when the player walks away). */
namespace coop::chest {
namespace {

constexpr u32 CONT = 0x1C;                   // container form type
constexpr int MAX = 128;

char g_list[1600];
volatile bool g_openWanted;
void* g_ref;
u64 g_activateAt, g_until, g_nextDiff;
trade::Count g_last[MAX];
int g_lastN;
char g_deny[400];
volatile bool g_denyWanted;
float g_origin[3];

template <class T> T At(const void* p, u32 off) { return *reinterpret_cast<const T*>(static_cast<const u8*>(p) + off); }

bool Readable(const void* p, u64 n) {
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    if (!a) return false;
    MemoryInfo mi{}; u32 pi;
    return R_SUCCEEDED(svcQueryMemory(&mi, &pi, a)) && (mi.perm & 1) && mi.addr + mi.size >= a + n;
}

/* a container base: preferably one named "Сундук" (the menu shows the name), from the data handler's list */
void* ChestBase() {
    static void* base;
    if (base) return base;
    auto dh = static_cast<u8*>(game::DataHandler());
    if (!dh) return nullptr;
    u32 o = 8 + CONT * 0x10;
    void** data = At<void**>(dh, o);
    u32 n = At<u32>(dh, o + 0xC);
    if (!n || !Readable(data, 8ull * n)) return nullptr;
    for (u32 i = 0; i < n && i < 4000 && !base; i++) {
        void* f = data[i];
        if (!Readable(f, 0x100) || At<u8>(f, 0x1A) != CONT) continue;
        for (u32 k = 0x20; k < 0x100; k += 8) {
            const char* s = At<const char*>(f, k);
            if (Readable(s, 16) && !strcmp(s, "Сундук")) { base = f; break; }
        }
    }
    if (!base && Readable(data[0], 0x20) && At<u8>(data[0], 0x1A) == CONT) base = data[0];
    Debug("chest: base %08x", base ? At<u32>(base, 0x14) : 0);
    return base;
}

void Close() {
    if (g_ref) game::DeleteRef(g_ref);
    g_ref = nullptr;
    g_until = 0;
}

}  // namespace

volatile bool g_notInClan;

void Open(const char* list) {
    if (!strncmp(list, "#chest none", 11)) { g_notInClan = true; return; }
    snprintf(g_list, sizeof g_list, "%s", list);
    g_openWanted = true;
}

void Denied(const char* text) {
    snprintf(g_deny, sizeof g_deny, "%s", text);
    g_denyWanted = true;
}

void Pump() {
    void* player = game::Player();
    if (!player) return;
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    if (__atomic_exchange_n(&g_notInClan, false, __ATOMIC_ACQ_REL)) game::Notify("Кооп: общий сундук недоступен");
    if (__atomic_exchange_n(&g_openWanted, false, __ATOMIC_ACQ_REL)) {
        Close();
        u32 cell, world;
        if (!game::WhereAmI(&cell, &world, g_origin)) return;
        void* base = ChestBase();
        g_ref = base ? game::PlaceObject(base, g_origin[0], g_origin[1], g_origin[2] - 3000.0f) : nullptr;
        if (!g_ref) { game::Notify("Кооп: общий сундук не открылся"); return; }
        /* empty it (whatever the base holds), then the clan's things */
        equip::RemoveAll(g_ref);
        const char* p = strchr(g_list, ' ');
        int items = 0;
        while (p && *p) {
            while (*p == ' ') p++;
            char* end;
            u32 id = strtoul(p, &end, 16);
            if (end == p || *end != ':') break;
            s32 n = strtol(end + 1, &end, 10);
            p = end;
            if (void* f = equip::Lookup(id)) { trade::AddItems(g_ref, f, n); items++; }
        }
        g_activateAt = now + 700;
        Debug("chest: %d kinds put in", items);
    }
    if (g_ref && g_activateAt && now >= g_activateAt) {
        g_activateAt = 0;
        g_lastN = trade::Changes(g_ref, g_last, MAX);
        trade::ActivateAsPlayer(g_ref);
        g_until = now + 300000;
        g_nextDiff = now + 700;
    }
    if (__atomic_exchange_n(&g_denyWanted, false, __ATOMIC_ACQ_REL)) {
        /* "#chestdeny <hex>:<n> ...": someone else took it first; not ours after all */
        int n = 0;
        for (const char* q = strchr(g_deny, ' '); q && *q;) {
            while (*q == ' ') q++;
            char* end;
            u32 id = strtoul(q, &end, 16);
            if (end == q || *end != ':') break;
            s32 c = strtol(end + 1, &end, 10);
            q = end;
            if (void* f = equip::Lookup(id)) { trade::AddItems(player, f, -c); n++; }
        }
        if (n) game::Notify("Кооп: это уже взял другой игрок");
    }
    if (!g_ref || g_activateAt) return;
    if (g_until && now > g_until) { Close(); return; }
    u32 cell, world; float pos[3];
    if (game::WhereAmI(&cell, &world, pos)) {
        float dx = pos[0] - g_origin[0], dy = pos[1] - g_origin[1];
        if (dx * dx + dy * dy > 600.0f * 600.0f) { Close(); return; }       // walked away: the session is over
    }
    if (now < g_nextDiff) return;
    g_nextDiff = now + 700;
    trade::Count cur[MAX];
    int n = trade::Changes(g_ref, cur, MAX);
    char cmd[480];
    int len = snprintf(cmd, sizeof cmd, "chest delta");
    int changes = 0;
    auto countOf = [](const trade::Count* c, int cn, u32 id) { for (int i = 0; i < cn; i++) if (c[i].id == id) return c[i].n; return 0; };
    for (int i = 0; i < n && len < (int)sizeof cmd - 24; i++) {
        s32 d = cur[i].n - countOf(g_last, g_lastN, cur[i].id);
        if (d) { len += snprintf(cmd + len, sizeof cmd - len, " %08x:%+d", cur[i].id, d); changes++; }
    }
    for (int i = 0; i < g_lastN && len < (int)sizeof cmd - 24; i++) {
        if (countOf(cur, n, g_last[i].id) || !g_last[i].n) continue;          // gone from the list entirely
        bool listed = false;
        for (int k = 0; k < n; k++) listed |= cur[k].id == g_last[i].id;
        if (!listed) { len += snprintf(cmd + len, sizeof cmd - len, " %08x:%+d", g_last[i].id, -g_last[i].n); changes++; }
    }
    if (!changes) return;
    SocialSend(cmd, false);
    memcpy(g_last, cur, n * sizeof cur[0]);
    g_lastN = n;
}

}  // namespace coop::chest
