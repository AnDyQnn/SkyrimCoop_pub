#include "truehud.hpp"
#include "client.hpp"
#include "equip.hpp"
#include "game.hpp"
#include "lib.hpp"
#include "world.hpp"
#include <nn/os.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>

#pragma GCC diagnostic ignored "-Wformat-truncation"

/* TrueHUD's actor info bars and boss bars (ersh1/TrueHUD: Widgets/InfoBarBase.cpp, ActorInfoBar.cpp, BossInfoBar.cpp,
   HUDHandler.cpp, TrueHUDMenu.cpp), ported for 1.1.177 (05.10.2026). Its swf is used as it is: our HUDMenu.as loads
   Interface/TrueHUD.swf into the HUD and calls GameDelegate "RegisterHUDComponents" with ["SSW"] every frame; that
   call lands here, and the widgets get the same calls the PC plugin makes (loadConfig, setColors, updateData, the
   position and scale), through small helpers of our HUDMenu.as (sswTH*): TrueHUD names its widgets by number, which a
   GFx path cannot reach. Settings: TrueHUD's MCM defaults (MCM/Config/TrueHUD/settings.ini).
   What the PC plugin learns from events (combat start / end, the crosshair target) is polled here instead.
   1.1.177: GFxMovie vtable: SetVariable 0x11, GFxValue {obj, type, pad, value}.
   Invoke 0x17 (checked in the vtable; CommonLib order). Camera: PlayerCamera (*GOT 0x2146ca0) +0x20 root node, its children (+0x118) [0] = NiCamera: worldToCam 4x4 at
   +0x10C, frustum near/far +0x15C/+0x160, viewport (l, r, t, b) +0x170 (read off Eden 05.10). */
