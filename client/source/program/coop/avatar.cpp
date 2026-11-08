#include "avatar.hpp"
#include "appearance.hpp"
#include "client.hpp"
#include "equip.hpp"
#include "game.hpp"
#include "lib.hpp"
#include "quests.hpp"
#include <nn/os.hpp>
#include <cstdio>
#include <cstring>

#pragma GCC diagnostic ignored "-Wformat-truncation"

/* Avatar: the character as its own thing, like a Divinity / Palworld character that goes from world to world.
   Saved: the player NPC base (race, sex, face, name, level — TESForm::SaveGame as for the puppets), the skills
   and attributes (base actor values), perks, perk points, spells, shouts and words of power, and what the
   player carries. Put onto the current character: the base is loaded back (SetRace when the race differs,
   else the 3D is rebuilt like the SetRace native does), actor values set, missing perks / spells / shouts
   / words / things added. Nothing is taken away. 1.1.177, from the Papyrus natives (tools/xrefs.py):
     GetBaseActorValue 0x9fe3d0 / SetActorValue 0xa02a40 (vm, stack, actor, BSFixedString&, [float])
     HasPerk -> 0x653460(actor, perk); AddPerk -> actor vtable +0x820 (actor, perk, rank 0)
     HasSpell -> 0x673c7c(actor, spell) / shouts 0x6745c8; AddSpell -> 0x673ef4, AddShout -> 0x6745d4
     Game.TeachWord 0xa2bdb0 / UnlockWord 0xa2c0b0 (vm, stack, none, word); unlocked = word +0x12 bit 0
     GetItemCount 0xa50430 (vm, stack, ref, form); AddItem 0xa4e540 (vm, stack, ref, form, count, silent)
     perk points: PlayerCharacter +0xAB1 (AddPerkPoints native); actor race +0x228
     SetRace 0xa03b80; its 3D rebuild: 0x8eb48(*GOT 0x2144f30, 3D +0x60, 0) ? 0x6cae44(actor +0x128, 0x17),
       0x85b00(*GOT 0x2144f28, actor, 0, 0)
   Every form kind is walked through TESDataHandler's per-type arrays (found at run time). */
