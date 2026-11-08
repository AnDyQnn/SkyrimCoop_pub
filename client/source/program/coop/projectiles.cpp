#include "projectiles.hpp"
#include "actors.hpp"
#include "client.hpp"
#include "equip.hpp"
#include "game.hpp"
#include "lib.hpp"
#include <cstring>

/* Projectiles the Skyrim Together way (Projectile.cpp HookLaunch, CombatService::OnNotifyProjectileLaunch, 08.10):
   the shooter's console launches the real projectile and sends the launch (ACT_PROJ); the others launch the same
   projectile from the shooter's copy there (a player's puppet, an NPC's remote copy), from the same origin and angles.
   A remote shooter launches nothing of its own. Damage stays one console's (actors.cpp: a puppet's hits do not count
   here, hits on our own player do). Beams and flames (concentration spells) are not sent: ST syncs those as spell
   casts, here magic.cpp's copy on the puppet.
   1.1.177: Projectile::Launch 0x7b5348 (x8 = result handle, x0 = LaunchData*); LaunchData vtable 0x1a37390. Fields
   (Eden, a live firebolt): origin +0x08, base +0x20, shooter +0x28, combat controller +0x30, weapon +0x38, ammo +0x40,
   angles +0x48 / +0x4C, a counted pointer +0x50 (not sent), floats +0x60 / +0x64, cell +0x68, spell +0x70, casting
   source +0x78, +0x7C, poison +0x88, area +0x90, power +0x94, scale +0x98, flags +0x9C..+0xA2 (+0xA0 use origin).
   The projectile base's type: +0x78 & 0x7F0000 (0x40000 beam, 0x80000 flame). */