namespace coop::truehud {
namespace {

struct GFxValue { void* obj; u32 type; u32 pad; u64 value; };
constexpr u32 GFX_UNDEF = 0, GFX_BOOL = 2, GFX_NUMBER = 3, GFX_STRING = 4, GFX_MANAGED = 0x40;
constexpr u32 VT_INVOKE = 0x17;      // (05.10: the 1.1.177 GFxMovieRoot vtable 0x20c9768 — 0x17 forwards (name, result, args, n); 0x18 is the format-string one)
constexpr u32 GOT_REGISTER_HUD = 0x2158820, GOT_PLAYER_CAMERA = 0x2146ca0;

struct Fn { u32 off; u32 sig[4]; };
constexpr Fn GET_COMBAT_TARGET{0x9fe630, {0xD100C3FF, 0xA9017BFD, 0x910043FD, 0xF90013F3}};   // Actor.GetCombatTarget
constexpr Fn IS_HOSTILE{0x63b9ec, {0xA9BE7BFD, 0xA9014FF4, 0x910003FD, 0xAA0103F4}};          // Actor::IsHostileToActor
constexpr Fn IS_TEAMMATE{0xa01190, {0, 0, 0, 0}};                                               // Actor.IsPlayerTeammate (followers.cpp)
constexpr Fn GET_AV{0x9fe0f0, {0xD10783FF, 0xA91A7BFD, 0x910683FD, 0xF900DBFC}};                // Actor.GetActorValue
constexpr Fn GET_AV_PCT{0x9fe2f0, {0xD10783FF, 0xA91A7BFD, 0x910683FD, 0xF900DBFC}};            // Actor.GetActorValuePercentage
constexpr Fn GET_LEVEL{0x9ff89c, {0xA9BF7BFD, 0x910003FD, 0xAA0203E0, 0x97F024F3}};             // Actor.GetLevel
constexpr Fn GET_SCALE{0x1cf708, {0xFC1E0FE8, 0xA9017BFD, 0x910043FD, 0x7D419000}};             // TESObjectREFR::GetScale
constexpr Fn FS_CTOR{0xae6da8, {0xA9BE7BFD, 0xF9000BF3, 0x910003FD, 0xF900001F}};
constexpr u32 OFF_COMMANDED = 0x236;                                                            // Actor.IsCommandedActor: bit 0

/* TrueHUD settings.ini defaults */
constexpr int INFO_DIRECTION = 0, INFO_DISPLAY_NAME = 1, INFO_DAMAGE_COUNTER = 4, INFO_INDICATOR = 1, INFO_DMG_ALIGN = 2;
constexpr int RES_TARGET = 1, RES_HOSTILES = 0, RES_TEAMMATES = 1, RES_OTHERS = 0;
constexpr int SPEC_TARGET = 1, SPEC_HOSTILES = 0, SPEC_TEAMMATES = 0, SPEC_OTHERS = 0, INDICATOR_MODE = 1;   // (ours) level as text
constexpr float PHANTOM = 0.75f, DMG_DURATION = 2.f, OPACITY = 100.f, WIDTH = 60.f, RES_WIDTH = 30.f,
                TARGET_WIDTH = 100.f, TARGET_RES_WIDTH = 70.f, OFFSET_Z = 30.f, INFO_SCALE = 1.f;
constexpr int LEVEL_THRESHOLD = 10;
constexpr int BOSS_DMG_ALIGN = 2, BOSS_RES_MODE = 1, BOSS_SPEC_MODE = 1, BOSS_RES_ALIGN = 1,
              BOSS_NAME_ALIGN = 0, BOSS_INDICATOR_MODE = 1, BOSS_MAX = 3;
constexpr float BOSS_WIDTH = 400.f, BOSS_RES_WIDTH = 100.f, BOSS_ANCHOR_X = 0.5f, BOSS_ANCHOR_Y = 0.08f, BOSS_OFFSET = 50.f;
/* colors: health, magicka, stamina, special x (bar, phantom, background, penalty, flash) */
constexpr u32 COLORS[20] = {0xDF2020, 0xCBCBCB, 0x4D0F0F, 0x5F0000, 0xDF2020, 0x284BD7, 0xCBCBCB, 0x0E1A4D, 0x5F0000, 0x284BD7,
                            0x007E00, 0xCBCBCB, 0x004D00, 0x5F0000, 0x007E00, 0xFFA600, 0xCBCBCB, 0x805300, 0x5F0000, 0xFFA600};
constexpr u32 COLOR_DEFAULT = 0xCBCBCB, COLOR_DEFAULT_OUT = 0xCBCBCB, COLOR_WEAKER = 0x1E88E5, COLOR_WEAKER_OUT = 0x666666,
              COLOR_STRONGER = 0xCC2929, COLOR_STRONGER_OUT = 0x666666, COLOR_TEAMMATE = 0x1CB81C, COLOR_TEAMMATE_OUT = 0x666666;

enum TargetType : u8 { kTarget = 0, kEnemy = 1, kTeammate = 2, kOther = 3 };
enum TargetState : u8 { kAlive = 0, kStunned = 1, kBleedingOut = 2, kBleedingOutEssential = 3, kDead = 4 };
enum WidgetStateMode : u8 { kAdd = 0, kShow = 1, kHide = 2, kRemove = 3, kTargetKilled = 4 };
enum WidgetState : u8 { kActive = 0, kPendingHide = 1, kHidden = 2, kDelayedRemoval = 3, kPendingRemoval = 4, kRemoved = 5 };

/* TrueHUD BossRecognition (TrueHUD_base.ini): races and NPCs; plugin index 0 Skyrim, 1 Update, 2 Dawnguard, 4 Dragonborn
   are looked up by name at run time */
struct FileId { u8 file; u32 id; };   // file: 0 Skyrim.esm, 2 Dawnguard.esm, 4 Dragonborn.esm
constexpr FileId BOSS_RACES[] = {{0, 0x012E82}, {0, 0x0131EF}, {0, 0x0E7713}, {0, 0x0EBE18}, {0, 0x1052A3}, {2, 0x00283A},
                                 {2, 0x010D00}, {2, 0x0117DE}, {2, 0x015C34}, {4, 0x01F98F}, {4, 0x02C88B}, {4, 0x02C88C},
                                 {4, 0x03911A}, {4, 0x03CA97}};
constexpr FileId BOSS_NPCS[] = {{0, 0x013679}, {0, 0x019FE6}, {0, 0x01B07C}, {0, 0x01BA08}, {0, 0x01BB28}, {0, 0x01C4E5},
    {0, 0x01C902}, {0, 0x01D4B9}, {0, 0x01D4BA}, {0, 0x01E38B}, {0, 0x01E7D7}, {0, 0x02333A}, {0, 0x023AB0}, {0, 0x0240D7},
    {0, 0x026C52}, {0, 0x0284F2}, {0, 0x039BB7}, {0, 0x045F78}, {0, 0x046283}, {0, 0x048B55}, {0, 0x04B0AE}, {0, 0x04D246},
    {0, 0x05197F}, {0, 0x058303}, {0, 0x05B830}, {0, 0x064B1C}, {0, 0x06CD59}, {0, 0x07D679}, {0, 0x0834FE}, {0, 0x08BB91},
    {0, 0x090739}, {0, 0x09F360}, {0, 0x0A6842}, {0, 0x0A9548}, {0, 0x0AB6FF}, {0, 0x0BC09F}, {0, 0x0C0BE5}, {0, 0x0C1908},
    {0, 0x0CB11B}, {0, 0x0CC59B}, {0, 0x0D37F4}, {0, 0x0D823E}, {0, 0x0E1642}, {0, 0x0E1F81}, {0, 0x0E5F37}, {0, 0x0E76C9},
    {0, 0x0F6087}, {0, 0x0F6089}, {0, 0x0F608C}, {0, 0x1019C6}, {0, 0x10349B}, {2, 0x003788}, {2, 0x003BA7}, {2, 0x013823},
    {4, 0x019665}, {4, 0x01A373}, {4, 0x017936}, {4, 0x017938}, {4, 0x017F81}, {4, 0x01D77A}, {4, 0x01F998}, {4, 0x01FB98},
    {4, 0x026196}, {4, 0x0285C3}};
constexpr FileId BOSS_BLACKLIST[] = {{0, 0x013BA1}, {0, 0x032B94}};
/* ours: Ша'джара's contract bosses (SwitchPatch 0x1200 + k) and the mini-bosses (0x1270 + m), college/bounty.py */
constexpr u32 OUR_BOSS_FIRST = 0x1200, OUR_BOSS_N = 6, OUR_MINI_FIRST = 0x1270, OUR_MINI_N = 6;

struct Bar {
    u32 id;                 // the actor's FormID (TrueHUD keys by handle; the widget's name is this number)
    bool boss, used, configured, puppet;   // puppet: another player (ours: the name always shown)
    u8 state;               // WidgetState
    float depth;
    bool offscreen;
    int index;              // boss bars: their place in the stack
    float hpMax, mpMax, spMax;
};
constexpr int MAX_BARS = 24;
Bar g_bars[MAX_BARS];

uintptr_t g_origRegister, g_combatTarget, g_hostile, g_teammate, g_av, g_avPct, g_level, g_scale, g_fsCtor;
struct FS { const char* data; };
FS g_avNames[3];
bool g_ok, g_vanillaHidden;
int g_fileIdx[5] = {-2, -2, -2, -2, -2};
void* g_movie;
u64 g_lastScan;

template <class T> T At(const void* p, u32 off) { return *reinterpret_cast<const T*>(static_cast<const u8*>(p) + off); }
u32 Id(const void* f) { return At<u32>(f, 0x14); }

/* ---- GFx calls ---- */
GFxValue Num(double v) { GFxValue g{nullptr, GFX_NUMBER, 0, 0}; memcpy(&g.value, &v, 8); return g; }
GFxValue Bool(bool v) { GFxValue g{nullptr, GFX_BOOL, 0, (u64)(v ? 1 : 0)}; return g; }
GFxValue Str(const char* s) { GFxValue g{nullptr, GFX_STRING, 0, reinterpret_cast<u64>(s)}; return g; }
bool Invoke(const char* method, GFxValue* result, const GFxValue* args, u32 n) {
    if (!g_movie) return false;
    auto vt = *reinterpret_cast<uintptr_t**>(g_movie);
    return reinterpret_cast<bool (*)(void*, const char*, GFxValue*, const GFxValue*, u32)>(vt[VT_INVOKE])(g_movie, method, result, args, n);
}
/* a string result: unmanaged = the chars, managed (0x40) = a pointer to the chars' pointer */
[[maybe_unused]] const char* GfxStr(const GFxValue& r) {
    if (!r.value) return "";
    return (r.type & 0x40) ? *reinterpret_cast<const char* const*>(r.value) : reinterpret_cast<const char*>(r.value);
}
bool ResultBool(const GFxValue& r) { return (r.type & 0x0F) == GFX_BOOL ? (r.value & 0xFF) != 0 : false; }

/* sswTHCall(key, boss, method, args...) */
bool Call(const Bar& b, const char* method, const GFxValue* args, u32 n, GFxValue* result = nullptr) {
    GFxValue a[48];
    char key[16];
    snprintf(key, sizeof key, "%u", b.id);
    a[0] = Str(key); a[1] = Bool(b.boss); a[2] = Str(method); a[3] = Num(n);
    for (u32 i = 0; i < n && i < 44; i++) a[4 + i] = args[i];
    GFxValue r{};
    bool ok = Invoke("_root.HUDMovieBaseInstance.sswTHCall", result ? result : &r, a, 4 + n);
    return ok;
}

/* ---- game data ---- */
int FileIndex(int f) {
    static const char* names[5] = {"Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"};
    if (g_fileIdx[f] == -2) g_fileIdx[f] = game::PluginIndex(names[f]);
    return g_fileIdx[f];
}
bool Matches(u32 formId, const FileId* list, int n) {
    for (int i = 0; i < n; i++) {
        int idx = FileIndex(list[i].file);
        if (idx >= 0 && formId == (((u32)idx << 24) | list[i].id)) return true;
    }
    return false;
}

void* Player() { return game::Player(); }
float Av(void* a, int i) { return reinterpret_cast<float (*)(void*, u32, void*, FS*)>(g_av)(game::FakeVm(), 0, a, &g_avNames[i]); }
float AvPct(void* a, int i) { return reinterpret_cast<float (*)(void*, u32, void*, FS*)>(g_avPct)(game::FakeVm(), 0, a, &g_avNames[i]); }
u16 Level(void* a) { return (u16)reinterpret_cast<u32 (*)(void*, u32, void*)>(g_level)(game::FakeVm(), 0, a); }
void* CombatTarget(void* a) { return reinterpret_cast<void* (*)(void*, u32, void*)>(g_combatTarget)(game::FakeVm(), 0, a); }
bool Hostile(void* a, void* to) { return reinterpret_cast<bool (*)(void*, void*)>(g_hostile)(a, to); }
bool Dead(void* a) {
    auto vt = *reinterpret_cast<uintptr_t**>(a);
    return reinterpret_cast<bool (*)(void*, bool)>(vt[game::Get().vtIsDead / 8])(a, true);
}
bool IsPuppet(void* a) {
#ifndef SSW_OFFLINE
    return game::PuppetOwner(a) != 0;
#else
    (void)a; return false;
#endif
}
/* Utils::IsPlayerTeammateOrSummon */
bool Teammate(void* a) {
    if (IsPuppet(a)) return true;                                            // (ours) the other players: the party
    if (g_teammate && reinterpret_cast<bool (*)(void*, u32, void*)>(g_teammate)(game::FakeVm(), 0, a)) return true;
    return (At<u8>(a, OFF_COMMANDED) & 1) != 0;
}
bool IsBoss(void* a) {
    if (IsPuppet(a)) return false;
    auto base = At<u8*>(a, 0x40);
    u32 bid = base ? Id(base) : 0;
    if (Matches(bid, BOSS_BLACKLIST, sizeof BOSS_BLACKLIST / sizeof BOSS_BLACKLIST[0])) return false;
    if (Matches(bid, BOSS_NPCS, sizeof BOSS_NPCS / sizeof BOSS_NPCS[0])) return true;
    static int patch = -2;
    if (patch == -2) patch = game::PluginIndex("SwitchPatch.esp");
    if (patch >= 0 && (bid >> 24) == (u32)patch) {
        u32 lo = bid & 0xFFFFFF;
        if ((lo >= OUR_BOSS_FIRST && lo < OUR_BOSS_FIRST + OUR_BOSS_N) || (lo >= OUR_MINI_FIRST && lo < OUR_MINI_FIRST + OUR_MINI_N)) return true;
    }
    void* race = world::RaceOf(a);
    return race && Matches(Id(race), BOSS_RACES, sizeof BOSS_RACES / sizeof BOSS_RACES[0]);
}
const char* Name(void* a, char* buf, u32 size) {
#ifndef SSW_OFFLINE
    if (u16 id = game::PuppetOwner(a)) {                                    // (ours) a player: their name
        if (PlayerName(id, buf, size)) return buf;
    }
#endif
    auto base = At<u8*>(a, 0x40);
    const char* n = base && game::Readable(base + 0xD0, 8) ? At<const char*>(base, 0xD0) : nullptr;
    snprintf(buf, size, "%s", n && game::Readable(n, 1) ? n : "");
    return buf;
}
u16 LevelOf(void* a) {
#ifndef SSW_OFFLINE
    if (u16 id = game::PuppetOwner(a)) {
        proto::StatsMsg st;
        if (StatsOf(id, &st)) return st.level;
    }
#endif
    return Level(a);
}

/* ---- the camera: NiCamera::WorldPtToScreenPt3 ---- */
struct Cam { const float* m; float l, r, t, b, n, f; bool ok; };
Cam Camera() {
    Cam c{};
    void* pc = game::Singleton(game::ModuleAddr(GOT_PLAYER_CAMERA));
    if (!pc || !game::Readable(pc, 0x30)) return c;
    auto root = At<u8*>(pc, 0x20);
    if (!root || !game::Readable(root, 0x130)) return c;
    auto arr = At<u8**>(root, 0x118);
    u16 size = At<u16>(root, 0x122);
    if (!arr || !size || !game::Readable(arr, 8)) return c;
    u8* cam = arr[0];
    if (!cam || !game::Readable(cam, 0x180)) return c;
    c.m = reinterpret_cast<const float*>(cam + 0x10C);
    auto port = reinterpret_cast<const float*>(cam + 0x170);
    c.l = port[0]; c.r = port[1]; c.t = port[2]; c.b = port[3];
    c.n = At<float>(cam, 0x15C); c.f = At<float>(cam, 0x160);
    c.ok = c.r > c.l && c.n > 0.f && c.f > c.n;
    return c;
}
bool Project(const Cam& c, const float* p, float* x, float* y, float* z) {
    const float* m = c.m;
    float w = m[12] * p[0] + m[13] * p[1] + m[14] * p[2] + m[15];
    if (w <= 1e-5f) { *z = -1.f; return false; }
    float inv = 1.f / w;
    float sx = (m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3]) * inv;
    float sy = (m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7]) * inv;
    float sz = (m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]) * inv;
    *x = ((sx + 1.f) * 0.5f) * (c.r - c.l) + c.l;
    *y = ((sy + 1.f) * 0.5f) * (c.t - c.b) + c.b;
    *z = sz;
    return true;
}