namespace coop::avatar {
namespace {

struct Fn { u32 off; u32 sig[4]; };
constexpr Fn GET_BASE_AV{0x9fe3d0, {0xD10783FF, 0xA91A7BFD, 0x910683FD, 0xF900DBFC}};
constexpr Fn SET_AV{0xa02a40, {0xFC1B0FE8, 0xA9017BFD, 0x910043FD, 0xA9025FFC}};
constexpr Fn HAS_PERK{0x653460, {0xAA0103E2, 0xAA0003E1, 0xF9409400, 0xB4000040}};
constexpr Fn HAS_SPELL{0x673c7c, {0xD100C3FF, 0xA9027BFD, 0x910083FD, 0xF9000BE1}};
constexpr Fn HAS_SHOUT{0x6745c8, {0xF9402008, 0x91024100, 0x17E9924B, 0xA9BE7BFD}};
constexpr Fn ADD_SPELL{0x673ef4, {0xD10183FF, 0xA9037BFD, 0x9100C3FD, 0xF90023F5}};
constexpr Fn ADD_SHOUT{0x6745d4, {0xA9BE7BFD, 0xA9014FF4, 0x910003FD, 0xF9402008}};
constexpr Fn TEACH_WORD{0xa2bdb0, {0xA9BB7BFD, 0xF9000BF9, 0x910003FD, 0xA9025FF8}};
constexpr Fn UNLOCK_WORD{0xa2c0b0, {0xA9BB7BFD, 0xA90167FA, 0x910003FD, 0xA9025FF8}};
constexpr Fn ITEM_COUNT{0xa50430, {0x2A0103E8, 0xAA0003E9, 0xAA0203E0, 0xAA0303E1}};
constexpr Fn ADD_ITEM{0xa4e540, {0xD10183FF, 0xA9017BFD, 0x910043FD, 0xA90267FA}};
constexpr Fn GET_CHANGES{0x3521c, {0xA9BD7BFD, 0xA90157F6, 0x910003FD, 0xA9024FF4}};
constexpr Fn SET_RACE{0xa03b80, {0xA9BE7BFD, 0xF9000BF3, 0x910003FD, 0xF9411448}};
constexpr Fn IS_3D_READY{0x8eb48, {0xB4000101, 0x39419028, 0x51000D09, 0x71000D3F}};
constexpr Fn RESET_PART{0x6cae44, {0xF9400408, 0xB4000088, 0x394C2509, 0x2A010129}};
constexpr Fn RESET_3D{0x85b00, {0xD10203FF, 0xA9027BFD, 0x910083FD, 0xA9036FFC}};
constexpr Fn FS_CTOR{0xae6da8, {0xA9BE7BFD, 0xF9000BF3, 0x910003FD, 0xF900001F}};
constexpr u32 GOT_3D_CHECK = 0x2144f30, GOT_3D_RESET = 0x2144f28;
constexpr u32 VT_ADD_PERK = 0x820, OFF_RACE = 0x228, OFF_PERK_POINTS = 0xAB1;
/* PlayerCharacter +0x968 = PlayerSkills* (PlayerCharacter::UseSkill, vtable +0x800); PlayerSkills +0 = Data*:
   xp, levelThreshold, 18 x {level, xp, levelThreshold}, 18 x legendary count (CommonLib layout, checked by the
   levels matching the skills' base values before anything is read or written) */
constexpr u32 OFF_SKILLS = 0x968;
struct SkillXp { float xp, threshold, skill[18][3]; u32 legendary[18]; };
constexpr u32 SPEL = 0x16, WOOP = 0x76, SHOU = 0x77, QUST = 0x4D;
constexpr u32 ARMSMAN_1 = 0x000BABE4;          // a perk for sure: gives the perk form type
constexpr u32 BASE_FLAGS = 0x2u | 0x20u | 0x800u | 0x1000000u | 0x2000000u;   // base data (level), name, face, sex, race

uintptr_t g_getBaseAv, g_setAv, g_hasPerk, g_hasSpell, g_hasShout, g_addSpell, g_addShout, g_teach, g_unlock,
    g_itemCount, g_addItem, g_changes, g_setRace, g_3dReady, g_resetPart, g_reset3d, g_fsCtor;
bool g_ok;

const char* const AV_NAMES[] = {"OneHanded", "TwoHanded", "Marksman", "Block", "Smithing", "HeavyArmor", "LightArmor",
    "Pickpocket", "Lockpicking", "Sneak", "Alchemy", "Speechcraft", "Alteration", "Conjuration", "Destruction",
    "Illusion", "Restoration", "Enchanting", "Health", "Magicka", "Stamina", "DragonSouls"};
constexpr int AV_COUNT = sizeof AV_NAMES / sizeof AV_NAMES[0];
struct FixedString { const char* data; };
FixedString g_avNames[AV_COUNT];               // made on the game thread the first time (never at boot)

struct Head {
    u32 magic;                                 // 'AVT1'
    u16 version, avCount;
    u32 race, appearanceSize;
    u16 perks, spells, words, items;
    u8 perkPoints, pad[3];
    char name[48];
};
constexpr u32 MAGIC = 0x31545641;
struct Item { u32 id; s32 count; };

alignas(16) u8 g_buf[proto::AVATAR_MAX];
u32 g_lastHash;
volatile bool g_saveWanted, g_loadWanted, g_waiting;
u64 g_waitUntil, g_nextAuto;
char g_status[160] = "";
char g_bound[48];                              // name of the character that is the avatar now

template <class T> T At(const void* p, u32 off) { return *reinterpret_cast<const T*>(static_cast<const u8*>(p) + off); }
u32 Id(const void* form) { return At<u32>(form, 0x14); }
u8 Type(const void* form) { return At<u8>(form, 0x1A); }
u64 NowMs() { return nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds(); }

bool Readable(const void* p, u64 n) {
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    if (!a || (a & 7)) return false;
    MemoryInfo mi{}; u32 pi;
    return R_SUCCEEDED(svcQueryMemory(&mi, &pi, a)) && (mi.perm & 1) && mi.addr + mi.size >= a + n;
}

/* TESDataHandler's per-kind arrays (Switch 1.1.177: BSTArray<TESForm*> formArrays[type] at +0x8, 0x10 each:
   data +0, capacity u32 +8, size u32 +0xC — seen in a dump); checked by content before use. */
struct Forms { void** data; u32 count; };
Forms FindForms(u32 type) {
    auto dh = static_cast<u8*>(game::DataHandler());
    if (!dh || type > 0x8A) return {};
    u32 o = 8 + type * 0x10;
    void** data = At<void**>(dh, o);
    u32 n = At<u32>(dh, o + 0xC);
    if (n == 0 || n > At<u32>(dh, o + 8) || !Readable(data, 8ull * n) || !Readable(data[0], 0x20) ||
        !Readable(data[n - 1], 0x20) || Type(data[0]) != type || Type(data[n - 1]) != type) {
        Debug("avatar: forms of type %02x not found (+%x: %p, %u)", type, o, data, n);
        return {};
    }
    return Forms{data, n};
}

void MakeNames() {
    if (g_avNames[0].data) return;
    for (int i = 0; i < AV_COUNT; i++) reinterpret_cast<void (*)(FixedString*, const char*)>(g_fsCtor)(&g_avNames[i], AV_NAMES[i]);
}

float GetBase(void* a, int i) {
    return reinterpret_cast<float (*)(void*, u32, void*, FixedString*)>(g_getBaseAv)(game::FakeVm(), 0, a, &g_avNames[i]);
}
void SetAv(void* a, int i, float v) {
    reinterpret_cast<void (*)(void*, u32, void*, FixedString*, float)>(g_setAv)(game::FakeVm(), 0, a, &g_avNames[i], v);
}
bool HasPerk(void* a, void* p) { return reinterpret_cast<u8 (*)(void*, void*)>(g_hasPerk)(a, p) != 0; }
bool HasSpell(void* a, void* s) {
    return Type(s) == SHOU ? reinterpret_cast<u8 (*)(void*, void*)>(g_hasShout)(a, s) != 0
                           : reinterpret_cast<u8 (*)(void*, void*)>(g_hasSpell)(a, s) != 0;
}
s32 ItemCount(void* ref, void* form) {
    return reinterpret_cast<s32 (*)(void*, u32, void*, void*)>(g_itemCount)(game::FakeVm(), 0, ref, form);
}

/* things carried: the base container's and the inventory changes' objects, counted by GetItemCount */
int CollectItems(void* player, Item* out, int max) {
    int n = 0;
    auto add = [&](void* obj) {
        if (!obj || n >= max) return;
        u32 id = Id(obj);
        for (int i = 0; i < n; i++) if (out[i].id == id) return;
        s32 c = ItemCount(player, obj);
        if (c > 0) out[n++] = Item{id, c};
    };
    void* base = At<void*>(player, 0x40);
    auto entries = At<u8**>(base, 0x80);                    // TESContainer: ContainerObject** +0x80, count +0x88
    u32 count = At<u32>(base, 0x88);
    for (u32 i = 0; entries && i < count && i < 512; i++)
        if (entries[i]) add(At<void*>(entries[i], 8));
    void* ch = reinterpret_cast<void* (*)(void*)>(g_changes)(static_cast<u8*>(player) + 0x70);
    for (auto node = ch ? At<const u8*>(ch, 0) : nullptr; node; node = At<const u8*>(node, 8)) {
        const u8* e = At<const u8*>(node, 0);
        if (e) add(At<void*>(e, 0));
    }
    return n;
}

/* the player's skill data, only when its 18 levels are the skills' base values (the layout is the one we think) */
void* SkillData(void* player) {
    void* skills = At<void*>(player, OFF_SKILLS);
    void* d = Readable(skills, 8) ? At<void*>(skills, 0) : nullptr;
    if (!Readable(d, sizeof(SkillXp))) return nullptr;
    auto x = static_cast<const SkillXp*>(d);
    /* a skill's level here is its base value without what mods / perks add on top: most levels are exactly the
       base value, none is above it, all are sane (seen: 11 of 18 equal, the rest 1-6 lower) */
    int same = 0;
    for (int i = 0; i < 18; i++) {
        float base = GetBase(player, i), lv = x->skill[i][0];
        if (lv < 0.0f || lv > 1000.0f || lv > base + 0.5f || !(x->skill[i][2] >= 0.0f)) {
            Debug("avatar: skill data check failed at %d (%.1f vs %.1f)", i, lv, base);
            return nullptr;
        }
        if (lv > base - 0.5f) same++;
    }
    if (same < 9) { Debug("avatar: skill data check failed: %d of 18 equal", same); return nullptr; }
    return d;
}

u32 Hash(const u8* p, u32 n) { u32 h = 2166136261u; for (u32 i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u; return h; }

/* the whole profile into g_buf; 0 = failed */
u32 Build() {
    void* player = game::Player();
    if (!player) return 0;
    MakeNames();
    Head h{};
    h.magic = MAGIC;
    h.version = 2;
    h.avCount = AV_COUNT;
    h.race = At<void*>(player, OFF_RACE) ? Id(At<void*>(player, OFF_RACE)) : 0;
    h.perkPoints = At<u8>(player, OFF_PERK_POINTS);
    u32 pos = sizeof h;
    u32 app = appearance::SavePlayer(g_buf + pos, 16384, BASE_FLAGS);
    if (!app) return 0;
    h.appearanceSize = app;
    pos += (app + 3) & ~3u;
    for (int i = 0; i < AV_COUNT; i++) { float v = GetBase(player, i); memcpy(g_buf + pos, &v, 4); pos += 4; }
    auto put = [&](u32 v) { if (pos + 4 <= proto::AVATAR_MAX) { memcpy(g_buf + pos, &v, 4); pos += 4; } };
    if (void* armsman = equip::Lookup(ARMSMAN_1)) {
        Forms f = FindForms(Type(armsman));
        for (u32 i = 0; i < f.count && h.perks < 2000; i++)
            if (f.data[i] && HasPerk(player, f.data[i])) { put(Id(f.data[i])); h.perks++; }
    }
    for (u32 t : {SPEL, SHOU}) {
        Forms f = FindForms(t);
        for (u32 i = 0; i < f.count && h.spells < 2000; i++)
            if (f.data[i] && HasSpell(player, f.data[i])) { put(Id(f.data[i])); h.spells++; }
    }
    {
        Forms f = FindForms(WOOP);
        int flagsSeen[4] = {};
        for (u32 i = 0; i < f.count && h.words < 500; i++) {
            void* w = f.data[i];
            if (!w) continue;
            u32 flags = At<u32>(w, 0x10);
            if (flags & 0x40) flagsSeen[0]++;
            if (flags & 0x10000) flagsSeen[1]++;
            if (!(flags & (0x40 | 0x10000))) continue;       // known (0x40) or unlocked (0x10000)
            put(Id(w));
            put(flags & (0x40 | 0x10000));
            h.words++;
        }
        Debug("avatar: words known %d, unlocked %d", flagsSeen[0], flagsSeen[1]);
    }
    {
        static Item items[1024];
        int n = CollectItems(player, items, 1024);
        for (int i = 0; i < n && pos + 8 <= proto::AVATAR_MAX; i++) { memcpy(g_buf + pos, &items[i], 8); pos += 8; h.items++; }
    }
    /* the character's name: TESFullName of the base (+0xC8, BSFixedString +0xD0) */
    const char* nm = At<const char*>(At<void*>(player, 0x40), 0xD0);
    snprintf(h.name, sizeof h.name, "%s", nm && Readable(nm, 1) ? nm : "");
    /* v2: experience (the level bar and each skill's progress), when the layout checks out */
    SkillXp xp{};
    if (auto d = SkillData(player)) memcpy(&xp, d, sizeof xp);
    if (pos + sizeof xp <= proto::AVATAR_MAX) { memcpy(g_buf + pos, &xp, sizeof xp); pos += sizeof xp; }
    memcpy(g_buf, &h, sizeof h);
    return pos;
}

void Refresh3D(void* player) {
    void* obj3d = At<void*>(player, 0x60);
    if (!obj3d) return;
    void* checker = *reinterpret_cast<void**>(game::ModuleAddr(GOT_3D_CHECK));
    checker = checker ? *static_cast<void**>(checker) : nullptr;
    if (!checker || !reinterpret_cast<bool (*)(void*, void*, u32)>(g_3dReady)(checker, obj3d, 0)) return;
    if (void* part = At<void*>(player, 0x128)) reinterpret_cast<void (*)(void*, u32)>(g_resetPart)(part, 0x17);
    void* mgr = *reinterpret_cast<void**>(game::ModuleAddr(GOT_3D_RESET));
    mgr = mgr ? *static_cast<void**>(mgr) : nullptr;
    if (mgr) reinterpret_cast<void (*)(void*, void*, u32, u32)>(g_reset3d)(mgr, player, 0, 0);
}

bool Apply(const u8* data, u32 size) {
    void* player = game::Player();
    Head h;
    if (!player || size < sizeof h) return false;
    memcpy(&h, data, sizeof h);
    if (h.magic != MAGIC || (h.version != 1 && h.version != 2)) return false;
    MakeNames();
    u32 pos = sizeof h;
    u32 app = h.appearanceSize;
    if (pos + app > size) return false;
    void* oldRace = At<void*>(player, OFF_RACE);
    appearance::LoadPlayer(data + pos, app, BASE_FLAGS);
    pos += (app + 3) & ~3u;
    void* race = equip::Lookup(h.race);
    Debug("avatar: race %08x -> %08x", oldRace ? Id(oldRace) : 0, h.race);
    if (race && race != oldRace)
        reinterpret_cast<void (*)(void*, u32, void*, void*)>(g_setRace)(game::FakeVm(), 0, player, race);
    else
        Refresh3D(player);
    for (int i = 0; i < h.avCount && pos + 4 <= size; i++, pos += 4) {
        float v; memcpy(&v, data + pos, 4);
        if (i < AV_COUNT) SetAv(player, i, v);
    }
    auto get = [&]() { u32 v = 0; if (pos + 4 <= size) { memcpy(&v, data + pos, 4); } pos += 4; return v; };
    int added[4] = {};
    for (int i = 0; i < h.perks; i++) {
        void* p = equip::Lookup(get());
        if (!p || HasPerk(player, p)) continue;
        auto vt = *reinterpret_cast<uintptr_t**>(player);
        reinterpret_cast<void (*)(void*, void*, u32)>(vt[VT_ADD_PERK / 8])(player, p, 0);
        added[0]++;
    }
    for (int i = 0; i < h.spells; i++) {
        void* s = equip::Lookup(get());
        if (!s || HasSpell(player, s)) continue;
        if (Type(s) == SHOU) reinterpret_cast<void (*)(void*, void*)>(g_addShout)(player, s);
        else if (Type(s) == SPEL) reinterpret_cast<bool (*)(void*, void*)>(g_addSpell)(player, s);
        added[1]++;
    }
    for (int i = 0; i < h.words; i++) {
        void* w = equip::Lookup(get());
        u32 st = get();
        if (!w || Type(w) != WOOP) continue;
        u32 flags = At<u32>(w, 0x10);
        bool teach = !(flags & 0x40), unlock = (st & 0x10000) && !(flags & 0x10000);
        if (teach) reinterpret_cast<void (*)(void*, u32, void*, void*)>(g_teach)(game::FakeVm(), 0, nullptr, w);
        if (unlock) reinterpret_cast<void (*)(void*, u32, void*, void*)>(g_unlock)(game::FakeVm(), 0, nullptr, w);
        if (teach || unlock) added[2]++;
    }
    for (int i = 0; i < h.items && pos + 8 <= size; i++, pos += 8) {
        Item it; memcpy(&it, data + pos, 8);
        void* f = equip::Lookup(it.id);
        if (!f) continue;
        s32 have = ItemCount(player, f);
        if (it.count > have) {
            reinterpret_cast<void (*)(void*, u32, void*, void*, s32, bool)>(g_addItem)(game::FakeVm(), 0, player, f, it.count - have, true);
            added[3]++;
        }
    }
    *reinterpret_cast<u8*>(static_cast<u8*>(player) + OFF_PERK_POINTS) = h.perkPoints;
    if (h.version >= 2 && pos + sizeof(SkillXp) <= size) {          // experience, after the levels were set above
        SkillXp xp;
        memcpy(&xp, data + pos, sizeof xp);
        if (xp.threshold > 0)
            if (auto d = static_cast<SkillXp*>(SkillData(player))) {
                d->xp = xp.xp; d->threshold = xp.threshold;
                for (int i = 0; i < 18; i++) { d->skill[i][0] = xp.skill[i][0]; d->skill[i][1] = xp.skill[i][1]; d->skill[i][2] = xp.skill[i][2]; d->legendary[i] = xp.legendary[i]; }
            }
    }
    snprintf(g_status, sizeof g_status, "персонаж %s перенесён: перков +%d, заклинаний +%d, слов +%d, вещей +%d",
             h.name, added[0], added[1], added[2], added[3]);
    Log("avatar applied: %s", g_status);
    snprintf(g_bound, sizeof g_bound, "%s", h.name);
    return true;
}

}  // namespace

bool Resolve() {
    auto c = [](const Fn& f) { return game::Checked177(f.off, f.sig); };
    g_getBaseAv = c(GET_BASE_AV); g_setAv = c(SET_AV); g_hasPerk = c(HAS_PERK); g_hasSpell = c(HAS_SPELL);
    g_hasShout = c(HAS_SHOUT); g_addSpell = c(ADD_SPELL); g_addShout = c(ADD_SHOUT); g_teach = c(TEACH_WORD);
    g_unlock = c(UNLOCK_WORD); g_itemCount = c(ITEM_COUNT); g_addItem = c(ADD_ITEM); g_changes = c(GET_CHANGES);
    g_setRace = c(SET_RACE); g_3dReady = c(IS_3D_READY); g_resetPart = c(RESET_PART); g_reset3d = c(RESET_3D);
    g_fsCtor = c(FS_CTOR);
    g_ok = g_getBaseAv && g_setAv && g_hasPerk && g_hasSpell && g_hasShout && g_addSpell && g_addShout && g_teach &&
           g_unlock && g_itemCount && g_addItem && g_changes && g_setRace && g_3dReady && g_resetPart && g_reset3d && g_fsCtor;
    return g_ok;
}

void RequestSave() { g_saveWanted = true; }
void RequestLoad() { g_loadWanted = true; }
const char* Status() { return g_status; }

void Pump() {
    if (!g_ok || !game::Player()) return;
    u64 now = NowMs();
    bool connected = coop::Status() == NetStatus::Connected;
    /* by itself every 5 minutes online (when something changed), only for the character that is the avatar
       (saved or put here by hand in this session): a new game never overwrites a character of 100 hours */
    const char* nm = At<const char*>(At<void*>(game::Player(), 0x40), 0xD0);
    bool bound = g_bound[0] && nm && Readable(nm, 1) && !strcmp(nm, g_bound);
    bool autoSave = connected && bound && now > g_nextAuto;
    if (autoSave || !bound) g_nextAuto = now + 300000;
    if (__atomic_exchange_n(&g_saveWanted, false, __ATOMIC_ACQ_REL) || autoSave) {
        bool asked = !autoSave;
        if (!connected) { if (asked) game::Notify("Кооп: нет связи с сервером — персонаж не сохранён"); }
        else if (u32 n = Build()) {
            u32 h = Hash(g_buf, n);
            if (asked || h != g_lastHash) {
                bool sent = AvatarPut(g_buf, n);
                if (sent) g_lastHash = h;
                Head hd; memcpy(&hd, g_buf, sizeof hd);
                if (sent) snprintf(g_bound, sizeof g_bound, "%s", hd.name);
                snprintf(g_status, sizeof g_status, "персонаж %s на сервере (%u КБ: перков %u, заклинаний %u, вещей %u)",
                         hd.name, (n + 1023) / 1024, hd.perks, hd.spells, hd.items);
                Debug("avatar: %s", g_status);
                if (asked) game::Notify(sent ? "Кооп: персонаж сохранён на сервере" : "Кооп: сервер ещё принимает прошлый — попробуй позже");
            }
        } else if (asked) {
            game::Notify("Кооп: не удалось собрать персонажа");
        }
    }
    if (__atomic_exchange_n(&g_loadWanted, false, __ATOMIC_ACQ_REL)) {
        if (!connected) { game::Notify("Кооп: нет связи с сервером"); return; }
        if (!quests::IntroDone()) { game::Notify("Кооп: сначала пройди вступление (Хелген) — потом перенесёшь персонажа"); return; }
        AvatarRequest();
        g_waiting = true;
        g_waitUntil = now + 15000;
    }
    if (g_waiting) {
        int st = AvatarState();
        if (st == 2) {
            g_waiting = false;
            u32 size = 0;
            const u8* d = AvatarData(&size);
            bool ok = d && Apply(d, size);
            game::Notify(ok ? "Кооп: твой персонаж перенесён в эту игру" : "Кооп: профиль персонажа повреждён");
        } else if (st == 3) {
            g_waiting = false;
            game::Notify("Кооп: на сервере нет твоего персонажа — сначала «Сохранить персонажа»");
        } else if (now > g_waitUntil) {
            g_waiting = false;
            game::Notify("Кооп: сервер не ответил");
        }
    }
}

}  // namespace coop::avatar
