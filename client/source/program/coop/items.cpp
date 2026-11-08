#include "items.hpp"
#include "client.hpp"
#include "quests.hpp"
#include "equip.hpp"
#include "game.hpp"
#include "lib.hpp"
#include <nn/os.hpp>
#include <cstring>

/* Things on the ground (03.10): what a player drops lies there for everybody; who picks it up takes it from everybody.
   A drop is the player's TESObjectREFR::RemoveItem with reason "dropping" (vtable +0x2b0, 1.1.177; the Papyrus
   DropObject task calls the same slot) — the dropped reference is a runtime one (FF..), different on every console,
   so it travels as an item id of ours: (player id << 16) | n. The others place a reference of the same base where it
   fell (when they are in that cell or worldspace); picking up (activating) any of them sends "gone". The server keeps
   the list (world.json) for players who come later. */
namespace coop::items {
using proto::WorldHead;
namespace {

constexpr u32 VT_REMOVE_ITEM = 0x2b0;
constexpr u32 REASON_DROP = 3;
constexpr int MAX = 64;

/* ObjectRefHandle is returned through x8 (the caller passes a slot: 1.1.177 0xa5b568 "mov x8, sp") — a non-trivial
   type makes the compiler do the same, so the hook keeps the caller's x8 and passes it on */
struct Handle { u32 v = 0; Handle() {} Handle(const Handle& o) : v(o.v) {} ~Handle() {} };

struct Body { u32 base; u32 count; u32 world; u32 cell; float x, y, z; };
struct Item { u32 netId; Body b; void* ref; bool placed; };
Item g_items[MAX];
int g_n;
u16 g_seq;
nn::os::MutexType g_lock;
bool g_lockReady;
void Lock() { if (!g_lockReady) { nn::os::InitializeMutex(&g_lock, false, 0); g_lockReady = true; } nn::os::LockMutex(&g_lock); }
void Unlock() { nn::os::UnlockMutex(&g_lock); }

struct In { u32 netId; bool add; Body b; };
In g_in[32];
int g_inN;                                        // under Lock()
bool g_hooked;

u32 Id(const void* f) { return f ? *reinterpret_cast<const u32*>(static_cast<const u8*>(f) + 0x14) : 0; }

Item* Find(u32 netId) {
    for (int i = 0; i < g_n; i++) if (g_items[i].netId == netId) return &g_items[i];
    return nullptr;
}

void Send(u32 netId, bool add, const Body* b) {
    if (!quests::IntroDone()) return;                    // (07.10) nothing dropped in our intro is the world's
    u8 buf[sizeof(WorldHead) + sizeof(Body)];
    WorldHead h{proto::WK_ITEM, (u16)(add ? 1 : 0), netId};
    memcpy(buf, &h, sizeof h);
    if (add) memcpy(buf + sizeof h, b, sizeof(Body));
    PushWorld(buf, add ? sizeof buf : sizeof h);
}

HOOK_DEFINE_TRAMPOLINE(RemoveItemHook) {
    static Handle Callback(void* self, void* item, s32 count, u32 reason, void* extra, void* moveTo, const float* at, const float* rot) {
        Handle hd = Orig(self, item, count, reason, extra, moveTo, at, rot);
        u32 h = hd.v;
        if (self == game::Player() && reason == REASON_DROP) Debug("items: drop, handle %x", h);
        if (self == game::Player() && reason == REASON_DROP && !moveTo && item && Status() == NetStatus::Connected) {
            void* ref = game::RefFromHandle(h);
            if (!ref) Debug("items: dropped ref not found for handle %x", h);
            u32 cell, world; float pos[3];
            if (ref && game::WhereAmI(&cell, &world, pos)) {
                const float* p = game::RefPos(ref);
                Body b{Id(item), (u32)(count > 0 ? count : 1), world, world ? 0u : cell, p[0], p[1], p[2]};
                u32 netId = ((u32)MyId() << 16) | ++g_seq;
                Lock();
                if (g_n < MAX) g_items[g_n++] = Item{netId, b, ref, true};
                Unlock();
                Send(netId, true, &b);
                Debug("items: dropped %08x x%u -> %08x", b.base, b.count, netId);
            }
        }
        return hd;
    }
};

}  // namespace

void Received(const u8* p, u32 size) {
    if (size < sizeof(WorldHead)) return;
    WorldHead h; memcpy(&h, p, sizeof h);
    In in{h.ref, h.count != 0, {}};
    if (in.add) {
        if (size < sizeof h + sizeof(Body)) return;
        memcpy(&in.b, p + sizeof h, sizeof(Body));
    }
    Lock();
    if (g_inN < 32) g_in[g_inN++] = in;
    Unlock();
}

bool PickedUp(void* ref) {
    if (!ref || Status() != NetStatus::Connected) return false;
    Lock();
    u32 netId = 0;
    for (int i = 0; i < g_n; i++)
        if (g_items[i].ref == ref) { netId = g_items[i].netId; g_items[i] = g_items[--g_n]; break; }
    Unlock();
    if (!netId) return false;
    Send(netId, false, nullptr);
    Debug("items: picked up %08x", netId);
    return true;
}

void Pump() {
    void* player = game::Player();
    if (!player || Status() != NetStatus::Connected || !game::Settled()) return;
    if (!quests::IntroDone()) return;                  // (07.10) the others' items wait until we are out of Helgen
    if (!g_hooked) {                                   /* the player's own RemoveItem (its vtable, read live) */
        g_hooked = true;
        auto vt = *reinterpret_cast<uintptr_t**>(player);
        uintptr_t fn = vt ? vt[VT_REMOVE_ITEM / 8] : 0;
        if (fn && game::Readable(reinterpret_cast<void*>(fn), 4)) RemoveItemHook::InstallAtPtr(fn);
        Log("предметы на земле: %s (+%lx)", fn ? "есть" : "нет функции", (unsigned long)(fn - game::ModuleAddr(0)));
    }
    In in[32];
    Lock();
    int n = g_inN;
    memcpy(in, g_in, n * sizeof(In));
    g_inN = 0;
    for (int i = 0; i < n; i++) {
        Item* it = Find(in[i].netId);
        if (in[i].add) {
            if (!it && g_n < MAX) g_items[g_n++] = Item{in[i].netId, in[i].b, nullptr, false};
        } else if (it) {
            void* ref = it->ref;
            *it = g_items[--g_n];
            Unlock();
            if (ref) game::DeleteRef(ref);               /* someone took it */
            Lock();
        }
    }
    /* place the others' things that lie where we are */
    static u64 at;
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    if (now - at < 1000) { Unlock(); return; }
    at = now;
    u32 cell, world; float pos[3];
    if (!game::WhereAmI(&cell, &world, pos)) { Unlock(); return; }
    for (int i = 0; i < g_n; i++) {
        Item& it = g_items[i];
        if (it.placed || (it.netId >> 16) == MyId()) continue;
        bool here = world ? it.b.world == world : it.b.cell == cell;
        float dx = it.b.x - pos[0], dy = it.b.y - pos[1];
        if (!here || dx * dx + dy * dy > 8000.0f * 8000.0f) continue;
        void* base = equip::Lookup(it.b.base);
        if (!base) { it.placed = true; continue; }
        it.ref = game::PlaceObject(base, it.b.x, it.b.y, it.b.z + 10.0f);
        it.placed = true;
        Debug("items: %08x of a friend placed here (%08x)", it.netId, it.b.base);
    }
    Unlock();
}

}  // namespace coop::items