/* ---- actors ---- */
void* g_actors[160];
int g_actorN;
void* Find(u32 id) {
    for (int i = 0; i < g_actorN; i++) if (g_actors[i] && Id(g_actors[i]) == id) return g_actors[i];
    return nullptr;
}
Bar* BarOf(u32 id) {
    for (auto& b : g_bars) if (b.used && b.id == id) return &b;
    return nullptr;
}
int BossCount() { int n = 0; for (auto& b : g_bars) if (b.used && b.boss) n++; return n; }

/* InfoBarBase::SetWidgetState */
void SetState(Bar& b, WidgetStateMode m) {
    switch (m) {
    case kAdd:
        if (b.state == kPendingHide || b.state == kPendingRemoval || b.state == kHidden) b.state = kActive;
        [[fallthrough]];
    case kShow:
        if (b.state == kPendingHide || b.state == kHidden) b.state = kActive;
        break;
    case kHide:
        if (b.state == kActive) b.state = kPendingHide;
        break;
    case kRemove:
        if (b.state < kPendingRemoval) b.state = kPendingRemoval;
        break;
    case kTargetKilled:
        if (b.state < kDelayedRemoval) b.state = kDelayedRemoval;
        break;
    }
}

char g_hudText[200]; volatile bool g_hudTextDue;
volatile int g_bossDir = 0; volatile bool g_dirReload; volatile int g_widthAgain;          // test aid "thdir <n>": the boss bars' fill direction (0 normal, 1 centered, 2 reverse)

