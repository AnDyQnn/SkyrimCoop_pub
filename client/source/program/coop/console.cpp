#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#pragma GCC diagnostic ignored "-Wformat-security"
#include "console.hpp"
#include "client.hpp"
#include "game.hpp"
#include "equip.hpp"
#include "trade.hpp"
#include "glow.hpp"
#include <nn/os.hpp>
#include "followers.hpp"
#include "quests.hpp"
#include "avatar.hpp"
#include "magic.hpp"
#include "pvp.hpp"
#include "weather.hpp"
#include "mount.hpp"
#include "scrolls.hpp"
#include "truehud.hpp"
#include "npcsync.hpp"
#include "settings.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <strings.h>
#include "lib.hpp"

namespace coop::console {
namespace {

/* 1.1.177, from ConsoleMenu::ExecuteCommand (0x8cad48) and its task queue (tools/xrefs.py):
     Script* s = alloc(0x78); Script::Script(s) 0x2331c8; InitItem(s) 0xcba98;
     SetText(s, text) 0x233694; s->+0x50 = *global(GOT 0x21583a0);
     ConsoleTask t(0x18): ctor 0x967860(t, &unique_ptr<Script>, &targetRef, compiler);
     queue = 0x9678c0(); push 0x967948(queue, t) -> main loop 0x967b48 -> CompileAndRun(s, compiler, 1, target) */
struct Fn { u32 off; u32 sig[4]; };
constexpr Fn SCRIPT_CTOR{0x2331c8, {0xA9BE7BFD, 0xF9000BF3, 0x910003FD, 0xAA0003F3}};
constexpr Fn SCRIPT_INIT{0xcba98, {0xA9BE7BFD, 0xF9000BF3, 0x910003FD, 0x2A1F03E1}};
constexpr Fn SCRIPT_SET_TEXT{0x233694, {0xA9BD7BFD, 0xF9000BF5, 0x910003FD, 0xA9024FF4}};
constexpr Fn TASK_CTOR{0x967860, {0xF900001F, 0xF9400048, 0xF9000408, 0xF9400048}};
constexpr Fn QUEUE_GET{0x9678c0, {0xA9BE7BFD, 0xA9014FF4, 0x910003FD, 0xD0014808}};
constexpr Fn QUEUE_PUSH{0x967948, {0xA9BC7BFD, 0xF9000BF7, 0x910003FD, 0xA90257F6}};
constexpr u32 SCRIPT_GLOBAL_SLOT = 0x21583a0;

uintptr_t g_ctor, g_init, g_setText, g_taskCtor, g_queueGet, g_queuePush;
bool g_ok;

/* ScriptCompiler is an empty object on PC (CommonLibSSE creates one on the stack); the task keeps
   the pointer until the main loop runs it, so it lives here. */
alignas(16) u8 g_compiler[64];

uintptr_t Check(const Fn& f) { return game::Checked177(f.off, f.sig); }

/* Debug: watch the main loop run our commands (Script::CompileAndRun 0x2350d8, queue drain 0x967b48). */
constexpr Fn COMPILE_RUN{0x2350d8, {0xA9BB7BFD, 0xF9000BF9, 0x910003FD, 0xA9025FF8}};
constexpr Fn QUEUE_DRAIN{0x967b48, {0xA9BC7BFD, 0xA9015FF8, 0x910003FD, 0xA90257F6}};
volatile u32 g_drains;
HOOK_DEFINE_TRAMPOLINE(CompileRun) {
    static u64 Callback(void* script, void* compiler, u32 mode, void* target) {
        const char* text = *reinterpret_cast<const char**>(static_cast<u8*>(script) + 0x30);
        u64 r = Orig(script, compiler, mode, target);
        Debug("CompileAndRun mode %u '%s' -> %lx", mode, text ? text : "(null)", r);
        return r;
    }
};
/* Script compiler error reporter (0x237194(ctx, fmt, ...)): forwards the message to our log. */
constexpr Fn COMPILE_ERROR{0x237194, {0xA9BE7BFD,0xA9014FFC,0x910003FD,0xD10803FF}};
HOOK_DEFINE_TRAMPOLINE(CompileError) {
    static void Callback(void* ctx, const char* fmt, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f) {
        char msg[200];
        snprintf(msg, sizeof msg, fmt, a, b, c, d, e, f);
        Log("compiler: %s", msg);
        Orig(ctx, fmt, a, b, c, d, e, f);
    }
};
HOOK_DEFINE_TRAMPOLINE(Drain) {
    static void Callback(void* q) { __atomic_add_fetch(&g_drains, 1u, __ATOMIC_RELAXED); Orig(q); }
};

/* Console-only commands are stripped from the Switch build ("Script command "tgm" not found."),
   so the useful ones are done with the engine functions their Papyrus Debug twins call:
     Debug.SetGodMode       -> 0x7063a8(bool)           (getter 0x7063b8)
     Debug.ToggleAI         -> 0x72df00(*GOT 0x2144b80)
     Debug.ToggleCollisions -> 0x94020(*GOT 0x2144f30)
     Debug.ToggleMenus      -> byte +0x198 of *GOT 0x2145040
     Debug.CenterOnCell     -> 0x6fa428(player, const char* cell, nullptr) */
constexpr Fn GODMODE{0x7063a8, {0x12000008, 0xF0015A49, 0x39067128, 0xD65F03C0}};
constexpr Fn TOGGLE_AI{0x72df00, {0xA9BC7BFD, 0xA9015FF8, 0x910003FD, 0xA90257F6}};
constexpr Fn TOGGLE_COLL{0x94020, {0xA9BE7BFD, 0xF9000BF3, 0x910003FD, 0xB0010588}};
constexpr Fn CENTER_ON_CELL{0x6fa428, {0xD10143FF, 0xA9027BFD, 0x910083FD, 0xA90357F6}};
constexpr u32 AI_SLOT = 0x2144b80, COLL_SLOT = 0x2144f30, MENUS_SLOT = 0x2145040;
/* Actor.SetRace native (vm, stack, actor, race): the VM is not used */
constexpr Fn SET_RACE{0xa03b80, {0xA9BE7BFD, 0xF9000BF3, 0x910003FD, 0xF9411448}};
/* ObjectReference.MoveTo native (vm, stack, ref, target, x, y, z offsets in s0..s2, matchRotation): queues a
   move task holding the VM (our no-op one) */
constexpr Fn MOVE_TO{0xa51e00, {0xD10203FF, 0xFD0013EA, 0xF90027F9, 0x6D02A3E9}};
uintptr_t g_godMode, g_toggleAi, g_toggleColl, g_centerOnCell, g_setRace, g_moveTo;
volatile u16 g_tpTarget;
constexpr u32 ACTIVATE_SIG[4] = {0xD102C3FF, 0xA9057BFD, 0x910143FD, 0xA9066FFC};   // TESObjectREFR::ActivateRef
uintptr_t g_activate;

/* playable races, Skyrim.esm */
struct RaceName { const char* en; const char* ru; u32 id; };
constexpr RaceName RACES[] = {
    {"argonian", "аргонианин", 0x13740}, {"breton", "бретонец", 0x13741}, {"darkelf", "данмер", 0x13742},
    {"highelf", "альтмер", 0x13743}, {"imperial", "имперец", 0x13744}, {"khajiit", "каджит", 0x13745},
    {"nord", "норд", 0x13746}, {"orc", "орк", 0x13747}, {"redguard", "редгард", 0x13748}, {"woodelf", "босмер", 0x13749},
};

u32 ParseForm(const char* s) {                   // hex FormID or a race name
    while (*s == ' ') s++;
    for (auto& r : RACES)
        if (!strncasecmp(s, r.en, strlen(r.en)) || !strcmp(s, r.ru)) return r.id;
    return strtoul(s, nullptr, 16);
}

enum Builtin { B_NONE, B_TGM, B_TAI, B_TCL, B_TM, B_COC, B_SETRACE, B_EQUIP, B_UNEQUIPALL, B_TP, B_KILL, B_FACE, B_NEAR, B_USE, B_FIRE, B_GIVE, B_HEAL, B_MOUNT, B_REFINFO, B_READBOOK, B_ANIM, B_IDLE, B_PLACE, B_SHADER, B_CASTAT, B_SELLSIM, B_SUMMON, B_GOTO, B_SCENE, B_EXTRAS, B_GLOWTEST, B_AV, B_DROP, B_GLOB, B_FACE_TO, B_CAMDUMP, B_ADDITEM, B_RESETREF };
volatile int g_pending = B_NONE;
char g_pendingArg[96];

/* names compared without case, Cyrillic too (UTF-8 А-Я / Ё -> а-я / ё) */
u32 NextLower(const char*& p) {
    u8 c = (u8)*p++;
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    u32 cp = c;
    if ((c & 0xE0) == 0xC0 && *p) cp = ((c & 0x1F) << 6) | ((u8)*p++ & 0x3F);
    else if ((c & 0xF0) == 0xE0 && p[0] && p[1]) { cp = ((c & 0x0F) << 12) | (((u8)p[0] & 0x3F) << 6) | ((u8)p[1] & 0x3F); p += 2; }
    if (cp >= 0x410 && cp <= 0x42F) cp += 0x20;
    else if (cp == 0x401) cp = 0x451;
    return cp;
}
bool SameName(const char* a, const char* b) {
    while (*a && *b)
        if (NextLower(a) != NextLower(b)) return false;
    return !*a && !*b;
}

bool Is(const char* cmd, const char* name) {
    size_t n = strlen(name);
    return !strncasecmp(cmd, name, n) && (cmd[n] == 0 || cmd[n] == ' ');
}

}  // namespace

bool Resolve() {
    g_ctor = Check(SCRIPT_CTOR); g_init = Check(SCRIPT_INIT); g_setText = Check(SCRIPT_SET_TEXT);
    g_taskCtor = Check(TASK_CTOR); g_queueGet = Check(QUEUE_GET); g_queuePush = Check(QUEUE_PUSH);
    g_ok = g_ctor && g_init && g_setText && g_taskCtor && g_queueGet && g_queuePush;
    g_godMode = Check(GODMODE); g_toggleAi = Check(TOGGLE_AI); g_toggleColl = Check(TOGGLE_COLL);
    g_centerOnCell = Check(CENTER_ON_CELL);
    g_activate = game::Checked177(0x1deeb8, ACTIVATE_SIG);
    if (!g_activate && trade::Available()) g_activate = game::ModuleAddr(0x1deeb8);   // already hooked by trade.cpp
    g_setRace = Check(SET_RACE);
    g_moveTo = Check(MOVE_TO);
    if (uintptr_t a = Check(COMPILE_RUN)) CompileRun::InstallAtPtr(a);
    if (uintptr_t a = Check(QUEUE_DRAIN)) Drain::InstallAtPtr(a);
    if (uintptr_t a = Check(COMPILE_ERROR)) CompileError::InstallAtPtr(a);
    return g_ok;
}

bool Available() { return g_ok; }


/* test aid (01.10, armour glow): the player's 3D as the game holds it — pointers and the names they lead to, to find
   NiAVObject / NiNode fields on the Switch build */
bool HeapPtr(const void* p) {
    auto v = reinterpret_cast<uintptr_t>(p);
    return v >= 0x1000000000ull && v < 0x8000000000ull && !(v & 7) && game::Readable(p, 0x200);
}
const char* NameOf(const void* obj) {                  // NiObjectNET::name (a BSFixedString) at +0x10, if it reads
    if (!HeapPtr(obj)) return nullptr;
    auto s = *reinterpret_cast<const char* const*>(static_cast<const u8*>(obj) + 0x10);
    if (!HeapPtr(s)) return nullptr;
    for (int i = 0; i < 48; i++) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (!c) return i ? s : nullptr;
        if (c < 0x20 || c > 0x7e) return nullptr;
    }
    return nullptr;
}
void DumpObj(const char* tag, const void* obj, int words) {
    if (!HeapPtr(obj)) { Log("scene %s: %p (не указатель)", tag, obj); return; }
    auto w = reinterpret_cast<const u64*>(obj);
    Log("scene %s %p name=%s", tag, obj, NameOf(obj) ? NameOf(obj) : "-");
    for (int i = 0; i < words; i += 4)
        Log("scene %s +%03x: %016llx %016llx %016llx %016llx", tag, i * 8, (unsigned long long)w[i],
            (unsigned long long)w[i + 1], (unsigned long long)w[i + 2], (unsigned long long)w[i + 3]);
}
/* a node's children (NiNode: NiTObjectArray at +0x110: data ptr, then u16 capacity / size) */
void DumpKids(const void* node, int depth) {
    if (!HeapPtr(node) || depth > 6) return;
    auto kids = *reinterpret_cast<void* const* const*>(static_cast<const u8*>(node) + 0x118);
    u16 cap = *reinterpret_cast<const u16*>(static_cast<const u8*>(node) + 0x120);
    if (!HeapPtr(kids) || cap > 64) return;
    for (int i = 0; i < cap; i++) {
        const void* k = kids[i];
        if (!HeapPtr(k)) continue;
        u32 flags = *reinterpret_cast<const u32*>(static_cast<const u8*>(k) + 0xF4);
        Log("scene %*s%s  %p flags %08x", depth * 2, "", NameOf(k) ? NameOf(k) : "?", k, flags);
        DumpKids(k, depth + 1);
    }
}