namespace coop::projectiles {
namespace {

struct Fn { u32 off; u32 sig[4]; };
constexpr Fn LAUNCH{0x7b5348, {0xFC190FE8, 0xA9017BFD, 0x910043FD, 0xA9026FFC}};
constexpr u32 LAUNCH_DATA_VT = 0x1a37390, BEAM = 0x40000, FLAME = 0x80000;
uintptr_t g_launch;
volatile bool g_applying;

struct Handle { u32 v; Handle() : v(0) {} ~Handle() {} };   // not trivial: returned through x8, as the game does

template <class T> T& At(void* p, u32 off) { return *reinterpret_cast<T*>(static_cast<u8*>(p) + off); }
u32 Id(void* form) { return form ? game::RefFormId(form) : 0; }

/* launches of ours, from any thread: a short spin lock */
proto::ProjMsg g_out[16];
u32 g_outHead, g_outTail;
volatile u32 g_lock;
void Lock() { while (__atomic_exchange_n(&g_lock, 1u, __ATOMIC_ACQUIRE)) {} }
void Unlock() { __atomic_store_n(&g_lock, 0u, __ATOMIC_RELEASE); }

bool Concentration(u8* data) {
    void* base = At<void*>(data, 0x20);
    if (!base) return true;
    u32 t = At<u32>(base, 0x78) & 0x7F0000;
    return t == BEAM || t == FLAME;
}

HOOK_DEFINE_TRAMPOLINE(LaunchHook) {
    static Handle Callback(u8* data) {
        if (g_applying || !data) return Orig(data);
        void* shooter = At<void*>(data, 0x28);
        if (shooter && shooter == game::Player()) {
            static int said;
            void* b = At<void*>(data, 0x20);
            if (said++ < 20) Debug("снаряд игрока: база %08x тип %x заклинание %08x", Id(b), b ? At<u32>(b, 0x78) & 0x7F0000 : 0u, Id(At<void*>(data, 0x70)));
        }
        if (!shooter || static_cast<u8*>(shooter)[0x1A] != 0x3E || Concentration(data)) return Orig(data);
        if (actors::IsRemote(shooter)) return Handle{};          // a remote actor launches nothing of its own
        u32 sid = 0;
        if (shooter == game::Player()) sid = proto::ACT_PLAYER | MyId();
        else if (!actors::OwnedSid(shooter, &sid)) return Orig(data);
        proto::ProjMsg m{};
        m.sid = sid;
        memcpy(&m.ox, data + 0x08, 12);
        m.a48 = At<f32>(data, 0x48); m.a4C = At<f32>(data, 0x4C); m.f60 = At<f32>(data, 0x60); m.f64 = At<f32>(data, 0x64);
        m.base = Id(At<void*>(data, 0x20)); m.weapon = Id(At<void*>(data, 0x38)); m.ammo = Id(At<void*>(data, 0x40));
        m.cell = Id(At<void*>(data, 0x68)); m.spell = Id(At<void*>(data, 0x70)); m.poison = Id(At<void*>(data, 0x88));
        m.source = At<u32>(data, 0x78); m.b7C = data[0x7C];
        m.area = At<s32>(data, 0x90); m.power = At<f32>(data, 0x94); m.scale = At<f32>(data, 0x98);
        memcpy(m.flags, data + 0x9C, 7);
        Lock();
        if (g_outHead - g_outTail < 16) g_out[g_outHead++ % 16] = m;
        Unlock();
        return Orig(data);
    }
};

}  // namespace

bool Resolve() {
    g_launch = game::Checked177(LAUNCH.off, LAUNCH.sig);
    if (g_launch) LaunchHook::InstallAtPtr(g_launch);
    Log("снаряды: %s", g_launch ? "есть" : "НЕТ");
    return g_launch != 0;
}

void Pump() {
    for (int i = 0; i < 16; i++) {
        Lock();
        bool any = g_outTail != g_outHead;
        proto::ProjMsg m = any ? g_out[g_outTail++ % 16] : proto::ProjMsg{};
        Unlock();
        if (!any) break;
        PushActorPacket(proto::ACT_PROJ, &m, sizeof m);
    }
}

/* game thread: the owner's launch, from its copy here */
void Received(const proto::ProjMsg& m, void* shooter) {
    if (!g_launch || !shooter || !game::Player()) return;
    void* base = equip::Lookup(m.base);
    if (!base) return;
    alignas(16) u8 d[0xB0] = {};
    At<uintptr_t>(d, 0) = game::ModuleAddr(LAUNCH_DATA_VT);
    memcpy(d + 0x08, &m.ox, 12);
    At<void*>(d, 0x20) = base;
    At<void*>(d, 0x28) = shooter;
    At<void*>(d, 0x38) = m.weapon ? equip::Lookup(m.weapon) : nullptr;
    At<void*>(d, 0x40) = m.ammo ? equip::Lookup(m.ammo) : nullptr;
    At<f32>(d, 0x48) = m.a48; At<f32>(d, 0x4C) = m.a4C; At<f32>(d, 0x60) = m.f60; At<f32>(d, 0x64) = m.f64;
    void* cell = m.cell ? equip::Lookup(m.cell) : nullptr;
    if (!cell) cell = At<void*>(shooter, game::Get().offCell);   // Launch needs a cell (ST: it relies on pParentCell)
    if (!cell) return;
    At<void*>(d, 0x68) = cell;
    At<void*>(d, 0x70) = m.spell ? equip::Lookup(m.spell) : nullptr;
    At<u32>(d, 0x78) = m.source; d[0x7C] = m.b7C;
    At<void*>(d, 0x88) = m.poison ? equip::Lookup(m.poison) : nullptr;
    At<s32>(d, 0x90) = m.area; At<f32>(d, 0x94) = m.power; At<f32>(d, 0x98) = m.scale;
    memcpy(d + 0x9C, m.flags, 7);
    d[0xA0] = 1;                                         // always the origin sent (ST: else it recalculates it and desyncs)
    g_applying = true;
    Handle h = reinterpret_cast<Handle (*)(u8*)>(g_launch)(d);
    g_applying = false;
    static int said;
    if (said++ < 30) Debug("снаряд от %08x: база %08x, заклинание %08x -> %x", m.sid, m.base, m.spell, h.v);
}

}  // namespace coop::projectiles