void LoadConfig(Bar& b) {
    GFxValue a[44];
    int n = 0;
    if (!b.boss) {
        a[n++] = Num(INFO_DIRECTION); a[n++] = Num(b.puppet ? 4 : INFO_DISPLAY_NAME);   // (ours) players: "everyone"
        a[n++] = Bool(true); a[n++] = Bool(false);
        a[n++] = Num(INFO_DAMAGE_COUNTER); a[n++] = Num(b.puppet ? 4 : INFO_INDICATOR);   // (ours) players: level always
        a[n++] = Num(INFO_DMG_ALIGN);
        a[n++] = Num(RES_TARGET); a[n++] = Num(RES_HOSTILES); a[n++] = Num(RES_TEAMMATES); a[n++] = Num(RES_OTHERS);
        a[n++] = Num(SPEC_TARGET); a[n++] = Num(SPEC_HOSTILES); a[n++] = Num(SPEC_TEAMMATES); a[n++] = Num(SPEC_OTHERS);
        a[n++] = Bool(false); a[n++] = Num(INDICATOR_MODE); a[n++] = Num(PHANTOM); a[n++] = Num(DMG_DURATION);
        a[n++] = Num(OPACITY); a[n++] = Num(WIDTH); a[n++] = Num(RES_WIDTH); a[n++] = Num(TARGET_WIDTH); a[n++] = Num(TARGET_RES_WIDTH);
    } else {
        a[n++] = Num(g_bossDir); a[n++] = Bool(true); a[n++] = Bool(false); a[n++] = Bool(true); a[n++] = Bool(true);
        a[n++] = Num(BOSS_DMG_ALIGN); a[n++] = Num(BOSS_RES_MODE); a[n++] = Num(BOSS_SPEC_MODE); a[n++] = Bool(false);
        a[n++] = Num(BOSS_RES_ALIGN); a[n++] = Num(BOSS_NAME_ALIGN); a[n++] = Num(BOSS_INDICATOR_MODE);
        a[n++] = Num(PHANTOM); a[n++] = Num(DMG_DURATION); a[n++] = Num(OPACITY); a[n++] = Num(BOSS_WIDTH); a[n++] = Num(BOSS_RES_WIDTH);
    }
    for (int i = 0; i < 20; i++) a[n++] = Num(COLORS[i]);
    if (n != (b.boss ? 37 : 44)) Log("TrueHUD: loadConfig %d аргументов", n);
    Call(b, "loadConfig", a, n);
}

