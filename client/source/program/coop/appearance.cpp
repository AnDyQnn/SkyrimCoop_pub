#include "appearance.hpp"
#include "client.hpp"
#include "equip.hpp"
#include "game.hpp"
#include "lib.hpp"
#include <nn/os.hpp>
#include <cstring>

namespace coop::appearance {
namespace {

struct Fn { u32 off; u32 sig[4]; };
/* 1.1.177 (tools/xrefs.py, notes/plan.md):
     BGSSaveFormBuffer ctor 0x5c4b2c: vtable, buffer +8, capacity +0x10, position +0x14, changeFlags +0x1B
     BGSLoadFormBuffer: base ctor 0x5c2970, sub-object ctor 0x5c2438(+0x28), vtable = *GOT 0x2145730 + 0x10;
       buffer +8, capacity +0x10 (u64), unk1C +0x1C, size1 +0x20, position +0x24, formId +0x28, size2 +0x2C,
       form +0x38, changeFlags +0x40, flags2 +0x44, loadFlag +0x4B
     WriteFormId 0x5c5264(buf, TESForm*), ReadFormId 0x5c2d38(buf, u32* out) -> bool error
     WriteData 0x5c5494(buf, const void*, u32), ReadData 0x5c31e4(buf, void*, u32)
     TESForm vtable: CreateDuplicateForm [10], SaveGame [15], LoadGame [16] */
constexpr Fn SAVE_CTOR{0x5c4b2c, {0xA9BE7BFD, 0xF9000BF3, 0x910003FD, 0xAA0003F3}};
constexpr Fn LOAD_BASE_CTOR{0x5c2970, {0x9000DC88, 0xF9407908, 0xF900081F, 0xB900181F}};
constexpr Fn LOAD_SUB_CTOR{0x5c2438, {0xF900001F, 0xB900081F, 0xA9017C1F, 0xB801F01F}};
constexpr Fn WRITE_FORM_ID{0x5c5264, {0xA9BD7BFD, 0xF9000BF5, 0x910003FD, 0xA9024FF4}};
constexpr Fn READ_FORM_ID{0x5c2d38, {0xA9BE7BFD, 0xF9000BF3, 0x910003FD, 0x790033BF}};
constexpr Fn WRITE_DATA{0x5c5494, {0xA9BC7BFD, 0xF9000BF7, 0x910003FD, 0xA90257F6}};
constexpr Fn READ_DATA{0x5c31e4, {0xA9BE7BFD, 0xA9014FF4, 0x910003FD, 0xF9400409}};
constexpr u32 LOAD_VTABLE_GOT = 0x2145730;
/* ActorBase.SetEssential native (vm, stack, base, bool): ACBS flags at base+0x28, no VM use. The copy is made
   essential so that the local world cannot kill another player's puppet. */
constexpr Fn SET_ESSENTIAL{0xa44074, {0xA9BE7BFD, 0xA9014FF4, 0x910003FD, 0x2A0303F4}};
uintptr_t g_setEssential, g_fsCtor, g_fsDtor;
constexpr Fn FS_CTOR{0xae6da8, {0xA9BE7BFD, 0xF9000BF3, 0x910003FD, 0xF900001F}};
constexpr u32 VT_DUPLICATE = 10, VT_SAVE = 15, VT_LOAD = 16;
constexpr u32 FLAGS = 0x2u | 0x20u | 0x800u | 0x1000000u | 0x2000000u;   // base data, name, face, gender, race

uintptr_t g_saveCtor, g_loadBaseCtor, g_loadSubCtor, g_writeFormId, g_readFormId, g_writeData, g_readData;
bool g_ok;

/* raw FormIDs only for our own Save/Load calls, on this thread */
volatile bool g_raw;
nn::os::ThreadType* g_rawThread;
bool Raw() { return g_raw && nn::os::GetCurrentThread() == g_rawThread; }

HOOK_DEFINE_TRAMPOLINE(WriteFormIdHook) {
    static void Callback(void* buf, void* form) {
        if (!Raw()) { Orig(buf, form); return; }
        u32 id = form ? *reinterpret_cast<u32*>(static_cast<u8*>(form) + 0x14) : 0;
        reinterpret_cast<void (*)(void*, const void*, u32)>(g_writeData)(buf, &id, 4);
    }
};
HOOK_DEFINE_TRAMPOLINE(ReadFormIdHook) {
    static bool Callback(void* buf, u32* out) {
        if (!Raw()) return Orig(buf, out);
        u32 id = 0;
        reinterpret_cast<void (*)(void*, void*, u32)>(g_readData)(buf, &id, 4);
        *out = id;
        return false;                    // "no error"
    }
};

void* VCall(void* obj, u32 slot) { return reinterpret_cast<void*>((*reinterpret_cast<uintptr_t**>(obj))[slot]); }

/* ---- sending */
constexpr u32 MAX = 16384;
alignas(16) u8 g_saveBuf[MAX];
alignas(16) u8 g_out[MAX];
u32 g_outSize, g_lastHash;
u64 g_lastCollect;

u32 Hash(const u8* p, u32 n) { u32 h = 2166136261u; for (u32 i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u; return h; }

/* ---- receiving */
struct Entry { u16 id; bool dirty; u32 flags; u32 size; u8 data[MAX]; void* base; };
Entry g_entries[4];
nn::os::MutexType g_lock;
bool g_lockReady;
void Lock() { if (!g_lockReady) { nn::os::InitializeMutex(&g_lock, false, 0); g_lockReady = true; } nn::os::LockMutex(&g_lock); }
void Unlock() { nn::os::UnlockMutex(&g_lock); }

/* TESForm::LoadGame of those bytes into `npc` (raw FormIDs, see the hooks above). */
void LoadInto(void* npc, const u8* data, u32 size, u32 flags) {
    alignas(16) u8 lb[0x100] = {};
    reinterpret_cast<void (*)(void*)>(g_loadBaseCtor)(lb);
    reinterpret_cast<void (*)(void*)>(g_loadSubCtor)(lb + 0x28);
    *reinterpret_cast<uintptr_t*>(lb) = *reinterpret_cast<uintptr_t*>(game::ModuleAddr(LOAD_VTABLE_GOT)) + 0x10;
    *reinterpret_cast<const void**>(lb + 0x08) = data;
    *reinterpret_cast<u64*>(lb + 0x10) = size;
    *reinterpret_cast<s32*>(lb + 0x1C) = -1;
    *reinterpret_cast<u32*>(lb + 0x20) = size;
    *reinterpret_cast<u32*>(lb + 0x24) = 0;
    *reinterpret_cast<u32*>(lb + 0x28) = *reinterpret_cast<u32*>(static_cast<u8*>(npc) + 0x14);
    *reinterpret_cast<u32*>(lb + 0x2C) = size;
    *reinterpret_cast<void**>(lb + 0x38) = npc;
    *reinterpret_cast<u32*>(lb + 0x40) = flags;
    *reinterpret_cast<u32*>(lb + 0x44) = 0;
    lb[0x4B] = 0x40;
    g_rawThread = nn::os::GetCurrentThread();
    g_raw = true;
    reinterpret_cast<void (*)(void*, void*)>(VCall(npc, VT_LOAD))(npc, lb);
    g_raw = false;
    *reinterpret_cast<const void**>(lb + 0x08) = nullptr;     // our bytes, not the game's heap
}

/* TESForm::SaveGame of the player's NPC base with these change flags; 0 = failed. */
u32 SaveBase(u8* out, u32 max, u32 flags) {
    void* player = game::Player();
    if (!player) return 0;
    void* base = *reinterpret_cast<void**>(static_cast<u8*>(player) + 0x40);
    alignas(16) u8 sb[0x100] = {};
    reinterpret_cast<void (*)(void*)>(g_saveCtor)(sb);
    *reinterpret_cast<void**>(sb + 0x08) = out;
    *reinterpret_cast<u32*>(sb + 0x10) = max;
    *reinterpret_cast<u32*>(sb + 0x14) = 0;
    memcpy(sb + 0x1B, &flags, 4);
    g_rawThread = nn::os::GetCurrentThread();
    g_raw = true;
    reinterpret_cast<void (*)(void*, void*)>(VCall(base, VT_SAVE))(base, sb);
    g_raw = false;
    u32 n = *reinterpret_cast<u32*>(sb + 0x14);
    bool grown = *reinterpret_cast<void**>(sb + 0x08) != out;   // the game reallocated: don't trust it
    *reinterpret_cast<void**>(sb + 0x08) = nullptr;
    return grown || n > max ? 0 : n;
}

/* TESNPC head parts (PC SE: +0x238 array, +0x240 count; the Switch layout is shifted — the race is at +0x148, not
   +0x150): found once on the player's base by content — an array of head part forms (type 0x0C) with its count after */
int g_headOff = -2;
int HeadOff(void* npc) {
    if (g_headOff != -2) return g_headOff;
    auto b = static_cast<u8*>(npc);
    g_headOff = -1;
    for (int o = 0x200; o <= 0x260 && g_headOff < 0; o += 8) {
        if (!game::Readable(b + o, 9)) continue;
        void** arr = *reinterpret_cast<void***>(b + o);
        s8 n = *reinterpret_cast<s8*>(b + o + 8);
        if (!arr || n < 1 || n > 32 || !game::Readable(arr, 8u * n)) continue;
        bool all = true;
        for (int i = 0; i < n && all; i++) all = arr[i] && game::Readable(arr[i], 0x20) && static_cast<u8*>(arr[i])[0x1A] == 0x0C;
        if (all) g_headOff = o;
    }
    Log("внешность: части головы в NPC на %s%x", g_headOff > 0 ? "+" : "? ", g_headOff > 0 ? g_headOff : 0);
    return g_headOff;
}

uintptr_t g_create;                                  // CreateFormOfType (0xaff78)

void* Build(const u8* data, u32 size, u32 flags) {
    void* player = game::Player();
    if (!player) return nullptr;
    u32 headIds[32]; u32 headN = 0;
    if (size > 12) {
        u32 tail; memcpy(&tail, data + size - 4, 4);
        if (tail >= 8 && tail + 4 <= size && !memcmp(data + size - 4 - tail, "HDPT", 4)) {
            const u8* t = data + size - 4 - tail;
            memcpy(&headN, t + 4, 4);
            if (headN > 32 || 8 + 4 * headN != tail) headN = 0;
            memcpy(headIds, t + 8, 4 * headN);
            size -= tail + 4;
        }
    }
    void* mine = *reinterpret_cast<void**>(static_cast<u8*>(player) + 0x40);
    /* (08.10, Eden: the render crash 0xbf9940 <- 0xd91f34 <- 0xdb5b68 every few puppets; none with the player's own base)
       not a Duplicate of our base — its Copy shares our heap blocks, and the puppet's temporary base, deleted with the
       puppet, freed what our own character still draws from. The Skyrim Together way (TESNPC::Create / Initialize): a
       fresh NPC of the form factory (0xaff78, CreateFormOfType — what Duplicate itself calls), our base's links to other
       forms (race, class, outfits, skin, combat style...) and the ref-counted attack data, then their bytes loaded */
    static const u32 CREATE_SIG[4] = {0xA9BD7BFD, 0xA90157F6, 0x910003FD, 0xA9024FF4};
    if (!g_create) g_create = game::Checked177(0xaff78, CREATE_SIG);   // (no function-local static: its guard hung here)
    uintptr_t create = g_create;
    if (!create) return nullptr;
    void* npc = reinterpret_cast<void* (*)(u32)>(create)(0x2B);
    if (!npc) return nullptr;
    {
        uintptr_t lo = game::ModuleAddr(0), hi = game::ModuleAddr(0x3400000);
        auto src = static_cast<u8*>(mine), dst = static_cast<u8*>(npc);
        int forms = 0;
        for (u32 o = 0x18; o < 0x258; o += 8) {
            if (o == 0xD0) continue;                                   // the name (a BSFixedString) comes with their bytes
            u64 a = *reinterpret_cast<u64*>(src + o);
            if (a < 0x10000 || (a >= lo && a < hi) || !game::Readable(reinterpret_cast<void*>(a), 0x20)) continue;
            auto p = reinterpret_cast<u8*>(a);
            u32 id = *reinterpret_cast<u32*>(p + 0x14);
            if (id && p[0x1A] < 0x80 && equip::Lookup(id) == p) {       // a real form: a link, safe to share
                *reinterpret_cast<u64*>(dst + o) = a; forms++;
                continue;
            }
            u64 vt = *reinterpret_cast<u64*>(p);
            u32* rc = reinterpret_cast<u32*>(p + 8);
            if (o == 0x120 && vt >= lo && vt < hi && *rc >= 1 && *rc < 100000) {   // the attack data: our own reference
                __atomic_add_fetch(rc, 1u, __ATOMIC_ACQ_REL);
                *reinterpret_cast<u64*>(dst + o) = a;
            }
        }
        /* (08.10, Eden: the puppet walked in a T-pose) the plain numbers of TESActorBaseData (Switch +0x28; its
           ACTOR_BASE_DATA at +0x30: flags, magicka / stamina offsets, level, calc levels, speed multiplier,
           disposition, template flags, health offset, bleedout) — a fresh form has them all zero */
        memcpy(dst + 0x30, src + 0x30, 0x18);
        Debug("внешность: новая база %08x, ссылок на формы %d, скорость %u", game::RefFormId(npc), forms,
              *reinterpret_cast<u16*>(dst + 0x3E));
    }
    LoadInto(npc, data, size, flags);
    /* (07.10, his Khajiit / her elf: each saw the other with their OWN race's head) the copy is made from OUR base and
       keeps our face template (TESNPC::faceNPC +0x1F0): the head is drawn from it. Their face is their own */
    if (headN && HeadOff(npc) > 0) {
        auto arr = static_cast<void**>(game::GameAlloc(8ull * headN));
        u32 k = 0;
        for (u32 i = 0; arr && i < headN; i++) if (void* hp = equip::Lookup(headIds[i])) arr[k++] = hp;
        if (arr && k) {
            *reinterpret_cast<void***>(static_cast<u8*>(npc) + HeadOff(npc)) = arr;
            *reinterpret_cast<s8*>(static_cast<u8*>(npc) + HeadOff(npc) + 8) = (s8)k;
        }
    }
    if (g_setEssential) reinterpret_cast<void (*)(void*, u32, void*, bool)>(g_setEssential)(nullptr, 0, npc, true);
    /* a player's puppet must not speak NPC lines ("Эй, урод!" to whoever stands next to it): TESActorBaseData
       (+0x30) keeps the voice type at +0x28; SwitchPatch's SSWSilentVoice (0xF30) has no dialogue at all */
    {
        int idx = game::PluginIndex("SwitchPatch.esp");
        void* silent = idx >= 0 ? equip::Lookup((u32(idx) << 24) | 0xF30) : nullptr;
        /* (08.10: "was type 00" — +0x58 is not the voice here) the field is found once on our own base by content: a
           pointer to a VTYP form (0x62) */
        static int voiceOff = -2;
        if (voiceOff == -2) {
            voiceOff = -1;
            for (int o2 = 0x30; o2 < 0x258 && voiceOff < 0; o2 += 8) {
                void* p = *reinterpret_cast<void**>(static_cast<u8*>(mine) + o2);
                if (p && game::Readable(p, 0x20) && static_cast<u8*>(p)[0x1A] == 0x62 && equip::Lookup(game::RefFormId(p)) == p) voiceOff = o2;
            }
            Log("внешность: голос в NPC на %s%x", voiceOff > 0 ? "+" : "? ", voiceOff > 0 ? voiceOff : 0);
        }
        if (voiceOff < 0) voiceOff = 0x58;
        void** vt = reinterpret_cast<void**>(static_cast<u8*>(npc) + voiceOff);
        u8 oldType = *vt ? static_cast<u8*>(*vt)[0x1A] : 0;
        if (silent) *vt = silent;                          // (07.10, his: "тела друг друга оскорбляют") always, whatever was there
        Debug("puppet voice: was type %02x, silent %p", oldType, silent);
    }
    /* TESContainer sits at +0x78 in TESNPC here (the secondary vtable with top -120 has TESContainer's slots;
       objects +0x80, count u32 +0x88): the copy starts with
       the player template's things (potions, gold, lockpicks); a puppet holds only what its player wears */
    u32* count = reinterpret_cast<u32*>(static_cast<u8*>(npc) + 0x88);
    Debug("puppet base container: %u objects", *count);
    if (*count < 256) *count = 0;
    return npc;
}

}  // namespace

bool Resolve() {
    auto check = [](const Fn& f) {
        if (!f.sig[0]) return game::ModuleAddr(f.off);          // prologue not pinned: trust the offset
        return game::Checked177(f.off, f.sig);
    };
    /* all or nothing, and only on the build whose other functions matched */
    if (!game::Checked177(SAVE_CTOR.off, SAVE_CTOR.sig)) return false;
    g_saveCtor = check(SAVE_CTOR); g_loadBaseCtor = check(LOAD_BASE_CTOR); g_loadSubCtor = check(LOAD_SUB_CTOR);
    g_writeFormId = check(WRITE_FORM_ID); g_readFormId = check(READ_FORM_ID);
    g_writeData = check(WRITE_DATA); g_readData = check(READ_DATA);
    g_setEssential = check(SET_ESSENTIAL);
    g_fsCtor = check(FS_CTOR);
    g_fsDtor = g_fsCtor ? game::ModuleAddr(0xae6ab0) : 0;
    g_ok = g_saveCtor && g_writeFormId && g_readFormId && g_readData;
    if (g_ok) {
        WriteFormIdHook::InstallAtPtr(g_writeFormId);
        ReadFormIdHook::InstallAtPtr(g_readFormId);
    }
    return g_ok;
}

bool Available() { return g_ok; }

bool Collect(const void** data, u32* size, u32* flags) {
    if (!g_ok) return false;
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    if (now - g_lastCollect < 5000) return false;
    g_lastCollect = now;
    u32 n = SaveBase(g_saveBuf, MAX - 256, FLAGS);
    if (n == 0) {
        static int reported;
        if (reported++ < 3) Log("appearance: the player base could not be saved");
        return false;
    }
    /* (07.10, Skyrim Together way) the head parts by FormID after the face data: "HDPT", count, ids (TESNPC +0x238 / +0x240)
       — the saved face carries no head parts, the puppet took them from our own base */
    {
        auto base = static_cast<u8*>(*reinterpret_cast<void**>(static_cast<u8*>(game::Player()) + 0x40));
        int ho = HeadOff(base);
        void** parts = ho > 0 ? *reinterpret_cast<void***>(base + ho) : nullptr;
        s8 np = ho > 0 ? *reinterpret_cast<s8*>(base + ho + 8) : 0;
        static int said;
        if (said++ < 2) Log("внешность: частей головы %d (%p)", (int)np, parts);
        if (parts && np > 0 && np <= 32 && game::Readable(parts, 8u * np)) {
            memcpy(g_saveBuf + n, "HDPT", 4); n += 4;
            u32 c = 0, at = n; n += 4;
            for (int i = 0; i < np; i++)
                if (parts[i] && game::Readable(parts[i], 0x20)) { u32 id = *reinterpret_cast<u32*>(static_cast<u8*>(parts[i]) + 0x14); memcpy(g_saveBuf + n, &id, 4); n += 4; c++; }
            memcpy(g_saveBuf + at, &c, 4);
            u32 tail = 4 + 4 + 4 * c; memcpy(g_saveBuf + n, &tail, 4); n += 4;   // its own length last: found from the end
        }
    }
    u32 h = Hash(g_saveBuf, n);
    if (h == g_lastHash) return false;
    g_lastHash = h;
    memcpy(g_out, g_saveBuf, n);
    g_outSize = n;
    *data = g_out; *size = n; *flags = FLAGS;
    Debug("appearance: %u bytes to send", n);
    return true;
}

void Received(u16 id, u32 flags, const void* data, u32 size) {
    if (size > MAX) return;
    Lock();
    Entry* e = nullptr;
    for (auto& x : g_entries) if (x.id == id) e = &x;
    for (auto& x : g_entries) if (!e && !x.id) e = &x;
    if (e) { e->id = id; e->flags = flags; e->size = size; memcpy(e->data, data, size); e->dirty = true; }
    Unlock();
}

void Pump() {
    if (!g_ok) return;
    for (auto& e : g_entries) {
        if (!e.id || !e.dirty) continue;
        Lock();
        e.dirty = false;
        Unlock();
        void* npc = Build(e.data, e.size, e.flags);
        /* the name over it: the player's name in the room (TESFullName at TESNPC+0xC8, BSFixedString +0xD0) */
        char pn[32];
        if (npc && g_fsCtor && PlayerName(e.id, pn, sizeof pn)) {
            void* fs = static_cast<u8*>(npc) + 0xD0;
            reinterpret_cast<void (*)(void*)>(g_fsDtor)(fs);
            reinterpret_cast<void (*)(void*, const char*)>(g_fsCtor)(fs, pn);
        }
        Debug("appearance of player %u: %s", e.id, npc ? "built" : "failed");
        if (npc) e.base = npc;
    }
}

u32 SavePlayer(u8* out, u32 max, u32 flags) { return g_ok ? SaveBase(out, max, flags) : 0; }

bool LoadPlayer(const u8* data, u32 size, u32 flags) {
    void* player = game::Player();
    if (!g_ok || !player) return false;
    LoadInto(*reinterpret_cast<void**>(static_cast<u8*>(player) + 0x40), data, size, flags);
    return true;
}

void SetEssential(void* base, bool on) {
    if (g_setEssential && base) reinterpret_cast<void (*)(void*, u32, void*, bool)>(g_setEssential)(nullptr, 0, base, on);
}

void* BaseFor(u16 id) {
    for (auto& e : g_entries) if (e.id == id) return e.base;
    return nullptr;
}

bool SetTitle(u16 id, const char* title) {
    if (!g_fsCtor || !g_fsDtor) return false;
    for (auto& e : g_entries)
        if (e.id == id && e.base) {
            void* fs = static_cast<u8*>(e.base) + 0xD0;             // TESFullName's BSFixedString (see Pump)
            reinterpret_cast<void (*)(void*)>(g_fsDtor)(fs);
            reinterpret_cast<void (*)(void*, const char*)>(g_fsCtor)(fs, title);
            return true;
        }
    return false;
}

void Forget(u16 id) {
    Lock();
    for (auto& e : g_entries) if (e.id == id) e = Entry{};
    Unlock();
}

}  // namespace coop::appearance
