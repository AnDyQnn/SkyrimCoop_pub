#include "scrolls.hpp"
#include "settings.hpp"
#include <nn/fs.hpp>
#include "client.hpp"
#include "equip.hpp"
#include "game.hpp"
#include "trade.hpp"
#include "quests.hpp"
#include "lib.hpp"
#include <nn/os.hpp>
#include <cstdio>
#include <cstring>
#include <cmath>

/* The smuggler's spell scrolls (SwitchPatch.esp, college/smuggler.py): Elder-Scroll-looking books that are NOT
   spell tomes, so reading does not use them up — a clanmate can read the same scroll, it can be stolen, looted,
   put on a shelf. The scroll book keeps its spell's FormID (SwitchPatch's own space) in the book data where a tome
   keeps "teaches"; the flag "teaches a spell" is off, so the engine ignores it. We watch the books' "has been read"
   flag: when the player reads one, the flag is cleared again, the spell is added, the dragon-kill music plays and
   the Elder Scroll reading animation runs.
   Offsets are learned from a vanilla tome (SpellTomeIceSpike 0x0A26FE: flags 0x04, then the spell pointer). */
namespace nn::fs {
    Result CommitSaveData(const char* mount);
    Result GetEntryType(DirectoryEntryType* type, const char* path);
}