void RemoveWidget(Bar& b) {
    Call(b, "cleanUp", nullptr, 0);
    GFxValue a[2]; char key[16]; snprintf(key, sizeof key, "%u", b.id);
    a[0] = Str(key); a[1] = Bool(b.boss);
    Invoke("_root.HUDMovieBaseInstance.sswTHRemove", nullptr, a, 2);
    b = Bar{};
}

bool AddWidget(u32 id, bool boss) {
    if (BarOf(id)) return true;
    if (boss && BossCount() >= BOSS_MAX) boss = false;
    for (auto& b : g_bars) {
        if (b.used) continue;
        char key[16]; snprintf(key, sizeof key, "%u", id);
        GFxValue a[2]{Str(key), Bool(boss)}, r{};
        bool inv = Invoke("_root.HUDMovieBaseInstance.sswTHAdd", &r, a, 2);
        if (!inv || !ResultBool(r)) return false;
        void* actor = Find(id);
        b = Bar{id, boss, true, false, actor && IsPuppet(actor), kActive, 0.f, false, boss ? BossCount() : 0, 0.f, 0.f, 0.f};
        LoadConfig(b);                                                   // InfoBarBase::Initialize
        SetState(b, kShow);
        b.configured = true;
        return true;
    }
    return false;
}

void Position(Bar& b, void* a, const Cam& cam) {
    if (b.boss) {                                                        // BossInfoBar::UpdatePosition: a fixed anchor
        float y = BOSS_ANCHOR_Y + b.index * BOSS_OFFSET / 720.f;
        GFxValue args[5]; char key[16]; snprintf(key, sizeof key, "%u", b.id);
        args[0] = Str(key); args[1] = Bool(true); args[2] = Num(BOSS_ANCHOR_X); args[3] = Num(y); args[4] = Num(100.0 * INFO_SCALE);
        Invoke("_root.HUDMovieBaseInstance.sswTHPos", nullptr, args, 5);
        return;
    }
    /* ActorInfoBar::UpdatePosition: above the head (anchor: head = top of the actor, scaled) */
    const float* p = game::RefPos(a);
    float scale = g_scale ? reinterpret_cast<float (*)(void*)>(g_scale)(a) : 1.f;
    if (!(scale > 0.1f && scale < 10.f)) scale = 1.f;
    float pos[3] = {p[0], p[1], p[2] + 128.f * scale + OFFSET_Z};
    float x, y, z;
    bool front = Project(cam, pos, &x, &y, &z);
    b.depth = z;
    b.offscreen = !front || z < 0.f || x < -0.1f || x > 1.1f || y < -0.1f || y > 1.1f;
    if (b.offscreen) return;
    float lin = cam.n * cam.f / (cam.f + z * (cam.n - cam.f));
    float mult = fminf(fmaxf(lin, 500.f), 2000.f);
    float s = (((mult - 500.f) * (50.f - 100.f)) / (2000.f - 500.f)) + 100.f;
    GFxValue args[5]; char key[16]; snprintf(key, sizeof key, "%u", b.id);
    args[0] = Str(key); args[1] = Bool(false); args[2] = Num(x); args[3] = Num(1.f - y); args[4] = Num(s * INFO_SCALE);
    Invoke("_root.HUDMovieBaseInstance.sswTHPos", nullptr, args, 5);
}

