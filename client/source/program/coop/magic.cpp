#include "magic.hpp"
#include "client.hpp"
#include "equip.hpp"
#include "game.hpp"
#include "lib.hpp"
#include "trade.hpp"
#include <nn/os.hpp>
#include <cstdio>
#include <cstring>

/* Another player's spells on their puppet, the Skyrim Together way (the caster's console decides what the spell
   does; the others only see it): when the local player's hand releases a spell (graph event MRh_/MLh_SpellFire),
   "coopcast <hand> <spell>" goes out as an animation event; the other consoles make the puppet cast a harmless
   copy of that spell with the Spell.Cast native: the projectile, beam and impact art fly as usual, but every
   effect has magnitude and duration 0, so nothing is hurt, healed or summoned twice (damage to NPCs and players
   already comes through npcsync / pvp). Spells that summon or raise (an effect pointing at an NPC) are not cast.
   1.1.177: Spell.Cast native 0xa35ec0 (vm, stack, spell, source, target): queues a task (the no-op VM);
   TESForm::CreateDuplicateForm = vtable [10]; selected spells: actor + 0x1F8 + hand * 8. */
namespace coop::magic {
namespace {

struct Fn { u32 off; u32 sig[4]; };
constexpr Fn CAST{0xa35ec0, {0xD10183FF, 0xA9017BFD, 0x910043FD, 0xF90013F9}};
/* Weapon.Fire native (vm, stack, weapon, source, ammo) -> 0x5fa240(manager, weapon, source, ammo) */
constexpr Fn FIRE{0xa6e290, {0xA9BC7BFD, 0xF9000BF7, 0x910003FD, 0xA90257F6}};
constexpr u32 WEAP = 0x29, AMMO = 0x2A;
/* damage fields found by content: Hunting Bow 00013985 = 7 and Long Bow 0003B562 = 6 (u16), Iron Arrow
   0001397D = 8 and Steel Arrow 0001397F = 10 (float) */
constexpr u32 HUNTING_BOW = 0x13985, LONG_BOW = 0x3B562, IRON_ARROW = 0x1397D, STEEL_ARROW = 0x1397F;
constexpr u32 SELECTED_SPELLS = 0x1F8, SPEL = 0x16, NPC_ = 0x2B, VT_DUPLICATE = 10;

uintptr_t g_cast, g_fire;
u32 g_weapDamageOff, g_ammoDamageOff;
bool g_ok;

template <class T> T At(const void* p, u32 off) { return *reinterpret_cast<const T*>(static_cast<const u8*>(p) + off); }
u8 Type(const void* f) { return At<u8>(f, 0x1A); }
u32 Id(const void* f) { return At<u32>(f, 0x14); }

bool Readable(const void* p, u64 n) {
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    if (!a || (a & 7)) return false;
    MemoryInfo mi{}; u32 pi;
    return R_SUCCEEDED(svcQueryMemory(&mi, &pi, a)) && (mi.perm & 1) && mi.addr + mi.size >= a + n;
}

/* The spell's effects: a BSTArray<Effect*> {data, capacity u32, size u32} somewhere in MagicItem; found by
   content (entries whose +0x10 is a magic effect form, type 0x12). Offset kept once found. */
u32 g_effectsOff;
bool IsEffect(const void* e) { return Readable(e, 0x28) && Readable(At<void*>(e, 0x10), 0x20) && Type(At<void*>(e, 0x10)) == 0x12; }
bool FindEffects(void* spell, void*** data, u32** size) {
    for (u32 o = g_effectsOff ? g_effectsOff : 0x30; o < (g_effectsOff ? g_effectsOff + 1 : 0x120); o += 8) {
        void** d = At<void**>(spell, o);
        u32 n = At<u32>(spell, o + 0xC);
        if (n == 0 || n > 16 || n > At<u32>(spell, o + 8) || !Readable(d, 8ull * n)) continue;
        bool all = true;
        for (u32 i = 0; i < n && all; i++) all = IsEffect(d[i]);
        if (!all) continue;
        if (!g_effectsOff) { g_effectsOff = o; Debug("magic: spell effects at +%x", o); }
        *data = reinterpret_cast<void**>(static_cast<u8*>(spell) + o);
        *size = reinterpret_cast<u32*>(static_cast<u8*>(spell) + o + 0xC);
        return true;
    }
    return false;
}

/* an effect that brings an actor (summon, raise dead...): its magic effect points at an NPC */
bool BringsActor(const void* effect) {
    const void* mgef = At<void*>(effect, 0x10);
    for (u32 o = 0x30; o < 0x130; o += 8) {
        const void* f = At<void*>(mgef, o);
        if (Readable(f, 0x20) && Type(f) == NPC_) return true;
    }
    return false;
}

/* (07.10, Eden: a friend's copies made Serana and the NPCs they hit fight the friend's puppet) the copy's magic effects
   are duplicates that are not hostile, not detrimental, cause no hit event, pain or hit shader: nobody takes them as
   an attack. EffectSetting data (flags u32, base cost f32, assoc form, skill i32, resist i32) at +0x68 on PC SE; the
   offset is checked by content once. */
constexpr u32 MGEF_HOSTILE = 1u << 0, MGEF_DETRIMENTAL = 1u << 2, MGEF_NO_HIT_EVENT = 1u << 4, MGEF_PAINLESS = 1u << 26,
              MGEF_NO_HIT_EFFECT = 1u << 27;
int g_mgefData = -2;
struct MgefCopy { void* src; void* copy; };
MgefCopy g_mgefCopies[96];
int g_mgefCopyN;

bool MgefDataAt(const u8* m, u32 o) {
    if (!Readable(m + o, 0x20)) return false;
    float cost = *reinterpret_cast<const float*>(m + o + 4);
    s32 skill = *reinterpret_cast<const s32*>(m + o + 0x10), resist = *reinterpret_cast<const s32*>(m + o + 0x14);
    return cost >= 0.0f && cost < 100000.0f && skill >= -1 && skill < 200 && resist >= -1 && resist < 200 &&
           (*reinterpret_cast<const u32*>(m + o) >> 30) == 0;
}

void* Peaceful(void* mgef) {
    if (!mgef) return mgef;
    for (int i = 0; i < g_mgefCopyN; i++) if (g_mgefCopies[i].src == mgef) return g_mgefCopies[i].copy;
    auto m = static_cast<u8*>(mgef);
    if (g_mgefData == -2) {
        g_mgefData = MgefDataAt(m, 0x68) ? 0x68 : -1;
        Debug("magic: effect data at %s", g_mgefData > 0 ? "+68" : "? (not changed)");
    }
    if (g_mgefData < 0 || g_mgefCopyN >= 96) return mgef;
    auto vt = *reinterpret_cast<uintptr_t**>(mgef);
    void* copy = reinterpret_cast<void* (*)(void*, bool, void*)>(vt[VT_DUPLICATE])(mgef, false, nullptr);
    if (!copy || !MgefDataAt(static_cast<u8*>(copy), (u32)g_mgefData)) {
        Debug("magic: effect %08x not duplicated", Id(mgef));
        g_mgefCopies[g_mgefCopyN++] = MgefCopy{mgef, mgef};
        return mgef;
    }
    u32& fl = *reinterpret_cast<u32*>(static_cast<u8*>(copy) + g_mgefData);
    fl = (fl & ~(MGEF_HOSTILE | MGEF_DETRIMENTAL)) | MGEF_NO_HIT_EVENT | MGEF_PAINLESS | MGEF_NO_HIT_EFFECT;
    g_mgefCopies[g_mgefCopyN++] = MgefCopy{mgef, copy};
    return copy;
}

struct Copy { u32 id; void* form; bool refused; };
Copy g_copies[64];
int g_copyN;

void* Harmless(void* spell) {
    u32 id = Id(spell);
    for (int i = 0; i < g_copyN; i++) if (g_copies[i].id == id) return g_copies[i].refused ? nullptr : g_copies[i].form;
    if (g_copyN >= 64) return nullptr;
    Copy& c = g_copies[g_copyN++];
    c = Copy{id, nullptr, true};
    void** srcData; u32* srcSize;
    if (!FindEffects(spell, &srcData, &srcSize)) { Debug("magic: %08x has no effects found", id); return nullptr; }
    void** effects = static_cast<void**>(*srcData);
    u32 n = *srcSize;
    for (u32 i = 0; i < n; i++)
        if (BringsActor(effects[i])) { Debug("magic: %08x brings an actor - not repeated", id); return nullptr; }
    auto vt = *reinterpret_cast<uintptr_t**>(spell);
    void* copy = reinterpret_cast<void* (*)(void*, bool, void*)>(vt[VT_DUPLICATE])(spell, false, nullptr);
    if (!copy) return nullptr;
    void** dstData; u32* dstSize;
    if (!FindEffects(copy, &dstData, &dstSize)) return nullptr;
    /* own array and own effects: the copy may share the original's */
    auto arr = static_cast<void**>(game::GameAlloc(8ull * n));
    if (!arr) return nullptr;
    for (u32 i = 0; i < n; i++) {
        auto e = static_cast<u8*>(game::GameAlloc(0x28));
        if (!e) return nullptr;
        memcpy(e, effects[i], 0x28);
        *reinterpret_cast<float*>(e + 0) = 0.0f;       // magnitude
        /* (07.10, his: "бафы ... должны в онлайне работать") a friend's buff on themselves (Oakflesh, invisibility, muffle,
           ward, a heal's glow) shows on their puppet for its whole time: self delivery, not hostile, not a cloak (its
           area spell would hit the NPCs around) — duration kept, no power (magnitude 0), and it may apply */
        bool selfBuff = false;
        if (g_mgefData == -2) if (auto m0 = At<u8*>(e, 0x10)) g_mgefData = MgefDataAt(m0, 0x68) ? 0x68 : -1;
        if (g_mgefData > 0)
            if (auto m = At<u8*>(e, 0x10)) {
                const u8* dd = m + g_mgefData;
                u32 fl = At<u32>(dd, 0), arche = At<u32>(dd, 0x58), delivery = At<u32>(dd, 0x74);
                selfBuff = delivery == 0 && !(fl & (MGEF_HOSTILE | MGEF_DETRIMENTAL)) && arche != 23 /* cloak */ &&
                           arche != 1 /* script */ && arche != 18 && arche != 33;   /* summon / reanimate */
            }
        if (!selfBuff) *reinterpret_cast<u32*>(e + 8) = 0;   // duration
        /* (07.10, Eden: a friend's copy still set the player on fire and Unrelenting Force knocked him down, PvP off)
           the effect applies to nobody: its conditions are one item that is never true, GetRandomPercent < -1
           (TESConditionItem 0x38: next, comparison float +8, function u16 +0x18, flags +0x30 = opcode << 5).
           The projectile, its light and impact still show. */
        *reinterpret_cast<void**>(e + 0x10) = Peaceful(*reinterpret_cast<void**>(e + 0x10));
        if (selfBuff) { arr[i] = e; continue; }        // a buff: it applies (to the puppet only), power 0
        if (auto c = static_cast<u8*>(game::GameAlloc(0x38))) {
            memset(c, 0, 0x38);
            *reinterpret_cast<float*>(c + 0x08) = -1.0f;
            *reinterpret_cast<u16*>(c + 0x18) = 77;    // GetRandomPercent
            c[0x30] = 4 << 5;                          // kLessThan, AND
            *reinterpret_cast<void**>(e + 0x20) = c;
        }
        arr[i] = e;
    }
    *dstData = arr;
    *dstSize = n;
    *(dstSize - 1) = n;                                  // capacity
    c.form = copy;
    c.refused = false;
    Debug("magic: harmless copy of %08x (%u effects)", id, n);
    return copy;
}

/* the damage field of weapons (u16) and ammo (float): the offset where two known forms hold their known values */
void FindDamageOffsets() {
    static bool tried;
    if (tried) return;
    tried = true;
    auto hb = static_cast<u8*>(equip::Lookup(HUNTING_BOW)), lb = static_cast<u8*>(equip::Lookup(LONG_BOW));
    auto ia = static_cast<u8*>(equip::Lookup(IRON_ARROW)), sa = static_cast<u8*>(equip::Lookup(STEEL_ARROW));
    if (hb && lb && Type(hb) == WEAP && Type(lb) == WEAP)
        for (u32 o = 0x20; o < 0x200 && !g_weapDamageOff; o += 2)
            if (At<u16>(hb, o) == 7 && At<u16>(lb, o) == 6) g_weapDamageOff = o;
    if (ia && sa && Type(ia) == AMMO && Type(sa) == AMMO)
        for (u32 o = 0x20; o < 0x200 && !g_ammoDamageOff; o += 4)
            if (At<float>(ia, o) == 8.0f && At<float>(sa, o) == 10.0f) g_ammoDamageOff = o;
    Debug("magic: weapon damage +%x, ammo damage +%x", g_weapDamageOff, g_ammoDamageOff);
}

Copy g_arms[32];
int g_armN;

/* a copy of a bow / arrow with no damage (the real hit comes from the shooter's console) */
void* NoDamage(void* form) {
    u32 id = Id(form);
    for (int i = 0; i < g_armN; i++) if (g_arms[i].id == id) return g_arms[i].form;
    FindDamageOffsets();
    u32 off = Type(form) == WEAP ? g_weapDamageOff : Type(form) == AMMO ? g_ammoDamageOff : 0;
    if (!off || g_armN >= 32) return nullptr;
    auto vt = *reinterpret_cast<uintptr_t**>(form);
    auto copy = static_cast<u8*>(reinterpret_cast<void* (*)(void*, bool, void*)>(vt[VT_DUPLICATE])(form, false, nullptr));
    if (!copy) return nullptr;
    if (Type(form) == WEAP) *reinterpret_cast<u16*>(copy + off) = 0;
    else *reinterpret_cast<float*>(copy + off) = 0.0f;
    g_arms[g_armN++] = Copy{id, copy, false};
    return copy;
}

/* The player's ActorMagicCaster objects (1.1.177 vtable 0x1a1b4d8), found in the actor by their vtable. Their state
   is watched every frame: graph events do not reach our hook in first person, the casters change in any view. */
constexpr u32 CASTER_VTABLE = 0x1a1b4d8;
u32 g_casterOff[8];
int g_casterN = -1;

/* ActorMagicCaster::SpellCast(caster, bool doCast, ...) (vtable slot 9, 1.1.177: 0x5692e0): the moment a spell,
   power or shout really leaves the caster, in any camera. Caster +0x20 = the spell being cast, +0xEC = source
   (0 left hand, 1 right hand, 2 voice). Queued here, sent from Pump (no calls in the hook after Orig). */
constexpr Fn SPELL_CAST{0x5692e0, {0xA9BE7BFD, 0xA9014FF4, 0x910003FD, 0xAA0003F3}};
struct Cast { u32 source, spell; };
Cast g_casts[16];
volatile u32 g_castHead, g_castTail;
void* volatile g_playerCasters[4];
HOOK_DEFINE_TRAMPOLINE(SpellCastHook) {
    static void Callback(void* caster, bool doCast, u64 a2, u64 a3) {
        void* spell = caster ? At<void*>(caster, 0x20) : nullptr;
        u32 source = caster ? At<u32>(caster, 0xEC) : 0;
        Orig(caster, doCast, a2, a3);
        if (!doCast || !spell) return;
        bool mine = false;
        for (void* c : g_playerCasters) mine |= c == caster;
        if (!mine) return;
        u32 h = g_castHead;
        if (h - g_castTail >= 16) return;
        g_casts[h % 16] = Cast{source, At<u32>(spell, 0x14)};
        __atomic_store_n(&g_castHead, h + 1, __ATOMIC_RELEASE);
    }
};

}  // namespace

void Pump() {
    void* player = game::Player();
    if (!g_ok || !player) return;
    if (g_casterN < 0) {
        g_casterN = 0;
        uintptr_t vt = game::ModuleAddr(CASTER_VTABLE);
        for (u32 o = 0x40; o < 0x600 && g_casterN < 8; o += 8) {
            void* c = At<void*>(player, o);
            if (Readable(c, 0x60) && At<uintptr_t>(c, 0) == vt) { g_casterOff[g_casterN++] = o; Debug("magic: caster at player +%x", o); }
        }
    }
    for (int i = 0; i < g_casterN && i < 4; i++) g_playerCasters[i] = At<void*>(player, g_casterOff[i]);
    /* shouts do not go through SpellCast: the voice caster's +0x38 goes from 0 to the shout's time when it leaves */
    static float voicePrev;
    for (int i = 0; i < g_casterN; i++) {
        auto c = At<u8*>(player, g_casterOff[i]);
        if (!c || At<u32>(c, 0xEC) != 2) continue;
        float v = At<float>(c, 0x38);
        if (voicePrev == 0.0f && v > 0.0f) { Shouted(); Debug("magic: the player shouted (%.2f)", v); }
        voicePrev = v;
    }
    /* arrows: the worn ammo's count goes down by one while a bow is worn (graph events miss first person) */
    static u64 nextAmmo;
    static u32 ammoId;
    static s32 ammoDelta;
    u64 nowMs = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    if (g_fire && nowMs >= nextAmmo) {
        nextAmmo = nowMs + 100;
        const equip::Item* items; int n;
        u32 bow = 0, ammo = 0;
        if (equip::Worn(player, &items, &n))
            for (int i = 0; i < n; i++) {
                void* f = equip::Lookup(items[i].formId);
                if (f && Type(f) == WEAP && !bow) bow = items[i].formId;
                if (f && Type(f) == AMMO) ammo = items[i].formId;
            }
        if (bow && ammo) {
            trade::Count ch[128];
            int cn = trade::Changes(player, ch, 128);
            s32 d = 0;
            for (int i = 0; i < cn; i++) if (ch[i].id == ammo) d = ch[i].n;
            if (ammo == ammoId && d < ammoDelta) { Shot(); Debug("magic: the player shot %08x", ammo); }
            ammoId = ammo;
            ammoDelta = d;
        } else {
            ammoId = 0;
        }
    }
    while (g_castTail != __atomic_load_n(&g_castHead, __ATOMIC_ACQUIRE)) {
        Cast c = g_casts[g_castTail % 16];
        __atomic_store_n(&g_castTail, g_castTail + 1, __ATOMIC_RELEASE);
        char ev[40];
        snprintf(ev, sizeof ev, "coopcast %u %08x", c.source > 2 ? 2 : c.source, c.spell);
        PushEvent(ev);
        Debug("magic: the player cast %08x (source %u)", c.spell, c.source);
    }
}

bool Resolve() {
    g_cast = game::Checked177(CAST.off, CAST.sig);
    if (uintptr_t sc = game::Checked177(SPELL_CAST.off, SPELL_CAST.sig)) SpellCastHook::InstallAtPtr(sc);
    g_fire = game::Checked177(FIRE.off, FIRE.sig);
    g_ok = g_cast != 0;
    return g_ok;
}

/* (07.10, his: "смогу ли я товарища похилить") a restore-health spell of ours reaches the friend for real: our copy
   on their puppet is harmless, so the health goes as "coopheal <player> <amount>". Aimed / touch: the nearest friend
   within 2000 units; self-area (Healing Circle): every friend within the area */
void HealFriends(void* player, void* spell) {
    void** data; u32* size;
    if (!FindEffects(spell, &data, &size)) return;

    float amount = 0.0f, area = 0.0f; bool self = false;
    auto effects = static_cast<void**>(*data);
    for (u32 i = 0; i < *size; i++) {
        auto e = static_cast<u8*>(effects[i]);
        auto m = At<u8*>(e, 0x10);
        if (!m) continue;
        if (g_mgefData < 0) { g_mgefData = MgefDataAt(m, 0x68) ? 0x68 : -1; if (g_mgefData < 0) return; }
        const u8* dd = m + g_mgefData;
        u32 flags = At<u32>(dd, 0), arche = At<u32>(dd, 0x58), av = At<u32>(dd, 0x5C), delivery = At<u32>(dd, 0x74);
        if (av != 24 || (arche != 0 && arche != 34) || (flags & (MGEF_HOSTILE | MGEF_DETRIMENTAL))) continue;
        float mag = At<float>(e, 0); u32 dur = At<u32>(e, 8);
        amount += mag * (dur > 1 ? (float)dur : 1.0f);
        if (delivery == 0) { self = true; if ((float)At<u32>(e, 4) > area) area = (float)At<u32>(e, 4); }
    }
    if (amount < 1.0f) return;
    if (self && area < 1.0f) return;                       // a heal on oneself only
    const float* me = game::RefPos(player);
    float reach = self ? area * 22.0f + 200.0f : 2000.0f;
    RemotePlayer rs[MAX_REMOTE];
    int n = SnapshotRemotes(rs, MAX_REMOTE), best = -1; float bd = 0;
    for (int i = 0; i < n; i++) {
        float dx = rs[i].state.x - me[0], dy = rs[i].state.y - me[1], d = dx * dx + dy * dy;
        if (d > reach * reach) continue;
        if (self) { char ev[40]; snprintf(ev, sizeof ev, "coopheal %u %u", rs[i].state.id, (u32)amount); PushEvent(ev); continue; }
        if (best < 0 || d < bd) { best = i; bd = d; }
    }
    if (best >= 0) { char ev[40]; snprintf(ev, sizeof ev, "coopheal %u %u", rs[best].state.id, (u32)amount); PushEvent(ev); }
    Debug("magic: heal %08x %.0f (%s)", Id(spell), amount, self ? "area" : best >= 0 ? "friend" : "nobody");
}

void Released(u32 hand) {
    void* player = game::Player();
    if (!g_ok || !player || hand > 1) return;
    void* spell = At<void*>(player, SELECTED_SPELLS + hand * 8);
    if (!spell || Type(spell) != SPEL) return;
    char ev[40];
    snprintf(ev, sizeof ev, "coopcast %u %08x", hand, Id(spell));
    PushEvent(ev);
    HealFriends(player, spell);
}

void CastOn(void* puppet, u32 hand, u32 spellId) {
    if (!g_ok || !puppet) return;
    void* spell = equip::Lookup(spellId);
    if (!spell || Type(spell) != SPEL) { Debug("magic: %08x is not a spell here (%p type %02x)", spellId, spell, spell ? Type(spell) : 0); return; }
    void* copy = Harmless(spell);
    if (!copy) { Debug("magic: %08x has no harmless copy", spellId); return; }
    reinterpret_cast<bool (*)(void*, u32, void*, void*, void*)>(g_cast)(game::FakeVm(), 0, copy, puppet, nullptr);
    Debug("magic: puppet casts %08x (%s hand)", spellId, hand ? "right" : "left");
}

/* TESShout: three variations {TESWordOfPower* word, SpellItem* spell, float recovery} found by content; the
   strongest one whose word is unlocked (form flag 0x10000) is what was shouted (the charge is not known here) */
void Shouted() {
    void* player = game::Player();
    if (!g_ok || !player) return;
    void* shout = At<void*>(player, SELECTED_SPELLS + 2 * 8);
    if (!shout || Type(shout) != 0x77) return;
    void* best = nullptr;
    for (u32 o = 0x20; o + 0x10 <= 0x120; o += 8) {
        void* w = At<void*>(shout, o);
        void* sp = At<void*>(shout, o + 8);
        if (!Readable(w, 0x20) || !Readable(sp, 0x20) || Type(w) != 0x76 || Type(sp) != SPEL) continue;
        if (!best || (At<u32>(w, 0x10) & 0x10000)) best = sp;
    }
    if (!best) return;
    char ev[40];
    snprintf(ev, sizeof ev, "coopcast 2 %08x", Id(best));
    PushEvent(ev);
    Debug("magic: shout spell %08x goes out", Id(best));
}

/* the local player let an arrow go: the bow and the arrow worn now */
void Shot() {
    void* player = game::Player();
    if (!g_ok || !g_fire || !player) return;
    u32 bow = 0, arrow = 0;
    const equip::Item* items; int n;
    if (!equip::Worn(player, &items, &n)) return;
    for (int i = 0; i < n; i++) {
        void* f = equip::Lookup(items[i].formId);
        if (!f || items[i].left > 1) continue;
        if (Type(f) == WEAP && !bow) bow = items[i].formId;
        if (Type(f) == AMMO) arrow = items[i].formId;
    }
    if (!bow || !arrow) return;
    char ev[40];
    snprintf(ev, sizeof ev, "cooparrow %08x %08x", bow, arrow);
    PushEvent(ev);
}

void FireOn(void* puppet, u32 bowId, u32 arrowId) {
    if (!g_ok || !g_fire || !puppet) return;
    void* bow = equip::Lookup(bowId);
    void* arrow = equip::Lookup(arrowId);
    if (!bow || !arrow || Type(bow) != WEAP || Type(arrow) != AMMO) return;
    void* b = NoDamage(bow);
    void* a = NoDamage(arrow);
    if (!b || !a) return;
    reinterpret_cast<void (*)(void*, u32, void*, void*, void*)>(g_fire)(game::FakeVm(), 0, b, puppet, a);
    Debug("magic: puppet shoots %08x / %08x", bowId, arrowId);
}

void TestFire(void* source, u32 bowId, u32 arrowId, bool copy) {
    void* bow = equip::Lookup(bowId);
    void* arrow = equip::Lookup(arrowId);
    if (!g_fire || !source || !bow || !arrow) return;
    if (copy) { bow = NoDamage(bow); arrow = NoDamage(arrow); }
    if (!bow || !arrow) return;
    reinterpret_cast<void (*)(void*, u32, void*, void*, void*)>(g_fire)(game::FakeVm(), 0, bow, source, arrow);
    Debug("magic: test shot %08x / %08x%s", bowId, arrowId, copy ? " (copies)" : "");
}

}  // namespace coop::magic