void* g_readRef;                          // readbook's book: deleted once the page has had time to show
u64 g_readRefAt;

void PumpBuiltins() {
    if (g_readRef && (u64)nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds() - g_readRefAt > 8000) { game::DeleteRef(g_readRef); g_readRef = nullptr; }
    int b = __atomic_exchange_n(&g_pending, (int)B_NONE, __ATOMIC_ACQ_REL);
    char msg[128] = {};
    switch (b) {
        case B_TGM: {
            bool on = !reinterpret_cast<bool (*)()>(g_godMode + 0x10)();          // getter right after the setter
            reinterpret_cast<void (*)(bool)>(g_godMode)(on);
            snprintf(msg, sizeof msg, "Режим бога: %s", on ? "вкл" : "выкл");
            break;
        }
        case B_TAI:
            reinterpret_cast<void (*)(void*)>(g_toggleAi)(game::Singleton(game::ModuleAddr(AI_SLOT)));
            snprintf(msg, sizeof msg, "ИИ переключён");
            break;
        case B_TCL:
            reinterpret_cast<void (*)(void*)>(g_toggleColl)(game::Singleton(game::ModuleAddr(COLL_SLOT)));
            snprintf(msg, sizeof msg, "Столкновения переключены");
            break;
        case B_TM: {
            auto ui = static_cast<u8*>(game::Singleton(game::ModuleAddr(MENUS_SLOT)));
            if (ui) ui[0x198] ^= 1;
            break;
        }
        case B_COC:
            if (void* player = game::Player())
                reinterpret_cast<void (*)(void*, const char*, void*)>(g_centerOnCell)(player, g_pendingArg, nullptr);
            break;
        case B_SETRACE: {
            void* race = equip::Lookup(ParseForm(g_pendingArg));
            if (void* player = game::Player(); race && g_setRace) {
                reinterpret_cast<void (*)(void*, u32, void*, void*)>(g_setRace)(nullptr, 0, player, race);
                snprintf(msg, sizeof msg, "Раса изменена");
            } else snprintf(msg, sizeof msg, "Нет такой расы: %s", g_pendingArg);
            break;
        }
        case B_EQUIP: {
            char* p = g_pendingArg;                    // several FormIDs separated by spaces
            int n = 0;
            while (*p) {
                char* end;
                u32 id = strtoul(p, &end, 16);
                if (end == p) break;
                void* f = equip::Lookup(id);
                if (f && static_cast<u8*>(f)[0x1A] == 0x77) {      // a shout: Actor.EquipShout (vm, stack, actor, shout)
                    static uintptr_t eqShout;
                    if (!eqShout) eqShout = game::ModuleAddr(0x9fdc20);
                    reinterpret_cast<void (*)(void*, u32, void*, void*)>(eqShout)(game::FakeVm(), 0, game::Player(), f);
                    n++;
                } else if (equip::EquipOn(game::Player(), f)) n++;
                p = end;
            }
            snprintf(msg, sizeof msg, "Надето предметов: %d", n);
            break;
        }
        case B_TP: {
            void* target = game::PuppetRef(g_tpTarget);
            void* player = game::Player();
            if (target && player && g_moveTo) {
                reinterpret_cast<bool (*)(void*, u32, void*, void*, float, float, float, bool)>(g_moveTo)(
                    game::FakeVm(), 0, player, target, 80.0f, 0.0f, 0.0f, true);
                char name[32];
                if (!PlayerName(g_tpTarget, name, sizeof name)) snprintf(name, sizeof name, "игроку %u", g_tpTarget);
                snprintf(msg, sizeof msg, "Кооп: перемещение к %s", name);
            } else {                                                   // not loaded here: to their cell / world
                RemotePlayer rs[MAX_REMOTE];
                int n = SnapshotRemotes(rs, MAX_REMOTE);
                snprintf(msg, sizeof msg, "Кооп: игрок ещё загружается — попробуй через пару секунд");
                for (int i = 0; i < n; i++) {
                    const auto& st = rs[i].state;
                    if (st.id != g_tpTarget || !st.cell) continue;
                    float rot = st.rotZ;                           // a little behind them, not inside
                    game::TeleportTo(st.cell, st.worldspace, st.x - 100.0f * sinf(rot), st.y - 100.0f * cosf(rot), st.z + 10.0f,
                                     "Кооп: ты рядом с другом");
                    char name[32];
                    if (!PlayerName(g_tpTarget, name, sizeof name)) snprintf(name, sizeof name, "игроку %u", g_tpTarget);
                    snprintf(msg, sizeof msg, "Кооп: перемещение к %s", name);
                    Log("консоль: перемещение к %s (ячейка %08x)", name, st.cell);
                }
            }
            break;
        }
        case B_FACE:
            game::FaceTowards(game::PuppetRef(g_tpTarget));
            break;
        case B_GIVE: {
            char* end;
            u32 id = strtoul(g_pendingArg, &end, 16);
            long n = strtol(end, nullptr, 10);
            if (n == 0) n = 1;
            void* form = equip::Lookup(id);
            u8 type = form ? *(static_cast<u8*>(form) + 0x1A) : 0;
            if (!form || type == 0x3D || type == 0x3E || type == 0x2B) {       // references, actors, NPCs: no
                snprintf(msg, sizeof msg, "Кооп: нет такого предмета %08x", id);
                Log("консоль: нет предмета %08x", id);
                break;
            }
            trade::AddItems(game::Player(), form, (s32)n);
            snprintf(msg, sizeof msg, n > 0 ? "Кооп: выдано %ld шт. (%08x)" : "Кооп: забрано %ld шт. (%08x)", n > 0 ? n : -n, id);
            Log("консоль: %s %ld x %08x", n > 0 ? "выдано" : "забрано", n > 0 ? n : -n, id);
            break;
        }
        case B_HEAL:
            pvp::HealPlayer();
            snprintf(msg, sizeof msg, "Кооп: здоровье восстановлено");
            Log("консоль: здоровье восстановлено");
            break;
        case B_FIRE: {
            char* end;
            u32 bow = strtoul(g_pendingArg, &end, 16);
            u32 arrow = strtoul(end, &end, 16);
            magic::TestFire(game::Player(), bow, arrow, strstr(end, "copy") != nullptr);
            break;
        }
        case B_NEAR: {
            /* references within 1500 units whose base is furniture (form type 0x28): workbenches, grindstones */
            void* player = game::Player();
            u32 cell, world; float pos[3];
            if (!player || !game::WhereAmI(&cell, &world, pos)) break;
            int n = 0;
            u32 want = strtoul(g_pendingArg, nullptr, 16);
            for (u32 id = 1; id < 0x110000 && n < 25; id++) {
                auto ref = static_cast<u8*>(equip::Lookup(id));
                if (!ref || (ref[0x1A] != 0x3D)) continue;
                auto base = *reinterpret_cast<u8**>(ref + 0x40);
                if (!base || base[0x1A] != want) continue;
                const float* rp = game::RefPos(ref);
                float dx = rp[0] - pos[0], dy = rp[1] - pos[1], dz = rp[2] - pos[2];
                if (dx * dx + dy * dy + dz * dz > 1500.0f * 1500.0f) continue;
                Log("near: ref %08x base %08x at %.0f %.0f %.0f", id, *reinterpret_cast<u32*>(base + 0x14), rp[0], rp[1], rp[2]);
                n++;
            }
            snprintf(msg, sizeof msg, "Рядом мебели: %d (список в журнале)", n);
            break;
        }
        case B_ANIM:                                                       // test aid: anim <graph event> on the player
            Log("консоль: anim %s -> %d", g_pendingArg, game::SendAnimEvent(game::Player(), g_pendingArg));
            break;
        case B_IDLE: {                                                     // test aid: idle <IDLE FormID> (Actor.PlayIdle)
            static const u32 sig[4] = {0xA9BF7BFD, 0x910003FD, 0x2A0103E9, 0xAA0003E8};
            uintptr_t f = game::Checked177(0xa01d40, sig);
            void* idle = equip::Lookup(strtoul(g_pendingArg, nullptr, 16));
            bool r = f && idle && reinterpret_cast<bool (*)(void*, u32, void*, void*)>(f)(game::FakeVm(), 0, game::Player(), idle);
            Log("консоль: idle %s (%p) -> %d", g_pendingArg, idle, r);
            break;
        }
        case B_SUMMON:
            if (void* ref = equip::Lookup(strtoul(g_pendingArg, nullptr, 16)); ref && g_moveTo)
                reinterpret_cast<void (*)(void*, u32, void*, void*, float, float, float, bool)>(g_moveTo)(
                    game::FakeVm(), 0, ref, game::Player(), 120.0f, 0.0f, 0.0f, true);
            break;
        case B_SCENE: {                                                    // test aid: scene [offset hex] [words]
            auto player = static_cast<u8*>(game::Player());
            if (!player) break;
            void* loaded = *reinterpret_cast<void**>(player + 0x68);
            Log("scene: player %p loaded %p name %s", player, loaded, NameOf(loaded) ? NameOf(loaded) : "-");
            DumpObj("loaded", loaded, 8);
            unsigned off = 0, words = 48;
            sscanf(g_pendingArg, "%x %u", &off, &words);
            /* TESObjectREFR::Get3D: vtable +0x378 (the Is3DLoaded native 0xa514d0 calls it); off != 0: a field of loaded */
            void* root = off ? (HeapPtr(loaded) ? *reinterpret_cast<void**>(static_cast<u8*>(loaded) + off) : nullptr)
                             : reinterpret_cast<void* (*)(void*)>((*reinterpret_cast<uintptr_t**>(player))[0x378 / 8])(player);
            Log("scene: root (loaded+%x) %p name %s", off, root, NameOf(root) ? NameOf(root) : "-");
            DumpObj("root", root, words);
            DumpKids(root, 0);
            break;
        }
        case B_EXTRAS: {                                                   // test aid: the worn items' extra data (log)
            auto player = static_cast<u8*>(game::Player());
            uintptr_t gc = game::ModuleAddr(0x3521c);
            void* changes = player ? reinterpret_cast<void* (*)(void*)>(gc)(player + 0x70) : nullptr;
            if (!changes) break;
            int shown = 0;
            for (auto node = *reinterpret_cast<u8**>(changes); node && shown < 12; node = *reinterpret_cast<u8**>(node + 8)) {
                u8* entry = *reinterpret_cast<u8**>(node);
                if (!entry) continue;
                u8* object = *reinterpret_cast<u8**>(entry);
                for (auto x = *reinterpret_cast<u8**>(entry + 8); x; x = *reinterpret_cast<u8**>(x + 8)) {
                    u8* xl = *reinterpret_cast<u8**>(x);
                    if (!HeapPtr(xl)) continue;
                    u8* bits = *reinterpret_cast<u8**>(xl + 0x10);
                    if (!HeapPtr(bits) || !((bits[2] & 0x40) || (bits[0x13] & 0x08))) continue;   // worn or enchanted
                    shown++;
                    auto w = reinterpret_cast<u64*>(xl);
                    Log("extras %08x xl %p: %016llx %016llx %016llx %016llx", *reinterpret_cast<u32*>(object + 0x14), xl,
                        (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3]);
                    Log("extras   bits %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                        bits[0], bits[1], bits[2], bits[3], bits[4], bits[5], bits[6], bits[7], bits[8], bits[9], bits[10],
                        bits[11], bits[12], bits[13], bits[14], bits[15], bits[16], bits[17], bits[18], bits[19]);
                    for (int k = 0; k < 2; k++) {                           // the chain might start at +0 or +8
                        u8* d = reinterpret_cast<u8*>(w[k]);
                        for (int n = 0; HeapPtr(d) && n < 10; n++) {
                            auto v = reinterpret_cast<u64*>(d);
                            Log("extras   [%d] %p: %016llx %016llx %016llx %016llx", k, d, (unsigned long long)v[0],
                                (unsigned long long)v[1], (unsigned long long)v[2], (unsigned long long)v[3]);
                            d = reinterpret_cast<u8*>(v[1]);
                        }
                    }
                }
            }
            break;
        }
        case B_GLOWTEST: {                                                 // test aid: glowtest <layer FormID> <colour 0..6>
            unsigned id = 0; int c = 0;
            sscanf(g_pendingArg, "%x %d", &id, &c);
            glow::TestPaint(id, c);
            break;
        }
        case B_GOTO:                                                       // test aid: goto <ref FormID> (the player to it)
            if (void* ref = equip::Lookup(strtoul(g_pendingArg, nullptr, 16)); ref && g_moveTo)
                reinterpret_cast<void (*)(void*, u32, void*, void*, float, float, float, bool)>(g_moveTo)(
                    game::FakeVm(), 0, game::Player(), ref, 0.0f, 0.0f, 0.0f, true);
            break;
        case B_SELLSIM:
            if (void* it = equip::Lookup(strtoul(g_pendingArg, nullptr, 16))) {
                trade::AddItems(game::Player(), it, -1);
                trade::AddItems(game::Player(), equip::Lookup(0xF), 500);
            }
            break;
        case B_CASTAT: {
            static const u32 sig[4] = {0xD10183FF, 0xA9017BFD, 0x910043FD, 0xF90013F9};
            uintptr_t f = game::Checked177(0xa35ec0, sig);
            char* end;
            void* sp = equip::Lookup(strtoul(g_pendingArg, &end, 16));
            void* ref = equip::Lookup(strtoul(end, nullptr, 16));
            if (f && sp && ref) reinterpret_cast<void (*)(void*, u32, void*, void*, void*)>(f)(game::FakeVm(), 0, sp, ref, ref);
            Log("консоль: castat %s (%p %p)", g_pendingArg, sp, ref);
            break;
        }
        case B_SHADER: {                                                   // test aid: shader <EFSH> <ref> <secs>
            static const u32 sig[4] = {0xFC1B0FE8, 0xA9017BFD, 0x910043FD, 0xA9025FF8};
            uintptr_t f = game::Checked177(0xa379f0, sig);
            char* end;
            void* fx = equip::Lookup(strtoul(g_pendingArg, &end, 16));
            void* ref = equip::Lookup(strtoul(end, &end, 16));
            float secs = strtof(end, nullptr);
            if (f && fx && ref) reinterpret_cast<void (*)(void*, u32, void*, void*, float)>(f)(game::FakeVm(), 0, fx, ref, secs ? secs : 30.0f);
            Log("консоль: shader %s", g_pendingArg);
            break;
        }
        case B_PLACE: {                                                    // test aid: place <FormID>: in front of you
            void* base = equip::Lookup(strtoul(g_pendingArg, nullptr, 16));
            void* player = game::Player();
            if (!base || !player) break;
            const float* p = game::RefPos(player);
            float rot = *reinterpret_cast<const float*>(static_cast<const u8*>(player) + game::Get().offRotZ);
            void* ref = game::PlaceObject(base, p[0] + 220.0f * sinf(rot), p[1] + 220.0f * cosf(rot), p[2] + 40.0f);
            Log("консоль: place %s -> %p (FormID %08x)", g_pendingArg, ref, ref ? *reinterpret_cast<u32*>(static_cast<u8*>(ref) + 0x14) : 0u);
            break;
        }
        case B_READBOOK: {                                                 // test aid: readbook <book FormID>: read it
            void* base = equip::Lookup(strtoul(g_pendingArg, nullptr, 16));   // as if lying in front of you
            void* player = game::Player();
            if (!base || !player || !g_activate) break;
            const float* p = game::RefPos(player);
            void* ref = game::PlaceObject(base, p[0], p[1], p[2] - 400.0f);        // out of sight, read, gone
            if (ref) reinterpret_cast<bool (*)(void*, void*, u8, void*, s32, bool)>(g_activate)(ref, player, 0, nullptr, 1, false);
            if (g_readRef) game::DeleteRef(g_readRef);
            g_readRef = ref;
            g_readRefAt = (u64)nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
            Log("консоль: readbook %s -> %p", g_pendingArg, ref);
            break;
        }
        case B_RESETREF: {                                                 // test aid: resetref <ref> (ObjectReference.Reset)
            void* ref = equip::Lookup(strtoul(g_pendingArg, nullptr, 16));
            if (ref) reinterpret_cast<void (*)(void*, u32, void*, void*)>(game::ModuleAddr(0xa54db0))(game::FakeVm(), 0, ref, nullptr);
            Log("resetref %s: %s", g_pendingArg, ref ? "ok" : "нет");
            break;
        }
        case B_ADDITEM: {                                                  // test aid: additem <ref> <form> <n> (n < 0: remove)
            unsigned r = 0, f = 0; int n = 0;
            sscanf(g_pendingArg, "%x %x %d", &r, &f, &n);
            void* ref = equip::Lookup(r);
            void* form = equip::Lookup(f);
            if (ref && form && n) trade::AddItems(ref, form, n, true);
            Log("additem %08x %08x %d: %s", r, f, n, ref && form ? "ok" : "нет");
            break;
        }
        case B_REFINFO: {                                                  // test aid: refinfo <FormID>
            u32 id = strtoul(g_pendingArg, nullptr, 16);
            auto ref = static_cast<u8*>(equip::Lookup(id));
            if (!ref) { Log("refinfo %08x: нет такого", id); break; }
            u8 type = ref[0x1A];
            if (type == 0x4D) { Log("refinfo %08x: квест, флаги %02x (бит 0 = идёт)", id, ref[0xC4]); break; }
            if (type != 0x3D && type != 0x3E) {                         // raw words of any other form
                auto w = reinterpret_cast<u32*>(ref);
                for (int i = 0; i < 32; i += 8)
                    Log("refinfo %08x +%02x: %08x %08x %08x %08x %08x %08x %08x %08x", id, i * 4, w[i], w[i + 1],
                        w[i + 2], w[i + 3], w[i + 4], w[i + 5], w[i + 6], w[i + 7]);
            }
            void* base = (type == 0x3D || type == 0x3E) ? *reinterpret_cast<void**>(ref + 0x40) : nullptr;
            const float* p = (type == 0x3D || type == 0x3E) ? game::RefPos(ref) : nullptr;
            trade::Count c[64];
            int n = (type == 0x3D || type == 0x3E) ? trade::Changes(ref, c, 64) : 0;
            Log("refinfo %08x: тип %02x, база %08x (тип %02x), позиция %.0f %.0f %.0f, изменений инвентаря %d",
                id, type, base ? *reinterpret_cast<u32*>(static_cast<u8*>(base) + 0x14) : 0,
                base ? static_cast<u8*>(base)[0x1A] : 0, p ? p[0] : 0, p ? p[1] : 0, p ? p[2] : 0, n);
            for (int i = 0; i < n && i < 12; i++) Log("   %08x x%d", c[i].id, c[i].n);
            break;
        }
        case B_MOUNT:
            mount::Command(g_pendingArg);
            break;
        case B_USE: {
            auto ref = equip::Lookup(strtoul(g_pendingArg, nullptr, 16));
            if (ref && g_activate)
                reinterpret_cast<bool (*)(void*, void*, u8, void*, s32, bool)>(g_activate)(ref, game::Player(), 0, nullptr, 1, false);
            break;
        }
        case B_DROP: {                                                     // test aid: drop <form> — as the inventory's Drop
            void* item = equip::Lookup(strtoul(g_pendingArg, nullptr, 16));
            void* player = game::Player();
            if (!item || !player) break;
            auto vt = *reinterpret_cast<uintptr_t**>(player);
            struct Handle { u32 v = 0; Handle() {} Handle(const Handle& o) : v(o.v) {} ~Handle() {} };   // returned via x8
            u32 h = reinterpret_cast<Handle (*)(void*, void*, s32, u32, void*, void*, const float*, const float*)>(vt[0x2b0 / 8])(
                player, item, 1, 3, nullptr, nullptr, nullptr, nullptr).v;
            Log("консоль: drop %s -> handle %x (slot +%lx)", g_pendingArg, h, (unsigned long)(vt[0x2b0 / 8] - game::ModuleAddr(0)));
            break;
        }
        case B_FACE_TO: {                                                  // test aid: heading <degrees> | faceref <hex FormID>
            void* player = game::Player();
            if (!player) break;
            float deg;
            if (g_pendingArg[0] == 'r') {
                void* ref = equip::Lookup(strtoul(g_pendingArg + 1, nullptr, 16));
                if (!ref) break;
                const float* p = game::RefPos(player), *q = game::RefPos(ref);
                deg = atan2f(q[0] - p[0], q[1] - p[1]) * 57.29578f;
            } else {
                deg = strtof(g_pendingArg, nullptr);
            }
            reinterpret_cast<float*>(static_cast<u8*>(player) + game::Get().offRotZ)[0] = deg / 57.29578f;
            break;
        }
        case B_CAMDUMP: {                                                  // test aid (05.10, TrueHUD port): camera layout
            void* cam = game::Singleton(game::ModuleAddr(0x2146ca0));
            Log("camdump: PlayerCamera %p", cam);
            if (!cam || !game::Readable(cam, 0x40)) break;
            for (int r = 0x18; r <= 0x30; r += 8) {                          // TESCamera::cameraRoot candidates
                auto node = *reinterpret_cast<u8**>(static_cast<u8*>(cam) + r);
                if (!node || !game::Readable(node, 0x140)) continue;
                Log("camdump: root? cam+%x = %p vt %lx", r, node, (unsigned long)(*reinterpret_cast<u64*>(node) - game::ModuleAddr(0)));
                for (int co = 0x100; co <= 0x130; co += 8) {                  // NiNode children {ptr, cap, size}
                    auto arr = *reinterpret_cast<u8***>(node + co);
                    u16 sz = *reinterpret_cast<u16*>(node + co + 10);
                    if (!arr || !game::Readable(arr, 8) || sz == 0 || sz > 8) continue;
                    for (int k = 0; k < sz; k++) {
                        u8* c = arr[k];
                        if (!c || !game::Readable(c, 0x1c0)) continue;
                        Log("camdump:  children@%x [%d] %p vt %lx", co, k, c, (unsigned long)(*reinterpret_cast<u64*>(c) - game::ModuleAddr(0)));
                        auto f = reinterpret_cast<float*>(c);
                        for (int o = 0x100; o < 0x1c0; o += 0x10)
                            Log("camdump:    +%03x %.3f %.3f %.3f %.3f", o, f[o / 4], f[o / 4 + 1], f[o / 4 + 2], f[o / 4 + 3]);
                    }
                }
            }
            break;
        }
        case B_GLOB: {                                                     // test aid: glob <hex FormID> [value]
            char* end;
            auto g = static_cast<u8*>(equip::Lookup(strtoul(g_pendingArg, &end, 16)));
            if (!g || g[0x1A] != 0x09) { Log("консоль: glob %s — не глобальная", g_pendingArg); break; }
            float* v = reinterpret_cast<float*>(g + 0x34);
            if (*end) *v = strtof(end, nullptr);
            Log("консоль: glob %s = %.0f", g_pendingArg, *v);
            break;
        }
        case B_AV: {                                                       // test aid: av <name>: the player's actor value
            struct FS { const char* data; } fs{};
            reinterpret_cast<void (*)(FS*, const char*)>(game::ModuleAddr(0xae6da8))(&fs, g_pendingArg);
            static const u32 sig[4] = {0xD10783FF, 0xA91A7BFD, 0x910683FD, 0xF900DBFC};
            uintptr_t get = game::Checked177(0x9fe0f0, sig);                 // Actor.GetActorValue
            if (!get || !game::Player()) break;
            float v = reinterpret_cast<float (*)(void*, u32, void*, FS*)>(get)(game::FakeVm(), 0, game::Player(), &fs);
            Log("консоль: av %s = %.2f", g_pendingArg, v);
            break;
        }
        case B_KILL: {
            u32 id = strtoul(g_pendingArg, nullptr, 16);
            snprintf(msg, sizeof msg, game::KillActor(id) ? "Убит: %08x" : "Нет такого живого персонажа рядом: %08x", id);
            break;
        }
        case B_UNEQUIPALL:
            equip::UnequipAllOn(game::Player());
            break;
        default:
            return;
    }
    if (msg[0]) game::Notify(msg);
}