/* InfoBarBase::UpdateInfo */
void Info(Bar& b, void* a, u32 targetId) {
    void* player = Player();
    u16 playerLevel = game::RealLevel(g_level, player);
    bool team = Teammate(a);
    u8 type = targetId == b.id ? kTarget : Hostile(a, player) ? kEnemy : team ? kTeammate : kOther;
    if (type == kTarget && b.state == kPendingRemoval) SetState(b, kAdd);
    u8 tstate = Dead(a) ? kDead : kAlive;
    bool inCombat = CombatTarget(a) != nullptr;
    bool puppet = IsPuppet(a);
    if (b.state < kDelayedRemoval) {
        GFxValue r{};
        Call(b, "isHidden", nullptr, 0, &r);
        if (ResultBool(r)) b.state = kHidden;
        bool shown;
        if (b.boss) shown = !team && inCombat && Hostile(a, player);
        else if (puppet) shown = true;                                   // (ours) the other players: always
        else if (type > kTarget && !inCombat) shown = false;
        else shown = true;                                               // hostiles / teammates / others: "in combat" modes
        if (tstate > kBleedingOut) SetState(b, kTargetKilled);
        else if (!shown) SetState(b, kRemove);
        else if (!b.boss) SetState(b, b.offscreen ? kHide : kShow);
    }
    char name[64];
    Name(a, name, sizeof name);
    u16 level = LevelOf(a);
    u32 levelColor = COLOR_DEFAULT, outline = COLOR_DEFAULT_OUT;
    if (team) { levelColor = COLOR_TEAMMATE; outline = COLOR_TEAMMATE_OUT; }
    else if ((int)playerLevel - (int)level > LEVEL_THRESHOLD) { levelColor = COLOR_WEAKER; outline = COLOR_WEAKER_OUT; }
    else if ((int)level - (int)playerLevel > LEVEL_THRESHOLD) { levelColor = COLOR_STRONGER; outline = COLOR_STRONGER_OUT; }
    float cur[3], pct[3], mx[3];
    float* keep[3] = {&b.hpMax, &b.mpMax, &b.spMax};
    for (int i = 0; i < 3; i++) {
        cur[i] = fmaxf(Av(a, i), 0.f);
        pct[i] = AvPct(a, i);
        if (pct[i] > 0.001f) *keep[i] = cur[i] / pct[i];
        mx[i] = fmaxf(*keep[i], cur[i]);
    }
#ifndef SSW_OFFLINE
    if (puppet) {                                       // (ours) a player's real numbers come in STATS, in percent
        proto::StatsMsg st;
        u8 p3[3] = {100, 100, 100};
        if (u16 id = game::PuppetOwner(a); id && StatsOf(id, &st)) { p3[0] = st.hp; p3[1] = st.mp; p3[2] = st.sp; }
        for (int i = 0; i < 3; i++) { mx[i] = 100.f; cur[i] = p3[i]; }
    }
#endif
    if (tstate == kDead) { cur[0] = 0.f; pct[0] = 0.f; }
    GFxValue args[18];
    args[0] = Num(type); args[1] = Num(b.state); args[2] = Num(b.depth); args[3] = Num(tstate); args[4] = Str(name);
    args[5] = Num(level); args[6] = Num(levelColor); args[7] = Num(outline); args[8] = Num(0);
    for (int i = 0; i < 3; i++) {
        args[9 + i * 3] = Num(ceilf(mx[i]));
        args[10 + i * 3] = Num(ceilf(cur[i]));
        args[11 + i * 3] = Num(mx[i] > 0.f ? fminf(cur[i] / mx[i], 1.f) : 0.f);
    }
    Call(b, "updateData", args, 18);
}