namespace coop::scrolls {
namespace {

struct Fn { u32 off; u32 sig[4]; };
constexpr Fn HAS_SPELL{0x673c7c, {0xD100C3FF, 0xA9027BFD, 0x910083FD, 0xF9000BE1}};
constexpr Fn ADD_SPELL{0x673ef4, {0xD10183FF, 0xA9037BFD, 0x9100C3FD, 0xF90023F5}};
constexpr Fn MUSIC_ADD{0xa1aee0, {0xF000B9A8, 0xF9465108, 0xF9400100, 0xAA0203E1}};   // MusicType.Add (vm, stack, music)
constexpr Fn ITEM_COUNT{0xa50430, {0x2A0103E8, 0xAA0003E9, 0xAA0203E0, 0xAA0303E1}};   // ObjectReference.GetItemCount
constexpr Fn PLAY_IDLE{0xa01d40, {0xA9BF7BFD, 0x910003FD, 0x2A0103E9, 0xAA0003E8}};  // Actor.PlayIdle (vm, stack, actor, idle)
constexpr u32 IDLE_READ_ELDER_SCROLL = 0x0EC9D0;
constexpr Fn SOUND_PLAY{0xa73300, {0xD10143FF, 0xA9017BFD, 0x910043FD, 0xF90013F7}};   // Sound.Play (vm, stack, sound, from)
constexpr Fn DISABLE_CONTROLS{0xa28d40, {0xA9BD7BFD, 0xA90157F6, 0x910003FD, 0xA9024FF4}};
constexpr Fn ENABLE_CONTROLS{0xa28e60, {0xA9BE7BFD, 0xA9014FF4, 0x910003FD, 0x52808029}};
constexpr Fn FIRST_PERSON{0xa29fb0, {0xB000B8E8, 0xF9465108, 0xF9400100, 0x17FA3524}};   // Game.ForceFirstPerson
constexpr Fn SHAKE_CAMERA{0xa2b9e0, {0xD10083FF, 0xA9017BFD, 0x910043FD, 0xF90003FF}};   // Game.ShakeCamera (vm, stack, from, strength, secs)
constexpr Fn VFX_PLAY{0xa1cc10, {0xFC1C0FE8, 0xA900FBFD, 0x910023FD, 0xF9000FF7}};       // VisualEffect.Play (vm, stack, fx, on, secs, facing)
constexpr Fn IMOD_APPLY{0xa40680, {0xAA0203E0, 0xAA1F03E1, 0x17DD3AB4, 0x00000000}};     // ImageSpaceModifier.Apply (vm, stack, imod, strength)
constexpr Fn EQUIP_ITEM{0x9fd850, {0, 0, 0, 0}};                                            // Actor.EquipItem (vm, stack, actor, form, keep, silent)
constexpr Fn UNEQUIP{0xa04c70, {0xD10203FF, 0xA9037BFD, 0x9100C3FD, 0xA90467FA}};         // Actor.UnequipItem
constexpr Fn IN_MENU{0xa77404, {0xF000B6C8, 0xF000B6C9, 0xF9442908, 0xF9449D29}};        // Utility.IsInMenuMode (vm, stack)
constexpr Fn SPELL_CAST_NATIVE{0xa35ec0, {0xD10183FF, 0xA9017BFD, 0x910043FD, 0xF90013F9}};   // Spell.Cast (vm, stack, spell, from, to)
constexpr Fn SHADER_PLAY{0xa379f0, {0xFC1B0FE8, 0xA9017BFD, 0x910043FD, 0xA9025FF8}};   // EffectShader.Play (vm, stack, fx, ref, secs)
constexpr u32 ASPECT_FX[3] = {0x02021735, 0x02021734, 0x02021733};   // Dragonborn: DLC2DragonbornAspectFXS01..03
constexpr u32 SMUGGLER_REF = 0x828;                // SwitchPatch's own: her ACHR (college/smuggler.py)
constexpr Fn INCREMENT_SKILL{0x705238, {0xF944B400, 0x7100005F, 0x1A9F1442, 0x1400F6B4}};  // PlayerCharacter::IncrementSkill(av, n)
constexpr Fn MOVE_TO{0xa51e00, {0xD10203FF, 0xFD0013EA, 0xF90027F9, 0x6D02A3E9}};           // ObjectReference.MoveTo
constexpr u32 SMUGGLER_FACTION = 0x826;
constexpr u32 SMUGGLER_CHEST = 0x825, SKILL_BOOK_FIRST = 0xD00, SKILL_BOOKS = 6, GLOBAL_DAYS = 0x39;
/* her three shops (a marker in each, Skyrim.esm): the Ragged Flagon (Riften), the New Gnisis Cornerclub (Windhelm),
   the Winking Skeever (Solitude); she moves every 10 game days */
constexpr u32 SHOP_MARKER = 0xE00;       // SwitchPatch's own persistent markers 0xE00..0xE02 (always loaded)
constexpr const char* SHOP_NAME[3] = {"рынок Вайтрана", "двор Коллегии Винтерхолда", "рынок Маркарта"};   // 01.10
constexpr u32 QUEST_FLAGS = 0xC4;         // TESQuest flags: bit 0 running (quests.cpp)
constexpr u32 DIALOGUE_QUEST = 0xF00;     // SSWShaj: her greetings and topics (must be running)
constexpr Fn QUEST_START{0x28df8c, {0xD100C3FF, 0xA9017BFD, 0x910043FD, 0xA9024FF4}};
constexpr u32 RETURNED_GLOBAL = 0xF60, BOUGHT_GLOBAL = 0xF61, BOUGHT_TOPIC = 0x1190;   // her per-item lines (shajara.py)
constexpr Fn SAY{0xa54ef0, {0, 0, 0, 0}};           // ObjectReference.Say (vm, stack, ref, topic, speakAs, inHead)
constexpr u32 GRUMBLE_GLOBAL = 0xF01;     // SSW_SmugglerGrumble: her greeting scolds a sold unique (shajara.py)
constexpr u32 SHOP_GLOBAL = 0xE10;        // SSW_SmugglerShop: her packages walk her to the shop it names
constexpr u32 WHITERUN_MARKET = 0x0847A4;   // a persistent marker by the Whiterun market stalls (Skyrim.esm)
constexpr Fn EVALUATE_PACKAGE{0x9fdf70, {0x79402048, 0x52810409, 0x6A09011F, 0x54000040}};
using ControlsFn = void (*)(void*, u32, bool, bool, bool, bool, bool, bool, bool, bool, u32);
/* Skyrim's own reading of an Elder Scroll (ElderScrollScript.OnEquipped, the ordinary branch, decompiled): menus
   off, first person, the scroll in the hands (ElderScrollHandAttachArmor), IdleReadElderScroll; +1.05 s the
   blind-in sound; +0.5 s camera shake, the scroll's swirl (FXReadElderScrollEffect, 8.1 s) and the white flash
   (FXReadScrollsBlindImod); +3 s IdleStop, the scroll taken away, menus back; +1.9 s the blind-out sound. Ours
   adds the dragon soul pouring in (NPCDragonDeathFX2D) and the Word of Power choir. */
constexpr u32 ARMOR_SCROLL_IN_HANDS = 0x0F71DF, IDLE_READ = 0x0EC9D0, IDLE_STOP = 0x0E4242,
              SND_BLIND_IN = 0x10A95A, SND_BLIND_OUT = 0x10A95B, SND_DRAGON_SOUL = 0x06D1C4,
              VFX_READ = 0x044F20, IMOD_BLIND = 0x044F3F;
constexpr u32 VANILLA_TOME = 0x0A26FE, VANILLA_TOME_SPELL = 0x02B96C /* Ice Spike */, MUS_REWARD = 0x017415 /* MUSSpecialWordOfPower: the choir */, FORM_BOOK = 0x1B, FORM_SPEL = 0x16;
constexpr u32 FLAG_TEACHES = 0x04, FLAG_READ = 0x08;
constexpr int MAX_SCROLLS = 64;
/* the ancient scroll's blessing (college/blessing.py): an ability shown in Active Effects while skills learn 2x;
   seconds of play left in a global (kept in the save). Actor.RemoveSpell (vm, stack, actor, spell) takes it off. */
constexpr u32 BLESS_SPELL = 0xF41, BLESS_LEFT = 0xF42;
/* shout scrolls (college/shouts.py): the teaches field holds a SHOU; its three words are the next three FormIDs.
   Checked in avatar.cpp: HasShout / AddShout (actor, shout), Game.TeachWord / UnlockWord (vm, stack, none, word) */
constexpr u32 FORM_SHOU = 0x77;
/* her contracts (college/bounty.py): book "teaches" 0xFD000000 | k; boss k is our NPC_ 0x1200 + k, its quest 0x1220 + k
   (objective 10: the place — a forced alias on the map marker; 20: back to her). Globals keep the hunt in the save:
   SSW_BountyStage (0 none, 1 hunting, 2 dead — go to her), SSW_BountyIndex, SSW_BountyRef (the boss reference's
   low 24 bits, 0xFF...... refs). Quest natives (tools/xrefs.py, vm, stack, quest, ...). */
constexpr u32 BOSS_BASE = 0x1200, BOSS_QUEST = 0x1220, G_BOUNTY_STAGE = 0xF70, G_BOUNTY_INDEX = 0xF71, G_BOUNTY_REF = 0xF72,
              REWARD_TOPIC = 0x11C0, TAMRIEL = 0x3C;
constexpr int BOSSES = 6;
constexpr u32 BOSS_MARKER[BOSSES] = {0x06CCC8, 0x016234, 0x016274, 0x0D393C, 0x0EF0A7, 0x016238};   // (2: Lost Valley Redoubt; Reachwind Eyrie was inside Markarth)   // Skyrim.esm map markers
constexpr float BOSS_HP[BOSSES] = {6000, 9000, 5000, 9000, 8000, 7000};
constexpr const char* BOSS_NAME[BOSSES] = {"Ваграт Безликий", "Хьёрт Ледяная Глыба", "Мать Терновник", "Каратель Бталфта",
                                           "Тень Глубин", "Каменный Палач"};
constexpr u32 Q_START = 0xa710b0, Q_SET_STAGE = 0xa70b70, Q_OBJ_SHOW = 0xa70e40, Q_OBJ_DONE = 0xa70d10, Q_RESET = 0xa70ab0,
              A_IS_DEAD = 0xa00d30, R_SET_SCALE = 0xa55ca0, A_SET_AV = 0xa02a40, FS_CTOR = 0xae6da8;
constexpr Fn HAS_SHOUT{0x6745c8, {0xF9402008, 0x91024100, 0x17E9924B, 0xA9BE7BFD}};
constexpr Fn ADD_SHOUT{0x6745d4, {0xA9BE7BFD, 0xA9014FF4, 0x910003FD, 0xF9402008}};
constexpr Fn TEACH_WORD{0xa2bdb0, {0xA9BB7BFD, 0xF9000BF9, 0x910003FD, 0xA9025FF8}};
constexpr Fn UNLOCK_WORD{0xa2c0b0, {0xA9BB7BFD, 0xA90167FA, 0x910003FD, 0xA9025FF8}};
constexpr float BLESS_SECONDS = 3600.0f;
constexpr Fn REMOVE_SPELL{0xa025e0, {0xD100C3FF, 0xA9017BFD, 0x910043FD, 0xA9024FF4}};
constexpr u32 VT_USE_SKILL = 0x800 / 8;     // PlayerCharacter::UseSkill(av, points, form, unk): Game.AdvanceSkill's call

uintptr_t g_hasShout, g_addShout, g_teachWord, g_unlockWord, g_removeSpell, g_useSkill, g_questStart, g_evalPkg, g_incSkill, g_moveTo, g_shader, g_castNative, g_inMenu, g_hasSpell, g_addSpell, g_music, g_playIdle, g_sound, g_disable, g_enable, g_first, g_shake, g_vfx, g_imod, g_equip, g_unequip;
bool g_ok;
u32 g_flagsOff;                          // TESObjectBOOK: flags byte; the teaches union 8 bytes further
struct Scroll { u8* book; u32 spell; };
struct SkillBook { u8* book; u32 packed; };
struct Contract { u8* book; u32 k; };
Contract g_contracts[BOSSES];
int g_contractCount;
SkillBook g_skillBooks[8];
int g_skillCount;
u64 g_shopCheck, g_closeBookAt;
volatile float g_dayOffset;
/* her unique items: one sold by the player (it left the inventory while the gold went up) comes back to her 3 game
   days later — the vendor's own stock resets by then, so there are still only two in the world */
struct UniqueDef { const char* plugin; u32 obj; };
const UniqueDef UNIQUES[] = {
#include "smuggler_uniques.inc"
};
constexpr int NU = sizeof UNIQUES / sizeof UNIQUES[0];
u32 g_uniqueId[NU];
s32 g_uniqueHave[NU];
bool g_uniquesReady;
s32 g_lastGold = -1;
bool g_vendorRedone;                     // her vendor faction re-applied (once a session and after each move)
u64 g_goldUpAt, g_goldDownAt, g_lostAt[NU], g_gotAt[NU];
float g_grumbleDay = -1.0f;              // test aid (console "smugglerday +N")
Scroll g_scrolls[MAX_SCROLLS];
int g_count = -1;                        // -1: not looked for yet
int g_patchIdx = -1;
u64 g_next, g_sceneAt, g_lastB, g_outSince, g_auraAt;                   // the reading scene's start (0: none)
int g_step;
/* 03.10 (his wish): the scroll unfurls first, its page comes after. Read from the bag: the page is shut at once, the
   scene plays, then the page opens again (the scroll laid out of sight and activated: "take" puts it back in the bag;
   left there, it goes back into the bag silently). Read off a shelf: the scene after the page, as before. */
u8* g_sceneBook;
bool g_fromBag, g_known;
u8* g_skipRead;                          // the page opened again: its "read" flag is ours, not a new reading
void* g_pageRef;
s32 g_pageCount;
u64 g_pageAt;
int g_pageTries;
bool g_pageSeen;
uintptr_t g_itemCount;
s32 Count(void* ref, void* form) {
    return g_itemCount ? reinterpret_cast<s32 (*)(void*, u32, void*, void*)>(g_itemCount)(game::FakeVm(), 0, ref, form) : 0;
}

template <class T> T At(const void* p, u32 off) { return *reinterpret_cast<const T*>(static_cast<const u8*>(p) + off); }
u8 Type(const void* f) { return At<u8>(f, 0x1A); }
u32 Id(const void* f) { return At<u32>(f, 0x14); }
void* Form(u32 id) { return equip::Lookup(id); }

bool Calibrate() {
    /* the BOOK's "teaches" data: find the field holding the spell the vanilla tome teaches (Ice Spike), comparing
       pointers only. 02.10: the old scan read +0x1A of every pointer-looking value and one of them was not mapped on
       the real console (Data Abort, src+0xc974, right after the save loaded); Eden never had that value there. */
    auto tome = static_cast<u8*>(Form(VANILLA_TOME));
    void* want = Form(VANILLA_TOME_SPELL);
    if (!tome || !want || Type(tome) != FORM_BOOK) return false;
    for (u32 o = 0x20; o < 0x200; o += 4) {
        if (!(tome[o] & FLAG_TEACHES)) continue;
        if (At<void*>(tome, o + 8) == want) { g_flagsOff = o; return true; }
    }
    return false;
}

void Find() {
    g_count = 0;
    int idx = game::PluginIndex("SwitchPatch.esp");
    g_patchIdx = idx;
    auto dh = static_cast<u8*>(game::DataHandler());
    if (idx < 0 || !dh || !Calibrate()) {
        Log("свитки: не найдены (SwitchPatch %d, смещение %x)", idx, g_flagsOff);
        return;
    }
    u32 o = 8 + FORM_BOOK * 0x10;
    auto data = At<u8**>(dh, o);
    u32 n = At<u32>(dh, o + 0xC);
    for (u32 i = 0; i < n && g_count < MAX_SCROLLS; i++) {
        u8* b = data[i];
        if (!b || (Id(b) >> 24) != (u32)idx || (b[g_flagsOff] & FLAG_TEACHES)) continue;
        u32 raw = At<u32>(b, g_flagsOff + 8);                    // our spell, SwitchPatch's own FormID space
        u32 spell = ((u32)idx << 24) | (raw & 0xFFFFFF);
        void* sp = (raw >> 24) ? Form(spell) : nullptr;
        if (sp && (Type(sp) == FORM_SPEL || Type(sp) == FORM_SHOU)) g_scrolls[g_count++] = Scroll{b, spell};
        else if ((raw >> 24) == 0xFE && g_skillCount < 8) g_skillBooks[g_skillCount++] = SkillBook{b, raw};
        else if ((raw >> 24) == 0xFD && (raw & 0xFF) < BOSSES && g_contractCount < BOSSES)
            g_contracts[g_contractCount++] = Contract{b, raw & 0xFF};
    }
    Log("свитки заклинаний: %d, контрактов %d, книг навыков %d (SwitchPatch %02x, флаги книги +%x)", g_count,
        g_contractCount, g_skillCount, idx, g_flagsOff);
    Log("плагины: LegacyoftheDragonborn %d, _ResourcePack %d, всего до SwitchPatch %d", game::PluginIndex("LegacyoftheDragonborn.esp"),
        game::PluginIndex("_ResourcePack.esp"), idx);
    {   // (06.10, LotD: which files the game loaded around ours)
        static char list[8192];
        if (game::PluginList(list, sizeof list)) {
            int n = 0;
            for (char* p = list; *p; n++) {
                char* e = strchr(p, '\n');
                if (e) *e = 0;
                if (n >= 80) Log("плагин %d: %s", n, p);
                if (!e) break;
                p = e + 1;
            }
        }
    }
}

void Sound(u32 id, void* from) {
    if (void* snd = Form(id); snd && g_sound) reinterpret_cast<u32 (*)(void*, u32, void*, void*)>(g_sound)(game::FakeVm(), 0, snd, from);
}

void Idle(void* player, u32 id) {
    if (void* idle = Form(id); idle && g_playIdle) reinterpret_cast<bool (*)(void*, u32, void*, void*)>(g_playIdle)(game::FakeVm(), 0, player, idle);
}

/* the reading, step by step (see above); called every pump while it runs */
/* 2x skill experience while the blessing lasts: the player's UseSkill (every skill use: hits, spells, locks...)
   gets double points */
void SetGlobal(u32 obj, float v) {
    if (g_patchIdx < 0) return;
    if (auto g = static_cast<u8*>(Form(((u32)g_patchIdx << 24) | obj))) *reinterpret_cast<float*>(g + 0x34) = v;
}

[[maybe_unused]] int UniqueIndex(u32 id) {
    for (int k = 0; k < NU; k++) if (g_uniqueId[k] == id) return k;
    return -1;
}

float* BlessLeft() {
    void* g = g_patchIdx >= 0 ? Form(((u32)g_patchIdx << 24) | BLESS_LEFT) : nullptr;
    return g ? reinterpret_cast<float*>(static_cast<u8*>(g) + 0x34) : nullptr;      // TESGlobal value
}

volatile u32 g_doubled;                 // skill uses doubled (for the log)

HOOK_DEFINE_TRAMPOLINE(UseSkillHook) {
    static void Callback(void* self, u32 av, float points, void* form, u32 unk) {
        float* left = BlessLeft();
        if (left && *left > 0.0f && self == game::Player()) { points *= 2.0f; g_doubled = g_doubled + 1; }
        Orig(self, av, points, form, unk);
    }
};

void Bless(void* player) {
    float* left = BlessLeft();
    void* sp = g_patchIdx >= 0 ? Form(((u32)g_patchIdx << 24) | BLESS_SPELL) : nullptr;
    if (!left || !sp) return;
    *left = BLESS_SECONDS;
    if (!reinterpret_cast<u8 (*)(void*, void*)>(g_hasSpell)(player, sp)) reinterpret_cast<bool (*)(void*, void*)>(g_addSpell)(player, sp);
    Log("благословение свитка: опыт x2 на час");
}

/* the banner text: the spell's name (TESFullName in MagicItem +0x28, BSFixedString +0x30; seen in memory) and the same name in
   Latin letters, which the HUD's dragon font draws as runes */
char g_bannerName[160] = "ДРЕВНИЙ СВИТОК", g_bannerRunes[160] = "DREVNIY SVITOK";
bool g_noBanner;

void SetBanner(void* spell) {
    const char* nm = spell ? *reinterpret_cast<const char**>(static_cast<u8*>(spell) + 0x30) : nullptr;
    if (!nm || !*nm) return;
    snprintf(g_bannerName, sizeof g_bannerName, "%s", nm);
    static const char* const LAT[64] = {                  // А..я (U+0410..U+044F)
        "A", "B", "V", "G", "D", "E", "ZH", "Z", "I", "Y", "K", "L", "M", "N", "O", "P", "R", "S", "T", "U", "F", "KH",
        "TS", "CH", "SH", "SH", "", "Y", "", "E", "YU", "YA",
        "A", "B", "V", "G", "D", "E", "ZH", "Z", "I", "Y", "K", "L", "M", "N", "O", "P", "R", "S", "T", "U", "F", "KH",
        "TS", "CH", "SH", "SH", "", "Y", "", "E", "YU", "YA"};
    size_t o = 0;
    for (const u8* p = reinterpret_cast<const u8*>(nm); *p && o + 4 < sizeof g_bannerRunes; ) {
        const char* add = nullptr;
        char one[2] = {0, 0};
        if (p[0] < 0x80) {
            one[0] = (p[0] >= 'a' && p[0] <= 'z') ? char(p[0] - 32) : ((p[0] >= 'A' && p[0] <= 'Z') || p[0] == ' ') ? char(p[0]) : 0;
            add = one; p += 1;
        } else if ((p[0] & 0xE0) == 0xC0 && p[1]) {
            u32 c = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            if (c >= 0x410 && c <= 0x44F) add = LAT[c - 0x410];
            else if (c == 0x401 || c == 0x451) add = "YO";
            p += 2;
        } else {
            p += 1;
        }
        for (; add && *add && o + 1 < sizeof g_bannerRunes; add++) g_bannerRunes[o++] = *add;
    }
    g_bannerRunes[o] = 0;
    Log("свиток: надпись «%s» / %s", g_bannerName, g_bannerRunes);
}

void Scene(void* player, u64 now) {
    u64 t = now - g_sceneAt;
    void* vm = game::FakeVm();
    void* hands = Form(ARMOR_SCROLL_IN_HANDS);
    if (g_step == 0) {                                        // every menu shut first (book page, inventory, tween)
        bool menu = g_inMenu && reinterpret_cast<bool (*)(void*, u32)>(g_inMenu)(vm, 0);
        if (menu) {
            g_outSince = 0;
            /* read from the bag: the page goes away at once, it comes back after the scene (step 5) */
            if (g_fromBag && t > 150 && now - g_lastB > 450) { g_lastB = now; game::Press(1ull << 1, 120); }   // B
            return;
        }
        if (!g_outSince) g_outSince = now;
        if (now - g_outSince < 300) return;
        g_step = 1;
        g_sceneAt = now - 700;                                // the timeline below starts now (its +700 step)
        return;
    }
    if (g_step == 1 && t >= 700) {
        g_step = 2;
        if (g_disable) reinterpret_cast<ControlsFn>(g_disable)(vm, 0, false, false, false, false, false, true, false, false, 0);
        if (g_first) reinterpret_cast<void (*)(void*, u32)>(g_first)(vm, 0);
        if (hands && g_equip) reinterpret_cast<void (*)(void*, u32, void*, void*, bool, bool)>(g_equip)(vm, 0, player, hands, false, true);
        Idle(player, IDLE_READ);
        Sound(SND_DRAGON_SOUL, player);                       // the dragon soul pouring in
    } else if (g_step == 2 && t >= 700 + 1050) {
        g_step = 3;
        Sound(SND_BLIND_IN, player);
    } else if (g_step == 3 && t >= 700 + 1550) {
        g_step = 4;
        if (g_shake) reinterpret_cast<void (*)(void*, u32, void*, float, float)>(g_shake)(vm, 0, nullptr, 0.5f, 1.5f);
        if (void* fx = Form(VFX_READ); fx && g_vfx) reinterpret_cast<void (*)(void*, u32, void*, void*, float, void*)>(g_vfx)(vm, 0, fx, player, 8.1f, nullptr);
        if (void* im = Form(IMOD_BLIND); im && g_imod) reinterpret_cast<void (*)(void*, u32, void*, float)>(g_imod)(vm, 0, im, 1.0f);
        if (void* mus = Form(MUS_REWARD); mus && g_music) reinterpret_cast<void (*)(void*, u32, void*)>(g_music)(vm, 0, mus);   // the choir
        /* the banner: our hudmenu.gfx turns "##SSW##NAME|RUNES" into the game's big letter-by-letter title (as a
           learned Word of Power: dragon runes under it), gold with a crimson / emerald shimmer running along it */
        char banner[400];
        snprintf(banner, sizeof banner, "##SSW##%s|%s", g_bannerName, g_bannerRunes);
        if (!g_noBanner && !g_known) game::Notify(banner);   // a shout: the game shows its three word titles itself
        g_noBanner = false;
        game::Notify(g_known ? "<font color='#BEBEBE'>Эти письмена тебе уже знакомы</font>"
                             : "<font color='#4AFF8A'>Древнее знание теперь в тебе</font>");
    } else if (g_step == 4 && t >= 700 + 4550) {
        g_step = 5;
        Idle(player, IDLE_STOP);
        if (hands) {
            if (g_unequip) reinterpret_cast<void (*)(void*, u32, void*, void*, bool, bool)>(g_unequip)(vm, 0, player, hands, false, true);
            trade::AddItems(player, hands, -1);
        }
        if (g_enable) reinterpret_cast<ControlsFn>(g_enable)(vm, 0, false, false, false, false, false, true, false, false, 0);
    } else if (g_step == 5 && t >= 700 + 6450) {
        g_step = 6;
        Sound(SND_BLIND_OUT, player);
    } else if (g_step == 6 && t >= 700 + 7200) {
        g_sceneAt = 0;
        /* now the page (once the controls are back: activating sooner opened nothing): the scroll out of the bag,
           laid out of sight and read */
        if (g_fromBag && g_sceneBook && !g_pageRef) {
            s32 n = Count(player, g_sceneBook);
            const float* p = game::RefPos(player);
            Log("свиток: страница снова, в сумке %d", n);
            if (n > 0 && p) {
                trade::AddItems(player, g_sceneBook, -1);
                void* ref = game::PlaceObject(g_sceneBook, p[0], p[1], p[2] - 400.0f);
                if (ref) {
                    g_skipRead = g_sceneBook;
                    g_pageRef = ref; g_pageCount = n - 1; g_pageAt = now; g_pageTries = 1; g_pageSeen = false;
                    trade::ActivateAsPlayer(ref);
                } else {
                    trade::AddItems(player, g_sceneBook, 1, true);
                }
            }
        }
    }
}

/* ---- her contracts ---- */
float GetG(u32 obj) {
    auto g = g_patchIdx >= 0 ? static_cast<u8*>(Form(((u32)g_patchIdx << 24) | obj)) : nullptr;
    return g ? *reinterpret_cast<float*>(g + 0x34) : 0.0f;
}

struct FixedString { const char* data; };

void SetAv(void* actor, const char* name, float v) {
    static FixedString fs[4];
    static const char* names[4] = {"Health", "AttackDamageMult", "DamageResist", "MagicResist"};
    static bool made;
    if (!made) {
        for (int i = 0; i < 4; i++) reinterpret_cast<void (*)(FixedString*, const char*)>(game::ModuleAddr(FS_CTOR))(&fs[i], names[i]);
        made = true;
    }
    for (int i = 0; i < 4; i++)
        if (!strcmp(names[i], name))
            reinterpret_cast<void (*)(void*, u32, void*, FixedString*, float)>(game::ModuleAddr(A_SET_AV))(game::FakeVm(), 0, actor, &fs[i], v);
}

void* BossQuest(int k) { return Form(((u32)g_patchIdx << 24) | (BOSS_QUEST + k)); }

void QuestCall(u32 fn, void* q) { if (q) reinterpret_cast<bool (*)(void*, u32, void*)>(game::ModuleAddr(fn))(game::FakeVm(), 0, q); }
void QuestStage(void* q, u32 stage) {
    if (q) reinterpret_cast<bool (*)(void*, u32, void*, u32)>(game::ModuleAddr(Q_SET_STAGE))(game::FakeVm(), 0, q, stage);
}
/* force: shown again even when it already is (the game prints the objective each time) — only on a real change */
void QuestObjective(void* q, u32 idx, bool done, bool force = true) {
    if (!q) return;
    if (done) reinterpret_cast<void (*)(void*, u32, void*, u32, bool)>(game::ModuleAddr(Q_OBJ_DONE))(game::FakeVm(), 0, q, idx, true);
    else reinterpret_cast<void (*)(void*, u32, void*, u32, bool, bool)>(game::ModuleAddr(Q_OBJ_SHOW))(game::FakeVm(), 0, q, idx, true, force);
}

/* (07.10, his: "на контрактах сразу три метки") every start of a contract quest put one more entry for its objective in
   the player's objective list (BSTArray of {objective, instance id}, 16 bytes) and the old ones stayed, saved with the
   game: two or three markers. The list is found in the player (an array whose entries' objective +8 is this quest) and
   for each objective of the quest only the newest entry is kept */
void DedupObjectives(void* player, void* q) {
    static int off = -1;
    auto P = static_cast<u8*>(player);
    auto ours = [&](u8* data, u32 i) {
        u8* obj = *reinterpret_cast<u8**>(data + i * 16);
        return obj && game::Readable(obj + 8, 8) && *reinterpret_cast<void**>(obj + 8) == q;
    };
    if (off < 0) {
        for (int o = 0x100; o < 0x1400 && off < 0; o += 8) {
            if (!game::Readable(P + o, 16)) continue;
            u8* data = *reinterpret_cast<u8**>(P + o);
            u32 cap = *reinterpret_cast<u32*>(P + o + 8), n = *reinterpret_cast<u32*>(P + o + 12);
            if (!data || !n || n > cap || cap > 4096 || !game::Readable(data, n * 16)) continue;
            for (u32 i = 0; i < n; i++) if (ours(data, i)) { off = o; break; }
        }
        if (off < 0) return;
        Log("задачи игрока: список на +%x", off);
    }
    u8* data = *reinterpret_cast<u8**>(P + off);
    u32& n = *reinterpret_cast<u32*>(P + off + 12);
    if (!data || n > 4096 || !game::Readable(data, n * 16)) return;
    u32 removed = 0;
    for (int i = (int)n - 1; i >= 0; i--) {                   // newest last: an older entry of the same objective goes
        if (!ours(data, i)) continue;
        void* obj = *reinterpret_cast<void**>(data + i * 16);
        for (int j = i - 1; j >= 0; j--)
            if (*reinterpret_cast<void**>(data + j * 16) == obj) {
                memmove(data + j * 16, data + (j + 1) * 16, (n - j - 1) * 16);
                n--; i--; removed++;
            }
    }
    if (removed) Log("контракт: лишних меток убрано %u", removed);
}

/* (03.10) the contract on the map: Quest.Start through the no-op VM did not start it for real (no marker, not in the
   journal) — started by the engine's own start (as the SetCurrentStageID native does) and made the active quest */
void QuestStartReal(void* q) {
    static const u32 START_SIG[4] = {0xD100C3FF, 0xA9017BFD, 0x910043FD, 0xA9024FF4};      // 0x28df8c (quest, bool& queued, bool)
    static const u32 ACTIVE_SIG[4] = {0xAA0203E0, 0x12000061, 0x17E06EB5, 0x00000000};     // Quest.SetActive 0xa70b58
    static uintptr_t start, active;                 // (plain statics: a guarded static init pulls in 25 KB of runtime)
    static bool tried;
    if (!tried) { tried = true; start = game::Checked177(0x28df8c, START_SIG); active = game::Checked177(0xa70b58, ACTIVE_SIG); }
    if (!q) return;
    if (start && !(static_cast<u8*>(q)[0xC4] & 1)) {
        bool queued = false;
        bool ok = reinterpret_cast<bool (*)(void*, bool*, bool)>(start)(q, &queued, false);
        Log("contract quest start: %d (queued %d)", ok, queued);
    }
    if (active) reinterpret_cast<void (*)(void*, u32, void*, bool)>(active)(game::FakeVm(), 0, q, true);
}

/* a contract read: its quest starts (the objective points at the boss's place), the paper goes back to her */
bool IsDead(void* a);
extern const u32 G_WB_DEAD;
void StartContract(void* player, int k, u8* book, u64 now) {
    if (GetG(G_BOUNTY_STAGE) > 0.5f) {
        game::Notify("Сначала закончи прежний контракт Ша'джары");
        Log("contract %d: refused, another one is on (stage %.0f, boss %.0f)", k, GetG(G_BOUNTY_STAGE), GetG(G_BOUNTY_INDEX));
        return;
    }
    void* q = BossQuest(k);
    if (!q) return;
    /* (05.10) the same boss again: it may still lie dead from the last time (it is back 3 days after) — the contract
       counted it as done at once. Now it comes back at its place for the contract (G_WB_DEAD -1: WorldBosses brings
       it out when the player is near), and only a death after it was seen alive completes the contract */
    if (auto wb = static_cast<u8*>(Form(((u32)g_patchIdx << 24) | (0x1240 + k))))
        if ((!(At<u8>(wb, 0x11) & 0x08) && IsDead(wb)) || GetG(G_WB_DEAD + k) > 0.0f) SetGlobal(G_WB_DEAD + k, -1.0f);
    QuestCall(Q_RESET, q);
    QuestStartReal(q);                                   // (07.10) one start (Quest.Start as well made a second instance)
    QuestStage(q, 10);                                   // the objective: Bounty() once the quest runs (03.10: it
    SetGlobal(G_BOUNTY_STAGE, 1.0f);                     // was shown here, there and every minute — one message now)
    SetGlobal(G_BOUNTY_INDEX, (float)k);
    SetGlobal(G_BOUNTY_REF, 0.0f);
    trade::AddItems(player, book, -1);
    g_closeBookAt = now + 2500;
    char msg[200];
    snprintf(msg, sizeof msg, "<font color='#FF5A3C' size='26'>Контракт принят: %s</font>", BOSS_NAME[k]);
    game::Notify(msg);
    Log("контракт %d принят: %s", k, BOSS_NAME[k]);
}

/* ---- world bosses (02.10, the user: bosses with a meaning, back after a while, loot every time - a grind) ----
   Each contract boss lives at its place with a retinue: it comes out when the player is near (Tamriel, 4000 units),
   is back 3 game days after its death, and its body always holds loot. A contract on it pays her reward on top.
   State in SwitchPatch globals (saved with the game): G_WB_DEAD + k = the game day it died (0: alive / not out),
   G_WB_REF + k = its reference (low 24 bits). Same code offline and online. */
extern const u32 G_WB_DEAD = 0x1230;
constexpr u32 G_WB_REF = 0x1238;
/* test aid (console "wbtest <k> <marker FormID>", Eden only): boss k's place moved to a reference the teleport reaches
   safely, any worldspace (the map-marker teleport of the test tools crashed Eden by itself, 02.10) */
volatile int g_wbTestK = -1; volatile u32 g_wbTestMarker;
constexpr float WB_RESPAWN_DAYS = 3.0f;
struct Retinue { u32 lvln; int n; };
constexpr Retinue WB_RETINUE[BOSSES] = {
    {0x055955, 4},                // draugr (LCharDraugrMeleeAllMale) around the dragon priest
    {0x0B83C2, 4},                // wolves (LCharWolf) with the frost giant
    {0x0442C8, 3},                // Forsworn (LCharForswornMeleeFemale) with the hagraven
    {0x10EC90, 3},                // dwarven spiders (LCharDwarvenSpider) with the centurion
    {0, 0},                       // the lurker comes alone
    {0x033973, 3},                // vampires (LCharVampire) with the gargoyle
};
/* the loot pools (vanilla leveled lists): enchanted weapon, heavy / light armour, jewellery, gems */
constexpr u32 WB_LOOT[] = {0x000ED4, 0x1031C0, 0x1031C1, 0x101072, 0x10E992};

float GameDay() {
    auto days = Form(GLOBAL_DAYS);
    return (days ? At<float>(days, 0x34) : 0.0f) + g_dayOffset;
}

bool IsDead(void* a) { return reinterpret_cast<bool (*)(void*, u32, void*)>(game::ModuleAddr(A_IS_DEAD))(game::FakeVm(), 0, a); }

/* the world boss k's reference if it is out (its base checked), else null */
u8* WorldBossRef(int k, void* bossBase) {
    (void)bossBase;
    auto boss = static_cast<u8*>(Form(((u32)g_patchIdx << 24) | (0x1240 + k)));
    return boss && !(At<u8>(boss, 0x11) & 0x08) ? boss : nullptr;      /* out (enabled) */
}

/* one of her chest's things (a unique or a scroll she still has), taken out of the chest; null if none */
void* FromHerChest(bool scroll, u32 r) {
    void* chest = Form(((u32)g_patchIdx << 24) | SMUGGLER_CHEST);
    if (!chest) return nullptr;
    trade::Count c[128];
    int n = trade::Changes(chest, c, 128);
    u32 cand[64]; int m = 0;
    if (scroll) {
        for (int i = 0; i < g_count && m < 64; i++) {
            u32 id = Id(g_scrolls[i].book);
            s32 d = 0;
            for (int j = 0; j < n; j++) if (c[j].id == id) d = c[j].n;
            if (1 + d > 0) cand[m++] = id;
        }
    } else {
        for (int u = 0; u < NU && m < 64; u++) {
            if (!g_uniqueId[u]) continue;
            s32 d = 0;
            for (int j = 0; j < n; j++) if (c[j].id == g_uniqueId[u]) d = c[j].n;
            if (1 + d > 0) cand[m++] = g_uniqueId[u];
        }
    }
    if (!m) return nullptr;
    void* item = Form(cand[r % m]);
    if (item) trade::AddItems(chest, item, -1);
    return item;
}

void WorldBossLoot(int k, u8* boss, u64 now) {
    u32 r = (u32)((now * 2654435761ull) >> 5) ^ (u32)(k * 0x9E3779B9u);
    if (void* g = Form(0xF)) trade::AddItems(boss, g, 1500 + (int)(r % 2501));
    for (int i = 0; i < 2; i++) {
        r = r * 1103515245u + 12345u;
        if (void* l = Form(WB_LOOT[(r >> 8) % (sizeof WB_LOOT / sizeof WB_LOOT[0])])) trade::AddItems(boss, l, 1);
    }
    r = r * 1103515245u + 12345u;
    if ((r >> 8) % 100 < 15)
        if (void* u = FromHerChest(false, r >> 12)) { trade::AddItems(boss, u, 1); Log("world boss %d: rare %08x", k, Id(u)); }
    r = r * 1103515245u + 12345u;
    if ((r >> 8) % 100 < 5)
        if (void* sc = FromHerChest(true, r >> 12)) { trade::AddItems(boss, sc, 1); Log("world boss %d: scroll %08x", k, Id(sc)); }
}

/* The world bosses are references of SwitchPatch (03.10, bounty.py: boss k 0x1240 + k, retinue 0x1250 + 4k + i,
   persistent, initially disabled): the same FormIDs on every console, so online their death, health and zone are
   shared like any vanilla actor's, and each console puts its own loot on its own copy of the body. */
constexpr u32 WB_REF = 0x1240, WB_GUARD = 0x1250;
constexpr Fn REF_ENABLE{0xa4f440, {0xAA0003E8, 0x2A0103E4, 0x12000061, 0xAA0203E0}};      // ObjectReference.Enable (vm, stack, ref, fade)
constexpr Fn WB_RESURRECT{0xa02670, {0xD10143FF, 0xA9017BFD, 0x910043FD, 0xF90013F7}};   // Actor.Resurrect (vm, stack, actor)
uintptr_t g_refEnable, g_resurrect;

bool Disabled(const void* ref) { return (At<u8>(ref, 0x11) & 0x08) != 0; }   // TESForm flags bit 11 (IsDisabled)

void BringBack(u8* a, void* marker, float dx, float dy) {
    if (!a) return;
    if (Disabled(a) && g_refEnable) reinterpret_cast<void (*)(void*, u32, void*, bool)>(g_refEnable)(game::FakeVm(), 0, a, false);
    if (IsDead(a) && g_resurrect) reinterpret_cast<void (*)(void*, u32, void*)>(g_resurrect)(game::FakeVm(), 0, a);
    if (marker && g_moveTo)
        reinterpret_cast<void (*)(void*, u32, void*, void*, float, float, float, bool)>(g_moveTo)(game::FakeVm(), 0, a, marker, dx, dy, 150.0f, true);
}

void WorldBosses(void* player, u64 now) {
    static u64 at;
    if (now - at < 1000 || g_patchIdx < 0 || !game::Settled()) return;   // (02.10: one came out at the main menu)
    at = now;
    if (!g_refEnable) {
        g_refEnable = game::Checked177(REF_ENABLE.off, REF_ENABLE.sig);
        g_resurrect = game::Checked177(WB_RESURRECT.off, WB_RESURRECT.sig);
    }
    u32 cell, world; float pos[3];
    if (!game::WhereAmI(&cell, &world, pos) || (world != TAMRIEL && g_wbTestK < 0)) return;
    float today = GameDay();
    for (int k = 0; k < BOSSES; k++) {
        auto boss = static_cast<u8*>(Form(((u32)g_patchIdx << 24) | (WB_REF + k)));
        if (!boss) continue;
        float dead = GetG(G_WB_DEAD + k);                   /* > 0: the day it died; 0: alive or never out */
        bool out = !Disabled(boss);
        /* (07.10, his: "после убийства по контракту босса тело исчезает и сразу появляется второй") a contract (-1: "bring
           it out") taken while it already stood there alive: the mark stayed, its death was not counted and it was
           brought out again at once; the same when the game itself brought it back (a cell reset) with its death day
           still set. Out and alive = out: its next death counts */
        if (dead != 0.0f && out && !IsDead(boss)) { SetGlobal(G_WB_DEAD + k, 0.0f); dead = 0.0f; }
        static u64 broughtAt[BOSSES];
        if (out && dead > -0.5f && dead <= 0.0f && IsDead(boss) && now - broughtAt[k] > 5000) {   /* just killed (here or by another player): loot, the clock; not the old body a moment after it was brought back */
            WorldBossLoot(k, boss, now);
            SetGlobal(G_WB_DEAD + k, today > 0.01f ? today : 0.01f);
            char msg[200];
            snprintf(msg, sizeof msg, "<font color='#FFD24A' size='26'>%s повержен. Вернётся через %d дня.</font>",
                     BOSS_NAME[k], (int)WB_RESPAWN_DAYS);
            game::Notify(msg);
            Log("world boss %d killed (day %.2f)", k, today);
            continue;
        }
        if (out && !IsDead(boss)) continue;                  /* out and alive */
        if (dead > 0.0f && today - dead < WB_RESPAWN_DAYS) continue;   /* not back yet */
        bool test = k == g_wbTestK;
        void* marker = Form(test ? g_wbTestMarker : BOSS_MARKER[k]);
        if (!marker || game::WorldOf(marker) != game::WorldOf(player)) continue;
        const float* mp = game::RefPos(marker);
        float dx = mp[0] - pos[0], dy = mp[1] - pos[1];
        static u64 nearSince[BOSSES];
        if (dx * dx + dy * dy > 4000.0f * 4000.0f) { nearSince[k] = 0; continue; }
        if (!nearSince[k]) nearSince[k] = now;
        if (now - nearSince[k] < 8000) continue;             /* the cell must be loaded (02.10) */
        BringBack(boss, marker, 250.0f, 250.0f);
        broughtAt[k] = now;
        SetGlobal(G_WB_DEAD + k, 0.0f);
        SetGlobal(G_WB_REF + k, (float)(Id(boss) & 0xFFFFFF));
        SetAv(boss, "Health", BOSS_HP[k]);
        SetAv(boss, "AttackDamageMult", 2.5f);
        SetAv(boss, "DamageResist", 400.0f);
        SetAv(boss, "MagicResist", 40.0f);
        reinterpret_cast<void (*)(void*, u32, void*, float)>(game::ModuleAddr(R_SET_SCALE))(game::FakeVm(), 0, boss, 1.3f);
        const Retinue& rt = WB_RETINUE[k];
        for (int i = 0; i < rt.n; i++) {
            float a = 6.2832f * i / rt.n;
            BringBack(static_cast<u8*>(Form(((u32)g_patchIdx << 24) | (WB_GUARD + 4 * k + i))), marker,
                      250.0f + 450.0f * cosf(a), 250.0f + 450.0f * sinf(a));
        }
        char msg[200];
        snprintf(msg, sizeof msg, "<font color='#FF5A3C' size='26'>%s где-то рядом...</font>", BOSS_NAME[k]);
        game::Notify(msg);
        Log("world boss %d out (%08x), retinue %d, world %08x, %.0f units from its place", k, Id(boss), rt.n, world,
            sqrtf(dx * dx + dy * dy));
    }
}

/* ---- grind spots (03.10): a mini-boss with a retinue at a place in each hold (bounty.py MINI). No contract;
   back 2 game days after its death; smaller loot, every time. Same refs on every console (online: shared). */
constexpr int MINIS = 6;
constexpr u32 MB_NPC_REF = 0x1280, MB_GUARD = 0x1290, G_MB_DEAD = 0x12B0, G_MB_REF = 0x12B8;
constexpr u32 MB_MARKER[MINIS] = {0x016261, 0x016385, 0x016241, 0x0162B3, 0x0C2EEF, 0x0162CB};
constexpr float MB_HP[MINIS] = {3000, 3500, 2500, 3000, 2500, 4000};
constexpr int MB_GUARDS[MINIS] = {3, 2, 3, 3, 3, 3};
constexpr const char* MB_NAME[MINIS] = {"Грул Кровавый Топор", "Седой Тролль", "Матерь Чащи", "Белый Клык",
                                        "Морвен Пепельный", "Скьольд Неупокоенный"};
constexpr float MB_RESPAWN_DAYS = 2.0f;

void MiniBossLoot(int m, u8* boss, u64 now) {
    u32 r = (u32)((now * 2654435761ull) >> 7) ^ (u32)(m * 0x85EBCA6Bu);
    if (void* g = Form(0xF)) trade::AddItems(boss, g, 400 + (int)(r % 801));
    r = r * 1103515245u + 12345u;
    if (void* l = Form(WB_LOOT[(r >> 8) % (sizeof WB_LOOT / sizeof WB_LOOT[0])])) trade::AddItems(boss, l, 1);
    r = r * 1103515245u + 12345u;
    if ((r >> 8) % 100 < 5)
        if (void* u = FromHerChest(false, r >> 12)) { trade::AddItems(boss, u, 1); Log("mini boss %d: rare %08x", m, Id(u)); }
    r = r * 1103515245u + 12345u;
    if ((r >> 8) % 100 < 2)
        if (void* sc = FromHerChest(true, r >> 12)) { trade::AddItems(boss, sc, 1); Log("mini boss %d: scroll %08x", m, Id(sc)); }
}

void MiniBosses(void* player, u64 now) {
    static u64 at;
    if (now - at < 1000 || g_patchIdx < 0 || !game::Settled()) return;
    at = now;
    if (!g_refEnable) return;                                /* WorldBosses resolves the functions */
    u32 cell, world; float pos[3];
    if (!game::WhereAmI(&cell, &world, pos) || (world != TAMRIEL && g_wbTestK < 100)) return;
    float today = GameDay();
    for (int m = 0; m < MINIS; m++) {
        auto boss = static_cast<u8*>(Form(((u32)g_patchIdx << 24) | (MB_NPC_REF + m)));
        if (!boss) continue;
        float dead = GetG(G_MB_DEAD + m);
        bool out = !Disabled(boss);
        if (dead != 0.0f && out && !IsDead(boss)) { SetGlobal(G_MB_DEAD + m, 0.0f); dead = 0.0f; }   // (07.10) as WorldBosses: out and alive = out
        static u64 mbBroughtAt[MINIS];
        if (out && dead <= 0.0f && IsDead(boss) && now - mbBroughtAt[m] > 5000) {
            MiniBossLoot(m, boss, now);
            SetGlobal(G_MB_DEAD + m, today > 0.01f ? today : 0.01f);
            char msg[200];
            snprintf(msg, sizeof msg, "<font color='#FFD24A' size='24'>%s повержен. Вернётся через %d дня.</font>",
                     MB_NAME[m], (int)MB_RESPAWN_DAYS);
            game::Notify(msg);
            Log("mini boss %d killed (day %.2f)", m, today);
            continue;
        }
        if (out && !IsDead(boss)) continue;
        if (dead > 0.0f && today - dead < MB_RESPAWN_DAYS) continue;
        void* marker = Form(g_wbTestK == 100 + m ? g_wbTestMarker : MB_MARKER[m]);   /* wbtest 100+m <ref> */
        if (!marker || game::WorldOf(marker) != game::WorldOf(player)) continue;
        const float* mp = game::RefPos(marker);
        float dx = mp[0] - pos[0], dy = mp[1] - pos[1];
        static u64 nearSince[MINIS];
        if (dx * dx + dy * dy > 4000.0f * 4000.0f) { nearSince[m] = 0; continue; }
        if (!nearSince[m]) nearSince[m] = now;
        if (now - nearSince[m] < 8000) continue;
        BringBack(boss, marker, 250.0f, 250.0f);
        mbBroughtAt[m] = now;
        SetGlobal(G_MB_DEAD + m, 0.0f);
        SetGlobal(G_MB_REF + m, (float)(Id(boss) & 0xFFFFFF));
        SetAv(boss, "Health", MB_HP[m]);
        SetAv(boss, "AttackDamageMult", 1.6f);
        SetAv(boss, "DamageResist", 200.0f);
        reinterpret_cast<void (*)(void*, u32, void*, float)>(game::ModuleAddr(R_SET_SCALE))(game::FakeVm(), 0, boss, 1.15f);
        for (int i = 0; i < MB_GUARDS[m]; i++) {
            float a = 6.2832f * i / MB_GUARDS[m];
            BringBack(static_cast<u8*>(Form(((u32)g_patchIdx << 24) | (MB_GUARD + 4 * m + i))), marker,
                      250.0f + 400.0f * cosf(a), 250.0f + 400.0f * sinf(a));
        }
        char msg[200];
        snprintf(msg, sizeof msg, "<font color='#FF9A3C' size='24'>Здесь хозяйничает %s...</font>", MB_NAME[m]);
        game::Notify(msg);
        Log("mini boss %d out (%08x), %.0f units from its place", m, Id(boss), sqrtf(dx * dx + dy * dy));
    }
}

void Bounty(void* player, u64 now) {
    static u64 at;
    if (now - at < 1000 || g_patchIdx < 0) return;
    at = now;
    int stage = (int)(GetG(G_BOUNTY_STAGE) + 0.5f), k = (int)(GetG(G_BOUNTY_INDEX) + 0.5f);
    if (stage == 0 || k < 0 || k >= BOSSES) return;
    /* (03.10) the contract's journal entry and map marker: its start is queued (aliases first), so the stage and
       the objective are set once the quest runs; again after a load (a save keeps the quest, not always the marker) */
    {
        auto q = static_cast<u8*>(BossQuest(k));
        /* (06.10, his: "взял контракт, а маркера нету") the contract is on but its quest is not running (a save made
           while its start was queued, or a start that did not take): start it again — the marker comes with it */
        static u64 restartAt; static int restarts; static void* restartQ;
        if (restartQ != q) { restartQ = q; restarts = 0; }
        if (q && !(q[0xC4] & 1) && restarts < 3 && now - restartAt > 15000) {
            restartAt = now; restarts++;
            QuestStartReal(q);                           // (07.10) one start: Quest.Start + the real start gave two instances (2-3 markers)
            Log("contract %d: its quest was not running — started again (%d)", k, restarts);
        }
        static void* shownQ; static int shownStage; static u64 shownAt;
        if (q && (q[0xC4] & 1) && (shownQ != q || shownStage != stage || now - shownAt > 60000)) {
            if (*reinterpret_cast<u16*>(q + 0x1B8) < 10) QuestStage(q, 10);
            /* not forced: an objective already up stays silent (the minute's check only brings a lost marker back) */
            u32 obj = stage == 1 ? 10 : 20;                   // (10 was completed where the boss fell)
            QuestObjective(q, obj, false, false);
            DedupObjectives(game::Player(), q);
            QuestStartReal(q);                           // (running: only makes it active)
            if (shownQ != q || shownStage != stage) Log("contract %d: journal + marker (stage %d)", k, stage);
            shownQ = q; shownStage = stage; shownAt = now;
        }
    }
    void* bossBase = Form(((u32)g_patchIdx << 24) | (BOSS_BASE + k));
    u32 refLow = (u32)GetG(G_BOUNTY_REF);
    auto boss = refLow ? static_cast<u8*>(Form(0xFF000000 | refLow)) : nullptr;
    if (boss && (Type(boss) != 0x3E || *reinterpret_cast<void**>(boss + 0x40) != bossBase)) boss = nullptr;
    if (stage == 1) {
        /* the world boss of this contract (WorldBosses brings it out; the contract only watches it) */
        boss = WorldBossRef(k, bossBase);
        if (!boss) return;
        /* seen alive since the contract began (G_BOUNTY_REF: 0 until then) — a body from before does not count */
        if (!IsDead(boss) && (u32)GetG(G_BOUNTY_REF) != (Id(boss) & 0xFFFFFF)) SetGlobal(G_BOUNTY_REF, (float)(Id(boss) & 0xFFFFFF));
        if ((u32)GetG(G_BOUNTY_REF) != (Id(boss) & 0xFFFFFF)) return;
        /* its golden aura (Dragonborn's Dragon Aspect shader), kept on while it is loaded */
        static u64 auraAt;
        static int db = -2;
        if (db == -2) db = game::PluginIndex("Dragonborn.esm");
        if (g_shader && db >= 0 && k != 3 && At<void*>(boss, 0x68) && now - auraAt > 30000) {   // (05.10) not on the centurion: suspected of the crash at Bthalft
            auraAt = now;
            if (void* fx = Form(((u32)db << 24) | 0x021735))
                reinterpret_cast<void (*)(void*, u32, void*, void*, float)>(g_shader)(game::FakeVm(), 0, fx, boss, 35.0f);
        }
        if (reinterpret_cast<bool (*)(void*, u32, void*)>(game::ModuleAddr(A_IS_DEAD))(game::FakeVm(), 0, boss)) {
            void* q = BossQuest(k);
            QuestObjective(q, 10, true);
            QuestStage(q, 20);
            QuestObjective(q, 20, false, false);
            SetGlobal(G_BOUNTY_STAGE, 2.0f);
            char msg[200];
            snprintf(msg, sizeof msg, "<font color='#FFD24A' size='26'>%s повержен. Ша'джара ждёт.</font>", BOSS_NAME[k]);
            game::Notify(msg);
            Log("контракт %d: босс убит", k);
        }
        return;
    }
    /* stage 2: next to her she pays — gold or one rare thing from her chest */
    void* her = Form(((u32)g_patchIdx << 24) | SMUGGLER_REF);
    if (!her || At<void*>(her, game::Get().offCell) != At<void*>(player, game::Get().offCell)) return;
    const float* hp = game::RefPos(her);
    const float* pp = game::RefPos(player);
    float dx = hp[0] - pp[0], dy = hp[1] - pp[1];
    if (dx * dx + dy * dy > 350.0f * 350.0f) return;
    void* topic = Form(((u32)g_patchIdx << 24) | REWARD_TOPIC);
    if (topic) reinterpret_cast<void (*)(void*, u32, void*, void*, void*, bool)>(game::ModuleAddr(SAY.off))(game::FakeVm(), 0, her, topic, nullptr, false);
    u32 r = (u32)((now * 2654435761ull) >> 7);
    void* chest = Form(((u32)g_patchIdx << 24) | SMUGGLER_CHEST);
    void* item = nullptr;
    if (chest && r % 10 >= 6) {
        /* a unique she still has (her chest has one of each; the delta says if it is gone) */
        trade::Count c[128];
        int n = trade::Changes(chest, c, 128);
        int cand[NU], m = 0;
        for (int u = 0; u < NU; u++) {
            if (!g_uniqueId[u]) continue;
            s32 d = 0;
            for (int j = 0; j < n; j++) if (c[j].id == g_uniqueId[u]) d = c[j].n;
            if (1 + d > 0) cand[m++] = u;
        }
        if (m) item = Form(g_uniqueId[cand[(r / 10) % m]]);
    }
    if (item) {
        trade::AddItems(chest, item, -1);
        trade::AddItems(player, item, 1);
        game::Notify("<font color='#FFD24A' size='26'>Награда: редкая вещь из сундука Ша'джары</font>");
        Log("контракт %d: награда — предмет %08x", k, Id(item));
    } else {
        int gold = 20000 + (int)((r / 7) % 10001);
        if (void* g = Form(0xF)) trade::AddItems(player, g, gold);
        char msg[160];
        snprintf(msg, sizeof msg, "<font color='#FFD24A' size='26'>Награда: %d золотых</font>", gold);
        game::Notify(msg);
        Log("контракт %d: награда — %d золотых", k, gold);
    }
    void* q = BossQuest(k);
    QuestObjective(q, 20, true);
    QuestStage(q, 200);
    SetGlobal(G_BOUNTY_STAGE, 0.0f);
    SetGlobal(G_BOUNTY_REF, 0.0f);
}

}  // namespace

void AddDays(float n) { g_dayOffset += n; g_shopCheck = 0; }
void WorldBossTest(int k, u32 marker) { g_wbTestMarker = marker; g_wbTestK = k; }

bool Resolve() {
    g_hasSpell = game::Checked177(HAS_SPELL.off, HAS_SPELL.sig);
    g_addSpell = game::Checked177(ADD_SPELL.off, ADD_SPELL.sig);
    g_music = game::Checked177(MUSIC_ADD.off, MUSIC_ADD.sig);
    g_playIdle = game::Checked177(PLAY_IDLE.off, PLAY_IDLE.sig);
    g_sound = game::Checked177(SOUND_PLAY.off, SOUND_PLAY.sig);
    g_inMenu = game::Checked177(IN_MENU.off, IN_MENU.sig);
    g_castNative = game::Checked177(SPELL_CAST_NATIVE.off, SPELL_CAST_NATIVE.sig);
    g_shader = game::Checked177(SHADER_PLAY.off, SHADER_PLAY.sig);
    g_incSkill = game::Checked177(INCREMENT_SKILL.off, INCREMENT_SKILL.sig);
    g_moveTo = game::Checked177(MOVE_TO.off, MOVE_TO.sig);
    g_evalPkg = game::Checked177(EVALUATE_PACKAGE.off, EVALUATE_PACKAGE.sig);
    g_questStart = game::Checked177(QUEST_START.off, QUEST_START.sig);
    g_disable = game::Checked177(DISABLE_CONTROLS.off, DISABLE_CONTROLS.sig);
    g_enable = game::Checked177(ENABLE_CONTROLS.off, ENABLE_CONTROLS.sig);
    g_first = game::Checked177(FIRST_PERSON.off, FIRST_PERSON.sig);
    g_shake = game::Checked177(SHAKE_CAMERA.off, SHAKE_CAMERA.sig);
    g_vfx = game::Checked177(VFX_PLAY.off, VFX_PLAY.sig);
    g_imod = game::Checked177(IMOD_APPLY.off, IMOD_APPLY.sig);
    g_equip = game::ModuleAddr(EQUIP_ITEM.off);
    g_unequip = game::Checked177(UNEQUIP.off, UNEQUIP.sig);
    g_removeSpell = game::Checked177(REMOVE_SPELL.off, REMOVE_SPELL.sig);
    g_hasShout = game::Checked177(HAS_SHOUT.off, HAS_SHOUT.sig);
    g_addShout = game::Checked177(ADD_SHOUT.off, ADD_SHOUT.sig);
    g_teachWord = game::Checked177(TEACH_WORD.off, TEACH_WORD.sig);
    g_unlockWord = game::Checked177(UNLOCK_WORD.off, UNLOCK_WORD.sig);
    g_itemCount = game::Checked177(ITEM_COUNT.off, ITEM_COUNT.sig);
    g_ok = g_hasSpell && g_addSpell;
    return g_ok;
}

/* ---- a dragon's soul when the vanilla absorption got stuck (03.10, his console save: the Shearpoint dragon lay dead
   next to him, Dragon Rising done, and nothing happened — DragonActorScript's death sequence is known to hang).
   A dead dragon (DragonRace / Dragonborn's black dragons) within 1500 units for 20 s while the soul count did not
   grow: the module gives the soul itself. The last 16 such dragons are kept in SwitchPatch globals 0x12D0.. (low 24
   bits of the FormID) so a reload never gives a second one. */
constexpr u32 G_DRAGON_DONE = 0x12D0;
constexpr Fn GET_AV_D{0x9fe0f0, {0xD10783FF, 0xA91A7BFD, 0x910683FD, 0xF900DBFC}};    // Actor.GetActorValue
constexpr Fn MOD_AV_D{0xa01770, {0xD10783FF, 0xFD00D3E8, 0xF900DFFC, 0xA91AFBFD}};    // Actor.ModActorValue

bool DragonDone(u32 id) {
    for (int i = 0; i < 16; i++) if ((u32)GetG(G_DRAGON_DONE + i) == (id & 0xFFFFFF)) return true;
    return false;
}

void DragonSouls(void* player, u64 now) {
    static u64 at;
    static uintptr_t getAv, modAv;
    static bool tried;
    static FixedString fs;
    static u32 pendingId; static u64 pendingSince; static float soulsThen;
    if (now - at < 2000) return;
    at = now;
    if (!tried) {
        tried = true;
        getAv = game::Checked177(GET_AV_D.off, GET_AV_D.sig);
        modAv = game::Checked177(MOD_AV_D.off, MOD_AV_D.sig);
        reinterpret_cast<void (*)(FixedString*, const char*)>(game::ModuleAddr(FS_CTOR))(&fs, "DragonSouls");
    }
    if (!getAv || !modAv || g_patchIdx < 0) return;
    float souls = reinterpret_cast<float (*)(void*, u32, void*, FixedString*)>(getAv)(game::FakeVm(), 0, player, &fs);
    static int db = -2;
    if (db == -2) db = game::PluginIndex("Dragonborn.esm");
    const float* me = game::RefPos(player);
    void* actors[256];
    int n = game::HighActors(actors, 256);
    u32 found = 0;
    for (int i = 0; i < n && !found; i++) {
        auto a = static_cast<u8*>(actors[i]);
        if (!a || a == player || !game::Readable(a + 0x228, 8)) continue;
        void* race = *reinterpret_cast<void**>(a + 0x228);
        if (!race || !game::Readable(race, 0x18)) continue;
        u32 rid = Id(race);
        bool dragon = rid == 0x00012E82 || (db >= 0 && (rid == (((u32)db << 24) | 0x02C88C) || rid == (((u32)db << 24) | 0x02C88B)));
        if (!dragon || !IsDead(a) || DragonDone(Id(a))) continue;
        const float* p = game::RefPos(a);
        float dx = p[0] - me[0], dy = p[1] - me[1];
        if (dx * dx + dy * dy < 1500.0f * 1500.0f) found = Id(a);
    }
    if (!found) { pendingId = 0; return; }
    if (found != pendingId) { pendingId = found; pendingSince = now; soulsThen = souls; return; }
    auto remember = [&](u32 id) {
        for (int i = 15; i > 0; i--) SetGlobal(G_DRAGON_DONE + i, GetG(G_DRAGON_DONE + i - 1));
        SetGlobal(G_DRAGON_DONE, (float)(id & 0xFFFFFF));
    };
    if (souls > soulsThen + 0.5f) { remember(found); pendingId = 0; Log("дракон %08x: душа поглощена игрой", found); return; }
    if (now - pendingSince < 20000) return;
    reinterpret_cast<void (*)(void*, u32, void*, FixedString*, float)>(modAv)(game::FakeVm(), 0, player, &fs, 1.0f);
    remember(found);
    pendingId = 0;
    game::Notify("<font color='#FFD24A' size='26'>Душа дракона поглощена</font>");
    Log("дракон %08x: поглощение не сработало — душа выдана модулем (было %.0f)", found, souls);
}

void Pump() {
    void* player = game::Player();
    if (!g_ok || !player || !game::Settled()) return;
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    if (g_sceneAt) Scene(player, now);
    bool inMenu = g_inMenu && reinterpret_cast<bool (*)(void*, u32)>(g_inMenu)(game::FakeVm(), 0);
    if (g_pageRef && inMenu) g_pageSeen = true;
    if (g_pageRef && !inMenu && !g_pageSeen && now - g_pageAt > 1200 && g_pageTries < 4) {
        g_pageTries++;                                   // the page did not open: once more
        g_pageAt = now;
        trade::ActivateAsPlayer(g_pageRef);
        Log("свиток: страница — попытка %d", g_pageTries);
    } else if (g_pageRef && !inMenu && now - g_pageAt > 1500) {
        /* the page shut: taken ("Взять") — it is in the bag again; left — back into the bag, the laid one gone */
        if (g_sceneBook && Count(player, g_sceneBook) <= g_pageCount) {
            trade::AddItems(player, g_sceneBook, 1, true);
            game::DeleteRef(g_pageRef);
        }
        g_pageRef = nullptr;
        g_skipRead = nullptr;
    }
    DragonSouls(player, now);
    if (now < g_next) return;
    g_next = now + 400;
    if (g_count < 0) Find();
    /* the blessing: hook UseSkill once (its address from the player's vtable), count the hour of play down */
    if (!g_useSkill) {
        g_useSkill = (*reinterpret_cast<uintptr_t**>(player))[VT_USE_SKILL];
        UseSkillHook::InstallAtPtr(g_useSkill);
        Log("благословение: UseSkill +%lx", (unsigned long)(g_useSkill - game::ModuleAddr(0)));
    }
    {
        static u64 lastTick;
        float* left = BlessLeft();
        bool menu = g_inMenu && reinterpret_cast<bool (*)(void*, u32)>(g_inMenu)(game::FakeVm(), 0);
        if (left && *left > 0.0f && lastTick && !menu) {
            *left -= (now - lastTick) / 1000.0f;
            if (*left <= 0.0f) {
                *left = 0.0f;
                void* sp = Form(((u32)g_patchIdx << 24) | BLESS_SPELL);
                if (sp && g_removeSpell) reinterpret_cast<bool (*)(void*, u32, void*, void*)>(g_removeSpell)(game::FakeVm(), 0, player, sp);
                game::Notify("Благословение древнего свитка угасло");
                Log("благословение свитка закончилось");
            }
        }
        static u32 logged;
        if (g_doubled != logged && now - lastTick < 1000) { logged = g_doubled; Debug("благословение: удвоено начислений опыта %u", logged); }
        lastTick = now;
    }
    if (g_closeBookAt && now >= g_closeBookAt) { g_closeBookAt = 0; game::Press(1ull << 1, 150); }
    /* skill books: read -> +5 to each packed skill (5 bits each, av = 6 + index), the book crumbles */
    for (int i = 0; i < g_skillCount; i++) {
        u8* b = g_skillBooks[i].book;
        if (!(b[g_flagsOff] & FLAG_READ)) continue;
        b[g_flagsOff] &= ~FLAG_READ;
        u32 packed = g_skillBooks[i].packed;
        for (int k = 0; k < 4; k++) {
            u32 sk = (packed >> (5 * k)) & 0x1F;
            if (sk && g_incSkill) reinterpret_cast<void (*)(void*, u32, u32)>(g_incSkill)(player, 6 + sk - 1, 5);
        }
        trade::AddItems(player, b, -1);
        g_closeBookAt = now + 700;                            // the page closes: the book is gone
        Sound(SND_DRAGON_SOUL, player);
        game::Notify("<font color='#FFD24A' size='24'>Знание впитано: навыки +5</font>");
        Log("книга навыков прочитана: %08x", packed);
    }
    /* contracts: read -> the hunt starts (one at a time) */
    for (int i = 0; i < g_contractCount; i++) {
        u8* b = g_contracts[i].book;
        if (!(b[g_flagsOff] & FLAG_READ)) continue;
        b[g_flagsOff] &= ~FLAG_READ;
        StartContract(player, (int)g_contracts[i].k, b, now);
    }
    if (g_uniquesReady) { WorldBosses(player, now); MiniBosses(player, now); Bounty(player, now); }
    /* sold uniques: watch the player's counts of them and the gold (every 0.4 s) */
    {
        if (!g_uniquesReady) {
            for (int k = 0; k < NU; k++) {
                int idx = game::PluginIndex(UNIQUES[k].plugin);
                g_uniqueId[k] = idx >= 0 ? ((u32)idx << 24) | UNIQUES[k].obj : 0;
                g_uniqueHave[k] = -1;
            }
            g_uniquesReady = true;
        }
        static trade::Count inv[1024];
        int n = trade::Changes(player, inv, 1024);
        /* (06.10) once, his: "при следующем входе она мне отдала бабки за все, что я ей продал" - what lies in her chest
           and pockets beyond her own stock (Changes() > 0): a unique he does not have went there off his racks (the old
           pick-up; her chest has one of each, this is a second copy): back to him; anything else he sold her and was not
           paid for: paid now at its value (Form.GetGoldValue 0xa3d300), and gone from her. save:/skyrimcoop_ret2.txt = done */
        /* (06.10, his console: the flag file of the profile was set in a session he never saved — his saves got
           nothing) the mark is the save's own: SwitchPatch global SSW_SmugglerSettled 0x12E1 (1 settled, 2 + reset) */
        constexpr u32 G_SETTLED = 0x12E1;
        /* checked after every load (the global is per save), once the player has settled in the world */
        /* (07.10) a new game (still in the intro) has nothing to settle: marked done, no gold — a new character got 200000
           and three things in the cart */
        if (g_patchIdx >= 0 && Form(((u32)g_patchIdx << 24) | G_SETTLED) && GetG(G_SETTLED) < 1.5f && !quests::IntroDone())
            SetGlobal(G_SETTLED, 2.0f);
        if (g_patchIdx >= 0 && Form(((u32)g_patchIdx << 24) | G_SETTLED) && GetG(G_SETTLED) < 1.5f && game::Settled()) {
            void* ch = Form(((u32)g_patchIdx << 24) | SMUGGLER_CHEST);
            void* her = Form(((u32)g_patchIdx << 24) | SMUGGLER_REF);
            /* (07.10) the settlement was his old playthrough's, and it is done in his saves; on his wife's save it gave
               200000 gold and three artifacts she never had. Any other save is only marked */
            if (GetG(G_SETTLED) < 0.5f) { SetGlobal(G_SETTLED, 1.0f); Log("расчёт с контрабандисткой: не нужен (отмечен)"); }
            if (false && ch && her && GetG(G_SETTLED) < 0.5f) {
                int back = 0;
                s32 pay = 0;
                void* holders[2] = {ch, her};
                for (void* hold : holders) {
                    static trade::Count cc[512];
                    int cn = trade::Changes(hold, cc, 512);
                    for (int j = 0; j < cn; j++) {
                        if (cc[j].n <= 0 || cc[j].id == 0xF) continue;
                        if (hold == her && (cc[j].id >> 24) == (u32)g_patchIdx) continue;   // her own outfit (SwitchPatch)
                        void* it = Form(cc[j].id);
                        if (!it) continue;
                        bool unique = false;
                        for (int u = 0; u < NU; u++) if (g_uniqueId[u] == cc[j].id) unique = true;
                        s32 have = 0;
                        for (int q = 0; q < n; q++) if (inv[q].id == cc[j].id) have = inv[q].n;
                        if (unique && have <= 0) {
                            trade::AddItems(hold, it, -1);
                            trade::AddItems(player, it, 1, true);
                            back++;
                            if (cc[j].n == 1) continue;
                            cc[j].n -= 1;
                        }
                        s32 value = reinterpret_cast<s32 (*)(void*, u32, void*)>(game::ModuleAddr(0xa3d300))(game::FakeVm(), 0, it);
                        if (value <= 0) continue;
                        pay += value * cc[j].n;
                        trade::AddItems(hold, it, -cc[j].n);
                    }
                }
                /* his three named ones (her rewards / bought from her, then taken off his racks: no extra copy shows them):
                   Dawnguard Harkon's Sword 0x0067CF, Auriel's Bow 0x000800, Skyrim Dawnbreaker 0x04A38F — back if he has none */
                int dg = game::PluginIndex("Dawnguard.esm");
                const u32 named[3] = {dg >= 0 ? ((u32)dg << 24) | 0x0067CF : 0, dg >= 0 ? ((u32)dg << 24) | 0x000800 : 0, 0x0004A38F};
                for (u32 id : named) {
                    if (!id) continue;
                    s32 have = 0;
                    for (int q = 0; q < n; q++) if (inv[q].id == id) have = inv[q].n;
                    static trade::Count pc[1024];
                    int pn = trade::Changes(player, pc, 1024);
                    for (int q = 0; q < pn; q++) if (pc[q].id == id) have = pc[q].n;      // (after the copies above)
                    if (have > 0) continue;
                    if (void* it = Form(id)) {
                        trade::AddItems(ch, it, -1);                    // from her stock (her base copy)
                        trade::AddItems(player, it, 1, true);
                        back++;
                    }
                }
                pay += 200000;                                          // his: "пусть ещё 200К золотом накинет сверху"
                if (pay > 0) if (void* g = Form(0xF)) trade::AddItems(player, g, pay, true);
                SetGlobal(G_SETTLED, 1.0f);
                if (back || pay) {
                    char msg[200];
                    snprintf(msg, sizeof msg, "Ша'джара вернула вещи со стоек: %d и заплатила за проданное: %d золотых", back, pay);
                    game::Notify(msg);
                }
                Log("расчёт с контрабандисткой: вещей назад %d, золота %d", back, pay);
            }
            /* (06.10) then once: her actor state back to the plugin's (ObjectReference.Reset 0xa54db0) — something kept in
               his save made her take what he sold and pay nothing (a copy of her paid; after Reset she pays too, checked).
               She walks to her shop from her start place. save:/skyrimcoop_vreset1.txt = done */
            if (her && GetG(G_SETTLED) < 1.5f) {
                reinterpret_cast<void (*)(void*, u32, void*, void*)>(game::ModuleAddr(0xa54db0))(game::FakeVm(), 0, her, nullptr);
                g_vendorRedone = false;
                SetGlobal(G_SETTLED, 2.0f);
                Log("контрабандистка: состояние сброшено (торговля как у торговца)");
            }
        }
        s32 gold = 0;
        for (int j = 0; j < n; j++) if (inv[j].id == 0xF) gold = inv[j].n;
        auto days = static_cast<u8*>(Form(GLOBAL_DAYS));
        float today = (days ? At<float>(days, 0x34) : 0.0f) + g_dayOffset;
        for (int k = 0; k < NU; k++) {
            if (!g_uniqueId[k]) continue;
            s32 have = 0;
            for (int j = 0; j < n; j++) if (inv[j].id == g_uniqueId[k]) have = inv[j].n;
            if (g_uniqueHave[k] > have) g_lostAt[k] = now;
            if (g_uniqueHave[k] >= 0 && have > g_uniqueHave[k]) g_gotAt[k] = now;
            /* bought from her = it came while the gold went down: her own line about it, right away */
            if (g_gotAt[k] && g_goldDownAt && (g_gotAt[k] > g_goldDownAt ? g_gotAt[k] - g_goldDownAt : g_goldDownAt - g_gotAt[k]) < 3000) {
                g_gotAt[k] = 0;
                SetGlobal(BOUGHT_GLOBAL, float(k + 1));
                void* her = Form(((u32)g_patchIdx << 24) | SMUGGLER_REF);
                void* topic = Form(((u32)g_patchIdx << 24) | BOUGHT_TOPIC);
                if (her && topic)
                    reinterpret_cast<void (*)(void*, u32, void*, void*, void*, bool)>(game::ModuleAddr(SAY.off))(game::FakeVm(), 0, her, topic, nullptr, false);
                Log("куплена уникальная вещь %08x — реплика %d", g_uniqueId[k], k + 1);
            }
            if (g_gotAt[k] && now - g_gotAt[k] > 3000) g_gotAt[k] = 0;
            if (g_lostAt[k] && now - g_lostAt[k] > 3000) {
                /* (06.10) no gold came: dropped, hung on a rack, put on a display — the player's; it stays where it is
                   (the earlier "a dropped one goes to her in 3 days" took his Harkon's sword off his wall) */
                g_lostAt[k] = 0;
            }
            g_uniqueHave[k] = have;
        }
        if (g_lastGold >= 0 && gold > g_lastGold) g_goldUpAt = now;
        if (g_goldUpAt && now - g_goldUpAt > 3000) g_goldUpAt = 0;
        if (g_lastGold >= 0 && gold < g_lastGold) g_goldDownAt = now;
        if (g_goldDownAt && now - g_goldDownAt > 3000) g_goldDownAt = 0;
        g_lastGold = gold;
        if (g_grumbleDay >= 0 && today - g_grumbleDay > 5.0f)      // five days later she has said it or let it go
            if (auto gr = static_cast<u8*>(Form(((u32)g_patchIdx << 24) | GRUMBLE_GLOBAL))) {
                *reinterpret_cast<float*>(gr + 0x34) = 0.0f;
                SetGlobal(RETURNED_GLOBAL, 0.0f);
                g_grumbleDay = -1.0f;
            }
        void* chest = g_patchIdx >= 0 ? Form(((u32)g_patchIdx << 24) | SMUGGLER_CHEST) : nullptr;
        /* her vendor faction again, once a session and after each move (05.10: in his save she bartered but paid nothing
           and saw no chest, a placed copy of her paid): Actor.RemoveFromFaction 0xa022d0 + SetFactionRank 0xa03390.
           (06.10) the rest is the game's own vendor: her chest respawns (smuggler.py) - no pocket / restock of ours */
        if (void* her = chest ? Form(((u32)g_patchIdx << 24) | SMUGGLER_REF) : nullptr)
            if (!g_vendorRedone)
                if (void* fac = Form(((u32)g_patchIdx << 24) | SMUGGLER_FACTION)) {
                    g_vendorRedone = true;
                    reinterpret_cast<void (*)(void*, u32, void*, void*)>(game::ModuleAddr(0xa022d0))(game::FakeVm(), 0, her, fac);
                    reinterpret_cast<void (*)(void*, u32, void*, void*, s32)>(game::ModuleAddr(0xa03390))(game::FakeVm(), 0, her, fac, 0);
                    Log("контрабандистка: торговая фракция заново");
                }
        /* (07.10, his: "я ей продаю вещи … она мне не платит") her purse is the chest's gold (50000 in the base record,
           smuggler.py): once it was spent she took what he sold for nothing. Kept at 50000 at least, every 5 s */
        static u64 nextPurse;
        u64 nowMs = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
        if (chest && nowMs > nextPurse) {
            nextPurse = nowMs + 5000;
            void* purse[2] = {chest, Form(((u32)g_patchIdx << 24) | SMUGGLER_REF)};   // she pays from her pocket first
            for (void* hold : purse) {
                if (!hold) continue;
                static trade::Count pc[512];
                int pn = trade::Changes(hold, pc, 512);
                s32 delta = 0;
                for (int j = 0; j < pn; j++) if (pc[j].id == 0xF) delta = pc[j].n;
                if (delta < 0)
                    if (void* g = Form(0xF)) {
                        trade::AddItems(hold, g, -delta, true);
                        Log("контрабандистка: кошелёк пополнен на %d", -delta);
                    }
            }
        }
    }
    /* her dialogue quest running (a start-game-enabled quest added to an existing save may stay idle) */
    if (g_questStart && g_patchIdx >= 0) {
        static u64 qAt;
        if (now - qAt > 5000) {
            qAt = now;
            auto q = static_cast<u8*>(Form(((u32)g_patchIdx << 24) | DIALOGUE_QUEST));
            static bool tried;
            if (q && !tried) {                              // (+0xC4 bit 0 also carries "start game enabled")
                tried = true;
                bool queued = false;
                bool ok = reinterpret_cast<bool (*)(void*, bool*, bool)>(g_questStart)(q, &queued, false);
                Log("диалог контрабандистки: квест запущен (%d)", ok);
            }
        }
    }
    /* her shop moves every 10 game days; her skill books come back every 3 days */
    if (now >= g_shopCheck && g_patchIdx >= 0) {
        g_shopCheck = now + 10000;
        auto days = static_cast<u8*>(Form(GLOBAL_DAYS));
        float d = (days ? At<float>(days, 0x34) : 0.0f) + g_dayOffset;
        int shop = (int)(d / 10.0f) % 3;                      // 01.10: Whiterun -> the College -> Markarth, 10 days each
        void* her = Form(((u32)g_patchIdx << 24) | SMUGGLER_REF);
        auto glob = static_cast<u8*>(Form(((u32)g_patchIdx << 24) | SHOP_GLOBAL));
        static int lastShop = -1;
        static float shopSince;
        if (her && glob && shop != lastShop) {                // she sets off on foot (her packages follow the global)
            *reinterpret_cast<float*>(glob + 0x34) = (float)shop;
            if (g_evalPkg) reinterpret_cast<void (*)(void*, u32, void*)>(g_evalPkg)(game::FakeVm(), 0, her);
            if (lastShop >= 0) Log("контрабандистка отправилась: %s", SHOP_NAME[shop]);
            lastShop = shop; shopSince = d;
        }
        /* stuck on the road for 6 days (a blocked path): she gets there anyway, never in sight of the player */
                void* marker = Form(((u32)g_patchIdx << 24) | (SHOP_MARKER + shop));   // our square markers 0xE00..0xE02
        static bool placed;                                   // the first check of a session: she starts where she belongs
        if (her && marker && g_moveTo && (d - shopSince > 6.0f || !placed)) {
            placed = true;
            void* herCell = At<void*>(her, game::Get().offCell);
            void* markCell = At<void*>(marker, game::Get().offCell);
            void* myCell = At<void*>(player, game::Get().offCell);
            const float* hp = game::RefPos(her); const float* mp = game::RefPos(marker);
            float dx = hp[0] - mp[0], dy = hp[1] - mp[1];
            bool there = dx * dx + dy * dy < 1500.0f * 1500.0f;   // (the marker sits in the persistent cell)
            if (!there && herCell != myCell && markCell != myCell) {
                reinterpret_cast<void (*)(void*, u32, void*, void*, float, float, float, bool)>(g_moveTo)(
                    game::FakeVm(), 0, her, marker, 0.0f, 0.0f, 0.0f, true);
                Log("контрабандистка перенесена: %s", SHOP_NAME[shop]);
                g_vendorRedone = false;                         // her vendor faction again after the move
            }
            shopSince = d;
        }
    }
    /* the smuggler wears Miraak's Dragon Aspect all the time (Dragonborn 0x03D5B2, lasts 5 min): cast on her when
       her 3D appears and again every 4 min while she is loaded */
    static void* lastHer3d;
    if (false && g_castNative && now >= g_auraAt && g_patchIdx >= 0) {   // 01.10: no Dragon Aspect on her any more
        g_auraAt = now + 3000;
        static int db = -2;
        if (db == -2) db = game::PluginIndex("Dragonborn.esm");
        void* her = Form(((u32)g_patchIdx << 24) | SMUGGLER_REF);
        void* her3d = her && Type(her) == 0x3E ? At<void*>(her, 0x68) : nullptr;
        static u64 castAt;
        void* aspect = db >= 0 ? Form(((u32)db << 24) | 0x03D5B2) : nullptr;
        if (her3d && aspect && (her3d != lastHer3d || now - castAt > 240000)) {
            lastHer3d = her3d; castAt = now;
            reinterpret_cast<void (*)(void*, u32, void*, void*, void*)>(g_castNative)(game::FakeVm(), 0, aspect, her, her);
            Debug("контрабандистка: Воплощение дракона");
        }
        if (!her3d) lastHer3d = nullptr;
    }
    for (int i = 0; i < g_count; i++) {
        u8* b = g_scrolls[i].book;
        if (!(b[g_flagsOff] & FLAG_READ)) continue;
        b[g_flagsOff] &= ~FLAG_READ;                         // read again later (another character, a clanmate) works
        if (b == g_skipRead) { g_skipRead = nullptr; continue; }   // our own reopening of the page
        void* sp = Form(g_scrolls[i].spell);
        if (!sp || g_sceneAt) continue;
        g_sceneBook = b;
        g_fromBag = Count(player, b) > 0;
        g_known = false;
        Log("свиток открыт: в сумке %d, функция счёта %s", Count(player, b), g_itemCount ? "есть" : "нет");
        if (Type(sp) == FORM_SHOU) {
            /* a shout scroll: the shout, all three words learned and unlocked (no dragon souls), the words in runes */
            if (!g_addShout || !g_teachWord || !g_unlockWord) continue;
            bool had = g_hasShout && reinterpret_cast<bool (*)(void*, void*)>(g_hasShout)(player, sp);
            if (!had) reinterpret_cast<void (*)(void*, void*)>(g_addShout)(player, sp);
            game::Notify("##SSWFX##20");                  // our shimmer over the game's own "word learned" titles
            g_noBanner = true;
            char runes[160];
            size_t o = 0;
            for (u32 w = 1; w <= 3; w++) {
                void* word = Form(g_scrolls[i].spell + w);
                if (!word) continue;
                reinterpret_cast<void (*)(void*, u32, void*, void*)>(g_teachWord)(game::FakeVm(), 0, nullptr, word);
                reinterpret_cast<void (*)(void*, u32, void*, void*)>(g_unlockWord)(game::FakeVm(), 0, nullptr, word);
                const char* wn = *reinterpret_cast<const char**>(static_cast<u8*>(word) + 0x28);
                if (wn) o += snprintf(runes + o, sizeof runes - o, "%s%s", o ? " " : "", wn);
            }
            if (had) { g_known = true; g_noBanner = true; g_sceneAt = now; g_step = 0; continue; }
            const char* nm = *reinterpret_cast<const char**>(static_cast<u8*>(sp) + 0x28);
            snprintf(g_bannerName, sizeof g_bannerName, "%s", nm ? nm : "ДРЕВНИЙ КРИК");
            snprintf(g_bannerRunes, sizeof g_bannerRunes, "%s", o ? runes : "FUS RO DAH");
            Log("свиток крика: «%s» %s", g_bannerName, g_bannerRunes);
            Bless(player);
            g_sceneAt = now;
            g_step = 0;
            continue;
        }
        if (reinterpret_cast<u8 (*)(void*, void*)>(g_hasSpell)(player, sp)) {
            g_known = true;                                  // the scene still plays; no new knowledge, no banner
            g_sceneAt = now;
            g_step = 0;
            continue;
        }
        reinterpret_cast<bool (*)(void*, void*)>(g_addSpell)(player, sp);
        SetBanner(sp);
        Bless(player);
        g_sceneAt = now;
        g_step = 0;
        Log("свиток прочитан: заклинание %08x изучено", g_scrolls[i].spell);
    }
}

}  // namespace coop::scrolls