bool Execute(const char* command) {
    if (!command || !command[0]) return false;
    while (*command == ' ') command++;
    int builtin = B_NONE;
    if (Is(command, "tgm") && g_godMode) builtin = B_TGM;
    else if (Is(command, "tai") && g_toggleAi) builtin = B_TAI;
    else if (Is(command, "tcl") && g_toggleColl) builtin = B_TCL;
    else if (Is(command, "tm") && g_godMode) builtin = B_TM;
    else if ((Is(command, "coc") || Is(command, "centeroncell")) && g_centerOnCell) {
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        snprintf(g_pendingArg, sizeof g_pendingArg, "%s", arg + 1);
        builtin = B_COC;
    } else if ((Is(command, "setrace") || Is(command, "player.setrace")) && g_setRace) {
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        snprintf(g_pendingArg, sizeof g_pendingArg, "%s", arg + 1);
        builtin = B_SETRACE;
    } else if (Is(command, "equip") && equip::Available()) {           // equip <id> [<id> ...]: adds if missing
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        snprintf(g_pendingArg, sizeof g_pendingArg, "%s", arg + 1);
        builtin = B_EQUIP;
    } else if (Is(command, "give") || Is(command, "gold")) {           // give <hex FormID> [n] | gold <n>
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        if (Is(command, "gold")) snprintf(g_pendingArg, sizeof g_pendingArg, "f %s", arg + 1);
        else snprintf(g_pendingArg, sizeof g_pendingArg, "%s", arg + 1);
        builtin = B_GIVE;
    } else if (Is(command, "weather")) {                               // weather <hex FormID>: this sky here (the host's goes to all)
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        weather::Received(strtoul(arg + 1, nullptr, 16));
        Log("консоль: погода %s", arg + 1);
        return true;
    } else if (Is(command, "heal")) {                                  // health back to full
        builtin = B_HEAL;
    } else if (Is(command, "fire")) {                                  // test aid: fire <bow> <arrow> [copy]: the player shoots
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        snprintf(g_pendingArg, sizeof g_pendingArg, "%s", arg + 1);
        builtin = B_FIRE;
    } else if (Is(command, "chest")) {                                 // the clan's shared chest
        SocialSend("chest get", false);
        return true;
    } else if (Is(command, "horse") || Is(command, "ride") || Is(command, "dismount") || Is(command, "mounted")) {
        snprintf(g_pendingArg, sizeof g_pendingArg, "%s", command);   // test aids for riding (mount.cpp)
        builtin = B_MOUNT;
    } else if (Is(command, "anim") || Is(command, "idle")) {
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        snprintf(g_pendingArg, sizeof g_pendingArg, "%s", arg + 1);
        builtin = Is(command, "anim") ? B_ANIM : B_IDLE;
    } else if (Is(command, "summon")) {                                // test aid: summon <ref>: it comes to you
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        snprintf(g_pendingArg, sizeof g_pendingArg, "%s", arg + 1);
        builtin = B_SUMMON;
    } else if (Is(command, "sellsim")) {                               // test aid: sellsim <item>: as sold to a vendor
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        snprintf(g_pendingArg, sizeof g_pendingArg, "%s", arg + 1);
        builtin = B_SELLSIM;
    } else if (Is(command, "castat")) {                                // test aid: castat <spell> <ref>
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        snprintf(g_pendingArg, sizeof g_pendingArg, "%s", arg + 1);
        builtin = B_CASTAT;
    } else if (Is(command, "thwidth")) {                               // test aid: thwidth <w> setWidth again on the boss bars
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        truehud::SetBossWidth(atoi(arg + 1));
        return true;
    } else if (Is(command, "thdir")) {                                 // test aid: thdir <0|1|2> the boss bars' fill direction
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        truehud::SetBossDir(atoi(arg + 1));
        return true;
    } else if (Is(command, "wbtest")) {                                // test aid: wbtest <boss 0..5> <marker FormID>
        unsigned k = 0, m = 0;
        if (sscanf(command + 6, "%u %x", &k, &m) == 2) scrolls::WorldBossTest((int)k, m);
        return true;
    } else if (Is(command, "smugglerday")) {                           // test aid: smugglerday <days>
        const char* arg = strchr(command, ' ');
        scrolls::AddDays(arg ? strtof(arg + 1, nullptr) : 10.0f);
        return true;
    } else if (Is(command, "shader")) {
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        snprintf(g_pendingArg, sizeof g_pendingArg, "%s", arg + 1);
        builtin = B_SHADER;
    } else if (Is(command, "place")) {
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        snprintf(g_pendingArg, sizeof g_pendingArg, "%s", arg + 1);
        builtin = B_PLACE;
    } else if (Is(command, "readbook")) {
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        snprintf(g_pendingArg, sizeof g_pendingArg, "%s", arg + 1);
        builtin = B_READBOOK;
    } else if (Is(command, "resetref")) {
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        snprintf(g_pendingArg, sizeof g_pendingArg, "%s", arg + 1);
        builtin = B_RESETREF;
    } else if (Is(command, "additem")) {
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        snprintf(g_pendingArg, sizeof g_pendingArg, "%s", arg + 1);
        builtin = B_ADDITEM;
    } else if (Is(command, "refinfo")) {
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        snprintf(g_pendingArg, sizeof g_pendingArg, "%s", arg + 1);
        builtin = B_REFINFO;
    } else if (Is(command, "near")) {                                  // test aid: furniture around (log); near <type hex>: that form type
        const char* arg = strchr(command, ' ');
        snprintf(g_pendingArg, sizeof g_pendingArg, "%s", arg ? arg + 1 : "28");
        builtin = B_NEAR;
    } else if (Is(command, "use")) {                                   // test aid: use <ref FormID> (activate)
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        snprintf(g_pendingArg, sizeof g_pendingArg, "%s", arg + 1);
        builtin = B_USE;
    } else if (Is(command, "scene")) {                                  // scene [offset] [words]: 3D dump (log)
        const char* arg = strchr(command, ' ');
        snprintf(g_pendingArg, sizeof g_pendingArg, "%s", arg ? arg + 1 : "8 48");
        builtin = B_SCENE;
    } else if (Is(command, "extras")) {
        builtin = B_EXTRAS;
    } else if (Is(command, "glowtest")) {
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        snprintf(g_pendingArg, sizeof g_pendingArg, "%s", arg + 1);
        builtin = B_GLOWTEST;
    } else if (Is(command, "goto")) {                                   // goto <ref FormID>
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        snprintf(g_pendingArg, sizeof g_pendingArg, "%s", arg + 1);
        builtin = B_GOTO;
    } else if (Is(command, "drop")) {                                   // test aid: drop <hex form>
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        snprintf(g_pendingArg, sizeof g_pendingArg, "%s", arg + 1);
        builtin = B_DROP;
    } else if (Is(command, "heading") || Is(command, "faceref")) {       // test aid: heading <deg> | faceref <hex>
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        snprintf(g_pendingArg, sizeof g_pendingArg, "%s%s", Is(command, "faceref") ? "r" : "", arg + 1);
        builtin = B_FACE_TO;
    } else if (Is(command, "camdump")) {
        builtin = B_CAMDUMP;
    } else if (Is(command, "glob")) {                                   // test aid: glob <hex FormID> [value]
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        snprintf(g_pendingArg, sizeof g_pendingArg, "%s", arg + 1);
        builtin = B_GLOB;
    } else if (Is(command, "av")) {                                     // test aid: av <actor value name>
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        snprintf(g_pendingArg, sizeof g_pendingArg, "%s", arg + 1);
        builtin = B_AV;
    } else if (Is(command, "kill")) {                                   // kill <FormID>
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        snprintf(g_pendingArg, sizeof g_pendingArg, "%s", arg + 1);
        builtin = B_KILL;
    } else if (Is(command, "unequipall") && equip::Available()) {
        builtin = B_UNEQUIPALL;
    } else if (Is(command, "trade")) {                                // trade [player id]: open the exchange
        RemotePlayer rs[MAX_REMOTE];
        int n = SnapshotRemotes(rs, MAX_REMOTE);
        const char* arg = strchr(command, ' ');
        u16 id = arg ? (u16)atoi(arg + 1) : (n ? rs[0].state.id : 0);
        if (!id) { game::Notify("Кооп: никого нет"); return false; }
        trade::Start(id);
        return true;
    } else if (Is(command, "tradeput")) {                             // test: tradeput <hex form> <count>
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        char* end;
        u32 f = strtoul(arg + 1, &end, 16);
        trade::TestPut(f, (s32)atoi(end));
        return true;
    } else if (Is(command, "follow")) {                               // follow <hex FormID> <player id | 0>
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        char* end;
        u32 f = strtoul(arg + 1, &end, 16);
        followers::Command(f, (u16)atoi(end));
        return true;
    } else if (Is(command, "social")) {                               // social <server command>: clans, marriage
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        SocialSend(arg + 1, false);
        return true;
    } else if (Is(command, "clanhouse")) {                            // go to the clan house
        SocialSend("info", true);
        return true;
    } else if (Is(command, "avatar")) {                               // avatar save | avatar load
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        if (!strcmp(arg + 1, "save")) avatar::RequestSave();
        else if (!strcmp(arg + 1, "load")) avatar::RequestLoad();
        else return false;
        return true;
    } else if (Is(command, "queststage")) {                           // queststage <hex quest FormID> <stage>
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        char* end;
        u32 q = strtoul(arg + 1, &end, 16);
        while (*end == ' ') end++;
        quests::SetLocal(q, *end ? (u16)atoi(end) : (u16)0xFFFF);        // no stage: only show it
        return true;
    } else if (Is(command, "damage")) {                               // damage <hex FormID> <amount>
        const char* arg = strchr(command, ' ');
        if (!arg) return false;
        char* end;
        u32 f = strtoul(arg + 1, &end, 16);
        npcsync::TestDamage(f, strtof(end, nullptr));
        return true;
    } else if (Is(command, "coopdebug")) {                            // diagnostics to the server: coopdebug [on|off]
        const char* arg = strchr(command, ' ');
        if (arg) SetDebug(!strcmp(arg + 1, "on"));
        else ToggleDebug();
        return true;
    } else if (Is(command, "face")) {                                 // test aid: face <player id>
        const char* who = strchr(command, ' ');
        if (!who) return false;
        g_tpTarget = (u16)atoi(who + 1);
        g_pending = B_FACE;
        return true;
    } else if (Is(command, "tp")) {                                   // co-op: go to another player
        const char* who = strchr(command, ' ');
        RemotePlayer rs[MAX_REMOTE];
        int n = SnapshotRemotes(rs, MAX_REMOTE);
        for (int i = 0; i < n; i++) {
            char name[32];
            bool named = PlayerName(rs[i].state.id, name, sizeof name);
            if (who && !(named && SameName(name, who + 1)) && (u32)atoi(who + 1) != rs[i].state.id) continue;
            if (g_moveTo) {
                g_tpTarget = rs[i].state.id;
                g_pending = B_TP;
                return true;
            }
        }
        game::Notify("Кооп: такого игрока рядом нет");
        return false;
    }
    if (builtin != B_NONE) {
        g_pending = builtin;
        Debug("console builtin: %s", command);
        return true;
    }
    if (!g_ok) return false;
    void* script = game::GameAlloc(0x78);
    void* task = game::GameAlloc(0x18);
    if (!script || !task) return false;
    reinterpret_cast<void (*)(void*)>(g_ctor)(script);
    reinterpret_cast<void (*)(void*)>(g_init)(script);
    reinterpret_cast<void (*)(void*, const char*)>(g_setText)(script, command);
    *reinterpret_cast<void**>(static_cast<u8*>(script) + 0x50) = game::Singleton(game::ModuleAddr(SCRIPT_GLOBAL_SLOT));
    void* scriptPtr = script;      // unique_ptr<Script>: the task takes it over
    void* target = nullptr;        // no selected reference ("player.additem ..." names its target)
    reinterpret_cast<void (*)(void*, void**, void**, void*)>(g_taskCtor)(task, &scriptPtr, &target, g_compiler);
    void* queue = reinterpret_cast<void* (*)()>(g_queueGet)();
    reinterpret_cast<void (*)(void*, void*)>(g_queuePush)(queue, task);
    Debug("console: %s (queue drains so far %u)", command, g_drains);
    return true;
}

void Prompt() {
    static char last[200];
    char cmd[200];
    if (!settings::AskText(u"Консоль", u"Как в консоли ПК: tgm, tcl, tai, tm, coc Riverwood, tp Друг, setrace орк, equip 12e49, player.additem f 100",
                           last, cmd, sizeof cmd))
        return;
    snprintf(last, sizeof last, "%s", cmd);
    char msg[240];
    snprintf(msg, sizeof msg, Execute(cmd) ? "Консоль: %s" : "Консоль недоступна: %s", cmd);
    game::Notify(msg);
}

}  // namespace coop::console