/* which actors get a bar: what the PC plugin hears from combat events and the crosshair, polled */
void Scan(const Cam& cam, u32* targetId) {
    void* player = Player();
    g_actorN = game::HighActors(g_actors, 160);
    const float* pp = game::RefPos(player);
    float best = 0.06f;
    *targetId = 0;
    for (int i = 0; i < g_actorN; i++) {
        void* a = g_actors[i];
        if (!a || a == player) continue;
        const float* p = game::RefPos(a);
        float dx = p[0] - pp[0], dy = p[1] - pp[1];
        float d2 = dx * dx + dy * dy;
        if (d2 > 4000.f * 4000.f || Dead(a)) continue;
        bool puppet = IsPuppet(a);
        void* ct = CombatTarget(a);
        /* HUDHandler::ProcessEvent (combat): relevant when the target is the player, or the actor is a teammate */
        bool relevant = puppet || (ct && (ct == player || Teammate(ct) || IsPuppet(ct) || Teammate(a)));
        if (relevant && !BarOf(Id(a))) {
            bool boss = !puppet && IsBoss(a) && Hostile(a, player);
            AddWidget(Id(a), boss);
        }
        /* the soft target: the actor nearest the crosshair, close enough (TrueHUD: the crosshair reference) */
        if (cam.ok && d2 < 2500.f * 2500.f) {
            float pos[3] = {p[0], p[1], p[2] + 90.f}, x, y, z;
            if (Project(cam, pos, &x, &y, &z) && z > 0.f) {
                float e = fabsf(x - 0.5f) + fabsf(y - 0.5f) * 0.6f;
                if (e < best) { best = e; *targetId = Id(a); }
            }
        }
    }
    if (*targetId && !BarOf(*targetId)) {
        void* t = Find(*targetId);
        if (t) AddWidget(*targetId, false);
    }
}

}  // namespace

void Install() {
    g_combatTarget = game::Checked177(GET_COMBAT_TARGET.off, GET_COMBAT_TARGET.sig);
    g_hostile = game::Checked177(IS_HOSTILE.off, IS_HOSTILE.sig);
    g_teammate = game::ModuleAddr(IS_TEAMMATE.off);
    g_av = game::Checked177(GET_AV.off, GET_AV.sig);
    g_avPct = game::Checked177(GET_AV_PCT.off, GET_AV_PCT.sig);
    g_level = game::Checked177(GET_LEVEL.off, GET_LEVEL.sig);
    g_scale = game::Checked177(GET_SCALE.off, GET_SCALE.sig);
    g_fsCtor = game::Checked177(FS_CTOR.off, FS_CTOR.sig);
    g_ok = g_combatTarget && g_hostile && g_av && g_avPct && g_level && g_fsCtor;
    auto slot = reinterpret_cast<uintptr_t*>(game::ModuleAddr(GOT_REGISTER_HUD));
    if (g_ok && slot && *slot) {
        g_origRegister = *slot;
        *slot = reinterpret_cast<uintptr_t>(&OnHudCall);
    } else {
        g_ok = false;
    }
    Log("TrueHUD: %s", g_ok ? "есть" : "нет функций");
}

void OnHudCall(void* argsObj) {
    auto base = static_cast<u8*>(argsObj);
    auto args = *reinterpret_cast<const GFxValue**>(base + 0x28);
    u32 n = *reinterpret_cast<u32*>(base + 0x30);
    bool ours = n > 0 && (args[0].type & 0x0F) == GFX_STRING;
    if (ours) {
        const char* s = (args[0].type & GFX_MANAGED) ? *reinterpret_cast<const char* const*>(args[0].value)
                                                     : reinterpret_cast<const char*>(args[0].value);
        ours = s && !strcmp(s, "SSW");
    }
    if (!ours) {
        if (g_origRegister) reinterpret_cast<void (*)(void*)>(g_origRegister)(argsObj);
        return;
    }
    if (!g_ok) return;
    g_movie = *reinterpret_cast<void**>(base + 0x20);
    static bool said;
    if (!said) { said = true; Log("TrueHUD: HUD вызывает модуль (movie %p)", g_movie); }
    if (g_hudTextDue) {
        g_hudTextDue = false;
        GFxValue a = Str(g_hudText);
        bool ok = Invoke("_root.HUDMovieBaseInstance.sswDown", nullptr, &a, 1);
        static int said;
        if (said++ < 3) Log("HUD: текст по центру %s", ok ? "показан" : "не вызвался");
    }
    if (g_dirReload) { g_dirReload = false; for (auto& b : g_bars) if (b.used && b.boss) LoadConfig(b); }
    if (g_widthAgain) {
        int w = g_widthAgain; g_widthAgain = 0;
        for (auto& b : g_bars) if (b.used && b.boss) {
            if (w > 0) { GFxValue a[2]{Num((float)w), Num(BOSS_RES_WIDTH)}; Call(b, "setWidth", a, 2); }
            char key[16]; snprintf(key, sizeof key, "%u", b.id);
            GFxValue a[2]{Str(key), Bool(true)}, r{};
            Invoke(w == -2 ? "_root.HUDMovieBaseInstance.sswTHDump" : "_root.HUDMovieBaseInstance.sswTHInspect", &r, a, 2);
            const char* t = nullptr;
            if ((r.type & 0xF) == GFX_STRING) t = (r.type & GFX_MANAGED) ? *reinterpret_cast<const char**>(r.value) : reinterpret_cast<const char*>(r.value);
            if (!t) t = "?";
            for (size_t o = 0, L = strlen(t); o < L; o += 150) {
                char part[160]; snprintf(part, sizeof part, "%.150s", t + o);
                Log("TrueHUD: полоска босса %s", part);
            }
        }
    }
    void* player = Player();
    if (!player || !game::Readable(At<void*>(player, 0x68), 8)) return;
    if (!g_avNames[0].data) {
        const char* names[3] = {"Health", "Magicka", "Stamina"};
        for (int i = 0; i < 3; i++) reinterpret_cast<void (*)(FS*, const char*)>(g_fsCtor)(&g_avNames[i], names[i]);
    }
    if (!g_vanillaHidden) {                                              // bHideVanillaTargetBar
        g_vanillaHidden = true;
        GFxValue a = Bool(true);
        Invoke("_root.HUDMovieBaseInstance.sswTHVanilla", nullptr, &a, 1);
    }
    Cam cam = Camera();
    static bool camSaid;
    if (!camSaid) { camSaid = true; Debug("TrueHUD: камера %s (near %.1f far %.0f port %.2f %.2f %.2f %.2f)", cam.ok ? "есть" : "нет", cam.n, cam.f, cam.l, cam.r, cam.t, cam.b); }
    if (!cam.ok) return;
    static u32 target;
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    if (now - g_lastScan > 150) { g_lastScan = now; Scan(cam, &target); }
    int bossIndex = 0;
    static bool compassHidden;
    bool anyBoss = BossCount() > 0;
    if (anyBoss != compassHidden) {                                      // uBossBarModifyHUD: the compass away under a boss bar
        compassHidden = anyBoss;
        GFxValue a = Bool(anyBoss);
        Invoke("_root.HUDMovieBaseInstance.sswTHCompass", nullptr, &a, 1);
    }
    for (auto& b : g_bars) {
        if (!b.used) continue;
        void* a = Find(b.id);
        if (!a) { RemoveWidget(b); continue; }                           // unloaded
        GFxValue r{};
        Call(b, "isReadyToRemove", nullptr, 0, &r);                       // ActorInfoBar::Update
        if ((r.type & 0x0F) == GFX_UNDEF) { b = Bar{}; continue; }        // the widget is gone (Flash side): added again by the scan
        if (ResultBool(r) || b.state == kRemoved) { RemoveWidget(b); continue; }
        if (b.boss) b.index = bossIndex++;
        Position(b, a, cam);
        Info(b, a, target);
    }
}

void HudText(const char* text) {
    if (!strcmp(text, g_hudText)) return;
    snprintf(g_hudText, sizeof g_hudText, "%s", text);
    g_hudTextDue = true;
}
void SetBossWidth(int w) { g_widthAgain = w; Log("TrueHUD: полоски боссов, ширина %d", w); }
void SetBossDir(int d) { g_bossDir = d; g_dirReload = true; Log("TrueHUD: полоски боссов, направление %d", d); }

}  // namespace coop::truehud
