#include "game.hpp"
#include "client.hpp"
#include "appearance.hpp"
#include "equip.hpp"
#include "actions.hpp"
#include "trade.hpp"
#include "pvp.hpp"
#include "followers.hpp"
#include "quests.hpp"
#include "avatar.hpp"
#include "magic.hpp"
#include "weather.hpp"
#include "mount.hpp"
#include "plates.hpp"
#include "scrolls.hpp"
#include "glow.hpp"
#include "chest.hpp"
#include "npcpos.hpp"
#include "actors.hpp"
#include "projectiles.hpp"
#include "npcsync.hpp"
#include "world.hpp"
#include "party.hpp"
#include "items.hpp"
#include "truehud.hpp"
#include "booksort.hpp"
#include "updates.hpp"
#include "death.hpp"
#include "settings.hpp"
#include "menu.hpp"
#include "console.hpp"
#include "lib.hpp"
#include "lib/patch/code_patcher.hpp"
#include <nn/os.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace nn::hid {
    /* Only the addresses are needed (hook targets); the layouts are never touched. */
    struct NpadFullKeyState;
    struct NpadHandheldState;
    struct NpadJoyDualState;
    void GetNpadState(NpadFullKeyState* out, const u32& id);
    void GetNpadState(NpadHandheldState* out, const u32& id);
    void GetNpadState(NpadJoyDualState* out, const u32& id);
}

namespace coop { volatile const char* g_gameStep = "-"; volatile u32 g_hookIn[4], g_hookOut[4]; }
namespace coop::game {
void PumpTeleport();                 // defined below (public section)
bool PluginList(char* out, u32 size);
namespace {

Resolved g_res{};
bool g_ready = false;

/* ------------------------------------------------------------------ AArch64 decoding */

bool IsAdrp(u32 w)               { return (w & 0x9F000000) == 0x90000000; }
u32  Rd(u32 w)                   { return w & 0x1F; }
u32  Rn(u32 w)                   { return (w >> 5) & 0x1F; }
uintptr_t AdrpPage(uintptr_t pc, u32 w) {
    s64 imm = (s64)((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3));
    if (imm & (1 << 20)) imm -= (1 << 21);
    return (pc & ~(uintptr_t)0xFFF) + (imm << 12);
}
bool IsAddImm64(u32 w)           { return (w & 0xFFC00000) == 0x91000000; }   // ADD Xd, Xn, #imm12
u32  AddImm(u32 w)               { return (w >> 10) & 0xFFF; }
bool IsLdrX(u32 w)               { return (w & 0xFFC00000) == 0xF9400000; }   // LDR Xt, [Xn, #imm*8]
bool IsLdrW(u32 w)               { return (w & 0xFFC00000) == 0xB9400000; }   // LDR Wt, [Xn, #imm*4]
bool IsLdrS(u32 w)               { return (w & 0xFFC00000) == 0xBD400000; }   // LDR St, [Xn, #imm*4]
bool IsLdrH(u32 w)               { return (w & 0xFFC00000) == 0x79400000; }   // LDRH Wt, [Xn, #imm*2]
u32  LdrImm(u32 w)               { return (w >> 10) & 0xFFF; }
bool IsB(u32 w)                  { return (w & 0xFC000000) == 0x14000000; }
uintptr_t BTarget(uintptr_t pc, u32 w) {
    s64 imm = w & 0x3FFFFFF;
    if (imm & (1 << 25)) imm -= (1 << 26);
    return pc + imm * 4;
}

const u32* Code(uintptr_t a) { return reinterpret_cast<const u32*>(a); }

/* ------------------------------------------------------------------ native lookup */

struct Span { uintptr_t start, end; bool In(uintptr_t a) const { return start <= a && a < end; } };

Span TextSpan() { auto& m = exl::util::GetMainModuleInfo(); return {m.m_Text.m_Start, m.m_Text.GetEnd()}; }
Span RoSpan()   { auto& m = exl::util::GetMainModuleInfo(); return {m.m_Rodata.m_Start, m.m_Rodata.GetEnd()}; }

constexpr int MAX_HITS = 4;

/* Every "<name>\0" in .rodata that starts right after a NUL. */
int FindStrings(const char* name, uintptr_t* out) {
    Span ro = RoSpan();
    size_t len = strlen(name) + 1;
    int n = 0;
    auto p = reinterpret_cast<const char*>(ro.start + 1);
    auto end = reinterpret_cast<const char*>(ro.end - len);
    while (p < end && n < MAX_HITS) {
        p = static_cast<const char*>(memchr(p, name[0], end - p));
        if (!p) break;
        if (p[-1] == 0 && memcmp(p, name, len) == 0) out[n++] = reinterpret_cast<uintptr_t>(p);
        p++;
    }
    return n;
}

struct Wanted { const char* name; uintptr_t str[MAX_HITS]; int nstr; uintptr_t fn; };

/* Address computed by the "ADD Xd, Xn, #imm" at index j, pairing it with the ADRP of Xn
   up to 12 instructions back (compilers interleave the pairs). 0 = not an ADRP+ADD pair. */
uintptr_t PairAt(const u32* c, size_t j, uintptr_t base) {
    u32 d = c[j];
    if (!IsAddImm64(d)) return 0;
    for (size_t m = 1; m <= 12 && m <= j; m++) {
        u32 a = c[j - m];
        if (IsAdrp(a) && Rd(a) == Rn(d)) return AdrpPage(base + (j - m) * 4, a) + AddImm(d);
    }
    return 0;
}

/* One pass over .text: every ADD that completes an ADRP+ADD pair computing a wanted name
   string; the first pair computing a .text address within the next 16 instructions is the native. */
void FindNatives(Wanted* w, int count) {
    Span text = TextSpan();
    for (int i = 0; i < count; i++) w[i].nstr = FindStrings(w[i].name, w[i].str);
    const u32* c = Code(text.start);
    size_t words = (text.end - text.start) / 4;
    for (size_t i = 1; i + 20 < words; i++) {
        if (!IsAddImm64(c[i])) continue;
        uintptr_t addr = PairAt(c, i, text.start);
        if (!addr) continue;
        for (int n = 0; n < count; n++) {
            if (w[n].fn) continue;
            bool hit = false;
            for (int s = 0; s < w[n].nstr; s++) hit |= (w[n].str[s] == addr);
            if (!hit) continue;
            for (size_t j = i + 1; j < i + 16; j++) {
                uintptr_t fn = PairAt(c, j, text.start);
                if (fn && text.In(fn)) { w[n].fn = fn; break; }
            }
        }
    }
}

/* ------------------------------------------------------------------ decoders */

/* first "ldr <kind> ?, [x2, #imm]" in the first n instructions -> byte offset */
bool FieldOffset(uintptr_t fn, bool (*is)(u32), u32 scale, u32* out, int n = 6) {
    for (int i = 0; i < n; i++) {
        u32 w = Code(fn)[i];
        if (is(w) && Rn(w) == 2) { *out = LdrImm(w) * scale; return true; }
    }
    return false;
}

/* GetPlayer: adrp x8, page; ldr x8, [x8, #off]; ldr x0, [x8]; ret -> GOT slot */
bool DecodeGetPlayer(uintptr_t fn, uintptr_t* slot) {
    const u32* c = Code(fn);
    if (!IsAdrp(c[0]) || !IsLdrX(c[1]) || Rn(c[1]) != Rd(c[0]) || !IsLdrX(c[2])) return false;
    *slot = AdrpPage(fn, c[0]) + LdrImm(c[1]) * 8;
    return true;
}

/* first unconditional B in the first n instructions (tail call) */
bool TailCall(uintptr_t fn, uintptr_t* out, int n = 16) {
    for (int i = 0; i < n; i++) {
        u32 w = Code(fn)[i];
        if (IsB(w)) { *out = BTarget(fn + i * 4, w); return TextSpan().In(*out); }
    }
    return false;
}

/* ------------------------------------------------------------------ register tracking */

/* Linear walk with a tiny symbolic state per register: ADRP -> page, ADD page -> address,
   LDR [page+imm] -> GOT slot, LDR [slot] -> *slot, MOV copies, any other write forgets.
   Mirrors tools/puppet_resolve.py. */
enum SymKind : u8 { SYM_NONE, SYM_PAGE, SYM_ADDR, SYM_SLOT, SYM_DEREF };
struct Sym { SymKind kind; uintptr_t v; };

bool IsBl(u32 w)      { return (w & 0xFC000000) == 0x94000000; }
bool IsBlr(u32 w)     { return (w & 0xFFFFFC1F) == 0xD63F0000; }
bool IsMovX(u32 w)    { return (w & 0xFFE0FFE0) == 0xAA0003E0; }
bool IsStrX(u32 w)    { return (w & 0xFFC00000) == 0xF9000000; }
bool IsAndFffff(u32 w){ return (w & 0xFFFFFC00) == 0x12004C00; }   // and wd, wn, #0xfffff
bool IsAddUxtw4(u32 w){ return (w & 0xFFE0FC00) == 0x8B205000; }   // add xd, xn, wm, uxtw #4
uintptr_t BlTarget(uintptr_t pc, u32 w) { return BTarget(pc, w); }

bool WritesRd(u32 w) {
    if ((w & 0x3A000000) == 0x38000000) return w & 0x00400000;            // load/store: loads only
    if ((w & 0x7C000000) == 0x14000000 || (w & 0xFE000000) == 0x54000000) return false;
    if ((w & 0x7E000000) == 0x34000000 || (w & 0x7E000000) == 0x36000000) return false;
    if (IsBlr(w) || (w & 0xFFFFFC1F) == 0xD61F0000 || w == 0xD65F03C0) return false;
    if ((w & 0x7F20001F) == 0x6B00001F || (w & 0x7F80001F) == 0x7100001F) return false;
    u32 top = w & 0xFFC00000;
    if (top == 0xA9000000 || top == 0xA9800000 || top == 0x29000000 || top == 0x6D000000) return false;
    return true;
}

template<typename F>
void Track(uintptr_t start, uintptr_t stop, F&& visit) {
    Sym regs[32] = {};
    for (uintptr_t pc = start; pc < stop; pc += 4) {
        u32 w = *Code(pc);
        if (!visit(pc, w, regs)) return;
        u32 d = Rd(w);
        if (IsAdrp(w)) regs[d] = {SYM_PAGE, AdrpPage(pc, w)};
        else if (IsLdrX(w)) {
            Sym b = regs[Rn(w)]; u32 imm = LdrImm(w) * 8;
            regs[d] = b.kind == SYM_PAGE ? Sym{SYM_SLOT, b.v + imm}
                    : (b.kind == SYM_SLOT && imm == 0) ? Sym{SYM_DEREF, b.v} : Sym{};
        } else if (IsAddImm64(w)) {
            Sym b = regs[Rn(w)];
            regs[d] = b.kind == SYM_PAGE ? Sym{SYM_ADDR, b.v + AddImm(w)} : Sym{};
        } else if (IsMovX(w)) regs[d] = regs[(w >> 16) & 0x1F];
        else if (IsBl(w) || IsBlr(w)) for (int r = 0; r < 19; r++) regs[r] = {};
        else if (WritesRd(w)) regs[d] = {};
    }
}

struct PuppetApi {
    uintptr_t create, dataHandlerSlot, handleTableSlot, alloc, taskVtable, queue, queueSlot;
};
PuppetApi g_pup{};
bool g_puppets = false;
uintptr_t g_deleteNative;       // ObjectReference.Delete(vm, stackId, ref): SetDelete + unload, VM only for errors

bool ResolvePuppets(uintptr_t placeAtMe, uintptr_t setPosition) {
    PuppetApi p{};
    uintptr_t lastAnd = 0;
    Track(placeAtMe, placeAtMe + 0x800, [&](uintptr_t pc, u32 w, const Sym* r) {
        if (IsBl(w) && !p.create && r[0].kind == SYM_DEREF) {
            for (int k = 1; k <= 10; k++)
                if (Code(pc)[-k] == 0xAA1F03E7) { p.create = BlTarget(pc, w); p.dataHandlerSlot = r[0].v; break; }   // mov x7, xzr
        }
        if (IsAndFffff(w)) lastAnd = pc;
        if (lastAnd && IsAddUxtw4(w) && pc - lastAnd <= 16 && !p.handleTableSlot && r[Rn(w)].kind == SYM_SLOT)
            p.handleTableSlot = r[Rn(w)].v;
        return !(p.create && p.handleTableSlot);
    });
    u32 prev = 0;
    Track(setPosition, setPosition + 0x400, [&](uintptr_t pc, u32 w, const Sym* r) {
        if (IsBl(w) && prev == 0x52800500 && !p.alloc) p.alloc = BlTarget(pc, w);          // mov w0, #0x28
        else if (p.alloc && !p.taskVtable && IsStrX(w) && LdrImm(w) == 0 && r[Rd(w)].kind == SYM_ADDR)
            p.taskVtable = r[Rd(w)].v;
        else if (p.taskVtable && !p.queue && IsBl(w) && r[0].kind == SYM_DEREF) { p.queue = BlTarget(pc, w); p.queueSlot = r[0].v; }
        prev = w;
        return !p.queue;
    });
    if (!p.create || !p.handleTableSlot || !p.alloc || !p.taskVtable || !p.queue) {
        Log("puppets off: create %lx table %lx alloc %lx vt %lx queue %lx", p.create, p.handleTableSlot, p.alloc, p.taskVtable, p.queue);
        return false;
    }
    g_pup = p;
    return true;
}


/* ------------------------------------------------------------------ puppet animation (1.1.177)
   Found by hand from the Papyrus natives (tools/xrefs.py), used only when the first instructions
   match this build (the user's console runs the dumped 1.1.177; anything else: no animation):
     Actor.EnableAI            -> 0x607750  void Actor::EnableAI(Actor*, bool)   (flag at +0x118)
     SetAnimationVariableFloat -> 0x533b80  bool SetGraphVariableFloat(holder, const BSFixedString&, float)
     Debug.SendAnimationEvent  -> holder->vtable[2](holder, const BSFixedString&)  (NotifyAnimationGraph)
     BSFixedString(const char*) 0xae6da8, ~BSFixedString 0xae6ab0; holder = ref + 0x38 */
struct AnimFn { u32 off; u32 sig[4]; };
constexpr AnimFn ANIM_ENABLE_AI{0x607750, {0xD10083FF, 0xA9017BFD, 0x910043FD, 0xB9411808}};
constexpr AnimFn ANIM_SET_FLOAT{0x533b80, {0xD10103FF, 0xFD0013E8, 0xF9001FF3, 0xA902FBFD}};
constexpr AnimFn ANIM_FS_CTOR{0xae6da8, {0xA9BE7BFD, 0xF9000BF3, 0x910003FD, 0xF900001F}};
constexpr AnimFn ANIM_FS_DTOR{0xae6ab0, {0x14000004, 0, 0, 0}};

struct AnimApi { bool ok; uintptr_t enableAi, setFloat, fsCtor, fsDtor; } g_anim{};

uintptr_t CheckedFn(const AnimFn& f) {
    uintptr_t a = exl::util::modules::GetTargetOffset(f.off);
    if (!TextSpan().In(a)) return 0;
    for (int i = 0; i < 4; i++) if (Code(a)[i] != f.sig[i]) return 0;
    return a;
}

void ResolveAnim() {
    g_anim = {false, CheckedFn(ANIM_ENABLE_AI), CheckedFn(ANIM_SET_FLOAT), CheckedFn(ANIM_FS_CTOR), CheckedFn(ANIM_FS_DTOR)};
    g_anim.ok = g_anim.enableAi && g_anim.setFloat && g_anim.fsCtor && g_anim.fsDtor;
}

struct FixedString { const char* data = nullptr; };

template<typename F> void WithFixedString(const char* text, F&& use) {
    FixedString fs;
    reinterpret_cast<void (*)(FixedString*, const char*)>(g_anim.fsCtor)(&fs, text);
    use(&fs);
    reinterpret_cast<void (*)(FixedString*)>(g_anim.fsDtor)(&fs);
}

void* GraphHolder(void* ref) { return static_cast<u8*>(ref) + 0x38; }

bool AnimEvent(void* ref, const char* event) {
    if (!g_anim.ok) return false;
    bool ok = false;
    WithFixedString(event, [&](FixedString* fs) {
        void* holder = GraphHolder(ref);
        auto vt = *reinterpret_cast<uintptr_t**>(holder);
        ok = reinterpret_cast<bool (*)(void*, FixedString*)>(vt[2])(holder, fs);
    });
    return ok;
}

void AnimFloat(void* ref, const char* name, float value) {
    if (!g_anim.ok) return;
    WithFixedString(name, [&](FixedString* fs) {
        reinterpret_cast<bool (*)(void*, FixedString*, float)>(g_anim.setFloat)(GraphHolder(ref), fs, value);
    });
}

/* Puppets keep their AI: with it off Skyrim freezes the actor like a statue (no animation at all).
   Their base is a copy of the player's NPC, which has no AI packages, so they do not wander. */
constexpr bool KEEP_AI = true;

void EnableAi(void* ref, bool on) {
    if (!on && KEEP_AI) return;
    if (g_anim.ok) reinterpret_cast<void (*)(void*, bool)>(g_anim.enableAi)(ref, on);
}

/* ------------------------------------------------------------------ puppets */

using NotifyFn = void (*)(const char*, const char*, bool);
using WorldFn = void* (*)(void*);
using IsDeadFn = bool (*)(void*, bool);

template<typename T> T Field(void* obj, u32 off) { return *reinterpret_cast<T*>(static_cast<u8*>(obj) + off); }

u32 FormId(void* form) { return form ? Field<u32>(form, g_res.offFormId) : 0; }

/* ObjectRefHandle is returned through x8 (non-trivial type), as the game does. */
struct RefHandle {
    u32 v = 0;
    RefHandle() {}
    RefHandle(const RefHandle& o) : v(o.v) {}
    ~RefHandle() {}
};
using CreateFn = RefHandle (*)(void* dataHandler, void* base, const float* pos, const float* rot, void* cell,
                               void* worldspace, void* alreadyCreated, void* primitive, u64 linkedRoom,
                               bool forcePersist, bool arg11);
using AllocFn = void* (*)(u64 size);
using QueueFn = bool (*)(void* queue, void** task);

/* GOT slot -> global variable -> object: "ldr x8, [page, #slot]; ldr x0, [x8]" (SYM_DEREF). */
void* SingletonImpl(uintptr_t slot) {
    auto var = *reinterpret_cast<void***>(slot);
    return var ? *var : nullptr;
}

/* handle -> TESObjectREFR*, the same validity checks as the game's inline lookup */
void* LookupRef(u32 h) {
    if (!h) return nullptr;
    auto table = *reinterpret_cast<u8**>(g_pup.handleTableSlot);
    u32 idx = h & 0xFFFFF;
    u8* e = table + idx * 16;
    u32 flags = *reinterpret_cast<u32*>(e);
    uintptr_t p = *reinterpret_cast<uintptr_t*>(e + 8);
    if (!p || !(flags & (1u << 26)) || ((flags ^ h) & 0x3F00000)) return nullptr;
    void* ref = reinterpret_cast<void*>(p - 0x20);
    if ((*reinterpret_cast<u32*>(static_cast<u8*>(ref) + 0x28) >> 11) != idx) return nullptr;
    return ref;
}

/* Stand-in for the Papyrus VM in our tasks: the task only calls it to log "object not found"
   (vtable +0x20) and refcounts it at +8. Every slot is a no-op, the count never reaches zero. */
u64 NopMethod() { return 0; }
struct FakeVm { const void* const* vtable; u32 refcount; u32 pad; };
const void* g_fakeVmTable[64];
FakeVm g_fakeVm{g_fakeVmTable, 1u << 28, 0};

void* FakeVmPtr() {
    if (!g_fakeVmTable[0])
        for (auto& slot : g_fakeVmTable) slot = reinterpret_cast<const void*>(&NopMethod);
    __atomic_add_fetch(&g_fakeVm.refcount, 1u, __ATOMIC_RELAXED);   // the task drops one on destruction
    return &g_fakeVm;
}

/* The game's own SetPosition task (runs MoveRefrToPosition on the main thread). */
void QueueMove(u32 handle, float x, float y, float z) {
    void* queue = SingletonImpl(g_pup.queueSlot);
    if (!queue) return;
    auto t = static_cast<u8*>(reinterpret_cast<AllocFn>(g_pup.alloc)(0x28));
    if (!t) return;
    *reinterpret_cast<uintptr_t*>(t) = g_pup.taskVtable;
    *reinterpret_cast<u32*>(t + 0x08) = 1;          // refcount: our reference
    *reinterpret_cast<u32*>(t + 0x0C) = 0;          // stack id
    *reinterpret_cast<u32*>(t + 0x10) = handle;
    reinterpret_cast<float*>(t + 0x14)[0] = x;
    reinterpret_cast<float*>(t + 0x14)[1] = y;
    reinterpret_cast<float*>(t + 0x14)[2] = z;
    *reinterpret_cast<void**>(t + 0x20) = FakeVmPtr();  // VM: only used to report a missing object
    void* holder = t;
    reinterpret_cast<QueueFn>(g_pup.queue)(queue, &holder);
    if (__atomic_sub_fetch(reinterpret_cast<u32*>(t + 0x08), 1u, __ATOMIC_ACQ_REL) == 0) {
        auto vt = *reinterpret_cast<uintptr_t**>(t);
        reinterpret_cast<void (*)(void*)>(vt[1])(t);   // deleting destructor
    }
}


/* Animation wanted per puppet: written by the controller-poll thread, applied by MainRun on a
   game thread (graph calls go into Havok, which needs a game thread's thread-local state). */
uintptr_t g_setPosition;          // Actor::SetPosition (see MoveSmooth)
struct AnimWanted { u32 handle; bool moving; float speed; bool eventDue; bool aiOffDue; bool drawn; bool drawDue; bool sneak;
                    float tx, ty, tz, rot; bool hasTarget; };
AnimWanted g_animWanted[MAX_REMOTE];
nn::os::MutexType g_animLock;
bool g_animLockReady;

void AnimLock()   { if (!g_animLockReady) { nn::os::InitializeMutex(&g_animLock, false, 0); g_animLockReady = true; } nn::os::LockMutex(&g_animLock); }
void AnimUnlock() { nn::os::UnlockMutex(&g_animLock); }

struct Puppet {
    u16 id;
    u32 handle;
    float x, y, z, rot;
    u64 lastSpawnMs;
    u64 lastReportMs;
    bool moving;
    bool drawn;
    u64 firstSeenMs;
    void* base;             // NPC base it was spawned from
    bool dead;              // its player died: it lies there until they come back
};
Puppet g_puppetsList[MAX_REMOTE];
int g_puppetCount;

constexpr float INTRO_SIDE = 45.0f;    // units to the left of their seat

u32 Spawn(void* player, const proto::State& s, void** usedBase) {
    /* the other player's own NPC (race, face, name) once their appearance arrived, else a copy of ours */
    void* base = appearance::BaseFor(s.id);
    if (!base) base = Field<void*>(player, 0x40);
    *usedBase = base;
    float pos[3] = {s.x, s.y, s.z};
    float rot[3] = {0, 0, s.rotZ};
    void* cell = Field<void*>(player, g_res.offCell);
    void* ws = reinterpret_cast<WorldFn>(g_res.getWorldspace)(player);
    RefHandle h = reinterpret_cast<CreateFn>(g_pup.create)(SingletonImpl(g_pup.dataHandlerSlot), base,
                                                           pos, rot, cell, ws, nullptr, nullptr, 0, false, true);
    /* Hold a reference like Papyrus does after PlaceAtMe (count = low 10 bits at +0x28);
       without it the new reference is deleted right away. */
    void* ref = LookupRef(h.v);
    if (ref) __atomic_add_fetch(reinterpret_cast<u32*>(static_cast<u8*>(ref) + 0x28), 1u, __ATOMIC_ACQ_REL);
    Debug("spawned puppet for %u: handle %x ref %p", s.id, h.v, ref);
    return ref ? h.v : 0;
}

/* Puppets whose player died: killed on the game thread (VM pump). Only a puppet with its own base (the
   player's look) is made mortal for that; the base is essential again when the puppet is spawned anew. */
uintptr_t g_killImpl;          // checked before the hook changes its first instructions
u32 g_toKill[MAX_REMOTE];
int g_toKillCount;

/* Puppets to remove, deleted on the game thread (VM pump). */
u32 g_toDelete[MAX_REMOTE * 2];
int g_toDeleteCount;

void QueueDelete(Puppet* p) {
    if (!p->handle) return;
    AnimLock();
    if (g_toDeleteCount < MAX_REMOTE * 2) g_toDelete[g_toDeleteCount++] = p->handle;
    g_animWanted[p - g_puppetsList] = AnimWanted{};
    AnimUnlock();
    Log("puppet %u removed", p->id);
    /* (08.10, Eden: the render crash 0xbf9940 <- 0xd91f34 <- 0xdb5b68 came every time right after this) no move 30000
       units under the ground first: the plain Delete on the next pump takes its 3D away properly */
    p->handle = 0;
    p->base = nullptr;
    p->lastSpawnMs = 0;
}

void KillPuppets() {
    u32 todo[MAX_REMOTE];
    AnimLock();
    int n = g_toKillCount;
    memcpy(todo, g_toKill, n * sizeof(u32));
    g_toKillCount = 0;
    AnimUnlock();
    for (int i = 0; i < n; i++) {
        void* ref = LookupRef(todo[i]);
        if (!ref || !g_killImpl) continue;
        void* base = Field<void*>(ref, 0x40);
        if (!base || base == Field<void*>(Player(), 0x40)) continue;
        appearance::SetEssential(base, false);
        reinterpret_cast<void (*)(void*, void*, float, bool, bool)>(g_killImpl)(ref, nullptr, 0.0f, true, false);
        Debug("puppet ref %x: its player died", todo[i]);
    }
}

void DeletePuppets() {
    u32 todo[MAX_REMOTE * 2];
    AnimLock();
    int n = g_toDeleteCount;
    memcpy(todo, g_toDelete, n * sizeof(u32));
    g_toDeleteCount = 0;
    AnimUnlock();
    if (!g_deleteNative) return;
    FakeVmPtr();                                       // fills the no-op vtable
    for (int i = 0; i < n; i++)
        if (void* ref = LookupRef(todo[i])) {
            reinterpret_cast<bool (*)(void*, u32, void*)>(g_deleteNative)(&g_fakeVm, 0, ref);
            /* drop the hold taken at spawn so the engine can free it (never below 1 here) */
            auto count = reinterpret_cast<u32*>(static_cast<u8*>(ref) + 0x28);
            u32 c = __atomic_load_n(count, __ATOMIC_ACQUIRE);
            while ((c & 0x3FF) > 1 && !__atomic_compare_exchange_n(count, &c, c - 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
            Debug("puppet ref %x deleted: flags %x, 3D %p, count %u", todo[i], *reinterpret_cast<u32*>(static_cast<u8*>(ref) + 0x10),
                *reinterpret_cast<void**>(static_cast<u8*>(ref) + 0x68), __atomic_load_n(count, __ATOMIC_RELAXED) & 0x3FF);
        }
}

void UpdatePuppets(void* player, const RemotePlayer* rs, int n, u64 now) {
    static u64 inWorldSince;
    if (!Field<void*>(player, g_res.offCell)) { inWorldSince = 0; return; }   // loading: no cell yet
    if (!inWorldSince) inWorldSince = now;
    if (now - inWorldSince < 5000) return;     // references made while the world finishes loading get purged
    static bool dumped;
    if (!dumped) {                              // RE aid: vtables of the player's NPC base and of the player
        dumped = true;
        uintptr_t mod = exl::util::modules::GetTargetStart();
        void* base = Field<void*>(player, 0x40);
        uintptr_t bvt = *reinterpret_cast<uintptr_t*>(base), pvt = *reinterpret_cast<uintptr_t*>(player);
        Debug("RE: player base form %08x type %02x vtable +%lx, player vtable +%lx, base+0x30 %lx",
            FormId(base), *reinterpret_cast<u8*>(static_cast<u8*>(base) + 0x1a), bvt - mod, pvt - mod,
            *reinterpret_cast<uintptr_t*>(static_cast<u8*>(base) + 0x30));
    }
    for (int k = 0; k < g_puppetCount; k++) {                 // players who left the room
        Puppet& p = g_puppetsList[k];
        bool present = false;
        for (int i = 0; i < n; i++) present |= rs[i].state.id == p.id;
        if (p.id && !present) { QueueDelete(&p); p.id = 0; }
    }
    u32 myCell = FormId(Field<void*>(player, g_res.offCell));
    u32 myWorld = FormId(reinterpret_cast<WorldFn>(g_res.getWorldspace)(player));
    for (int i = 0; i < n; i++) {
        const auto& s = rs[i].state;
        bool here = myWorld ? s.worldspace == myWorld : s.cell == myCell;   // exterior: same world; interior: same cell
        if (!quests::IntroDone()) here = false;          // (03.10) no other bodies in our intro scene: a puppet in the cart
                                                          // pushed the prisoners out of it
        if (here && myWorld) {                             // exterior: only within the loaded cells around us
            const float* me = reinterpret_cast<const float*>(static_cast<u8*>(player) + g_res.offPos);
            float dx = s.x - me[0], dy = s.y - me[1];
            here = dx * dx + dy * dy < 9000.0f * 9000.0f;
        }
        Puppet* p = nullptr;
        for (int k = 0; k < g_puppetCount; k++) if (g_puppetsList[k].id == s.id) p = &g_puppetsList[k];
        for (int k = 0; !p && k < g_puppetCount; k++) if (!g_puppetsList[k].id) p = &g_puppetsList[k];   // reuse
        if (!p) {
            if (g_puppetCount >= MAX_REMOTE) continue;
            p = &g_puppetsList[g_puppetCount++];
        }
        if (p->id != s.id) {
            *p = Puppet{s.id, 0, s.x, s.y, s.z, s.rotZ, 0, 0, false, false, 0, nullptr};
        }
        void* ref = LookupRef(p->handle);
        if (!here) {                                       // they went elsewhere: no statue left behind
            if (ref) QueueDelete(p);
            continue;
        }
        void* look = appearance::BaseFor(s.id);
        if (ref && look && p->base != look) {              // their look arrived after the spawn
            QueueDelete(p);
            continue;
        }
        if (!ref && p->handle) {                           // diagnose why a spawned puppet was lost
            auto table = *reinterpret_cast<u8**>(g_pup.handleTableSlot);
            u8* e = table + (p->handle & 0xFFFFF) * 16;
            uintptr_t ptr = *reinterpret_cast<uintptr_t*>(e + 8);
            u32 cnt = ptr ? *reinterpret_cast<u32*>(ptr - 0x20 + 0x28) : 0;
            Debug("puppet %u lost: handle %x entry flags %x ptr %lx refcnt/idx %x (idx %x)", s.id, p->handle,
                *reinterpret_cast<u32*>(e), ptr, cnt, p->handle & 0xFFFFF);
        }
        if (!ref) {                                        // never spawned, or unloaded with its cell
            if (now - p->lastSpawnMs < 3000) continue;
            if (!p->firstSeenMs) p->firstSeenMs = now;
            if (appearance::Available() && !appearance::BaseFor(s.id) && now - p->firstSeenMs < 30000 &&
                !(s.flags & proto::FLAG_INTRO)) continue;  // wait for their look (not in the intro: the cart does not wait)
            p->lastSpawnMs = now;
            p->x = s.x; p->y = s.y; p->z = s.z; p->rot = s.rotZ;
            if (s.flags & proto::FLAG_DEAD) continue;      // no new puppet for a dead player
            p->handle = Spawn(player, s, &p->base);
            if (p->base != Field<void*>(player, 0x40)) appearance::SetEssential(p->base, true);
            p->dead = false;
            p->moving = false;
            p->drawn = false;
            AnimLock();
            g_animWanted[p - g_puppetsList] = {p->handle, false, 0.0f, false, true, false, false};   // AI off on the game thread
            AnimUnlock();
            continue;
        }
        if ((s.flags & proto::FLAG_DEAD) && !p->dead) {       // their player died: it falls
            p->dead = true;
            AnimLock();
            if (g_toKillCount < MAX_REMOTE) g_toKill[g_toKillCount++] = p->handle;
            AnimUnlock();
            Log("player %u died", s.id);
        }
        if (p->dead) {
            if (!(s.flags & proto::FLAG_DEAD)) QueueDelete(p);   // back (a load, a revive): a new one comes
            continue;
        }
        /* both in the intro (03.10): the cart puts us both on the player's seat — theirs sits beside, a little to its
           left in its own frame (forward = sin / cos of the heading) */
        float sx = s.x, sy = s.y;
        if ((s.flags & proto::FLAG_INTRO) && !quests::IntroDone()) {
            sx -= INTRO_SIDE * cosf(s.rotZ);
            sy += INTRO_SIDE * sinf(s.rotZ);
        }
        float dx = sx - p->x, dy = sy - p->y, dz = s.z - p->z;
        if (dx * dx + dy * dy + dz * dz > 1000.0f * 1000.0f) { p->x = sx; p->y = sy; p->z = s.z; }
        else { p->x += dx * 0.35f; p->y += dy * 0.35f; p->z += dz * 0.35f; }      // smooth toward the latest state
        p->rot = s.rotZ;
        reinterpret_cast<float*>(static_cast<u8*>(ref) + g_res.offRotZ)[0] = p->rot;
        if (!g_setPosition) {                          // other build: the old way (queued SetPosition task)
            const float* cur = reinterpret_cast<const float*>(static_cast<u8*>(ref) + g_res.offPos);
            float ex = p->x - cur[0], ey = p->y - cur[1];
            if (ex * ex + ey * ey > 12.0f * 12.0f || ((s.flags & proto::FLAG_MOVING) && s.speed > 5))
                QueueMove(p->handle, p->x, p->y, p->z);
        }
        bool moving = (s.flags & proto::FLAG_MOVING) && s.speed > 5;
        AnimLock();
        auto& aw = g_animWanted[p - g_puppetsList];
        aw.handle = p->handle;
        if (moving != p->moving) { aw.eventDue = true; p->moving = moving; }
        aw.moving = moving;
        aw.speed = moving ? s.speed : 0.0f;
        aw.tx = sx; aw.ty = sy; aw.tz = s.z; aw.rot = s.rotZ; aw.hasTarget = true;
        bool drawn = (s.flags & proto::FLAG_WEAPON_DRAWN) != 0;
        if (drawn != p->drawn) { aw.drawn = drawn; aw.drawDue = true; p->drawn = drawn; }
        aw.sneak = (s.flags & proto::FLAG_SNEAKING) != 0;
        AnimUnlock();
        if (now - p->lastReportMs >= 5000) {              // where the engine actually has it
            p->lastReportMs = now;
            const float* rp = reinterpret_cast<const float*>(static_cast<u8*>(ref) + g_res.offPos);
            Debug("puppet %u at %.0f %.0f %.0f (target %.0f %.0f %.0f), form %08x", s.id, rp[0], rp[1], rp[2],
                s.x, s.y, s.z, FormId(ref));
        }
    }
}

/* Actor.DrawWeapon native (1.1.177): ldr x8,[x2]; mov x0,x2; mov w1,#1; ldr x2,[x8,#0x578]; br x2 */
constexpr AnimFn DRAW_WEAPON{0xa052e0, {0xF9400048, 0xAA0203E0, 0x52800021, 0xF942BD02}};
uintptr_t g_drawWeapon;
/* Actor.SetDontMove native (vm, stack, actor, bool): no VM use */
constexpr AnimFn DONT_MOVE{0xa02f60, {0xA9BE7BFD, 0xA9014FF4, 0x910003FD, 0x12000061}};
uintptr_t g_dontMove;

/* Puppets' weapon: what their player wants. Their own AI sheathes a drawn weapon outside combat after a few
   seconds; Character::DrawWeaponMagicHands (vtable +0x578, 1.1.177: 0x632d78) is hooked and refuses to put the
   weapon away while the player holds it out (our own calls pass). */
bool wantDrawn[MAX_REMOTE];
void* drawRef[MAX_REMOTE];
volatile bool g_ourDraw;
constexpr AnimFn CHAR_DRAW{0x632d78, {0xD10283FF, 0xA9067BFD, 0x910183FD, 0xF9003BF7}};
HOOK_DEFINE_TRAMPOLINE(CharDrawHook) {
    static void Callback(void* actor, bool draw) {
        if (!draw && !g_ourDraw)
            for (int k = 0; k < MAX_REMOTE; k++)
                if (drawRef[k] == actor && wantDrawn[k]) return;
        Orig(actor, draw);
    }
};

/* Actor::SetPosition(const NiPoint3&, bool updateCharController): the virtual Skyrim Together's ForcePosition
   uses (SE vtable 0xA9; here slot 0xB2 = 0x590 in Character and PlayerCharacter, 1.1.177: 0x649638). Moves the
   reference, its 3D and its character controller in place — no MoveTo, so no fade / reload of the 3D. */
constexpr AnimFn ACTOR_SET_POSITION{0x649638, {0xD10143FF, 0xA9017BFD, 0x910043FD, 0xA9025FF8}};
constexpr u32 VT_SET_POSITION = 0x590;

/* Called ~45 times a second on the game thread: glide toward the latest network position (states come at
   20 Hz), snap when far (teleport, cell change), keep its own z when standing so it rests on the ground. */
void MoveSmooth(void* ref, const AnimWanted& a, u64 now) {
    static u64 last[MAX_REMOTE];
    auto vt = *reinterpret_cast<uintptr_t**>(ref);
    if (vt[VT_SET_POSITION / 8] != g_setPosition) return;             // not an actor of the expected class
    float* cur = reinterpret_cast<float*>(static_cast<u8*>(ref) + g_res.offPos);
    float dx = a.tx - cur[0], dy = a.ty - cur[1], dz = a.tz - cur[2];
    float d2 = dx * dx + dy * dy;
    reinterpret_cast<float*>(static_cast<u8*>(ref) + g_res.offRotZ)[0] = a.rot;
    if (!a.moving && d2 < 20.0f * 20.0f && dz < 80.0f && dz > -80.0f) return;   // standing where it should be
    float pos[3];
    if (d2 > 700.0f * 700.0f || dz > 300.0f || dz < -300.0f) { pos[0] = a.tx; pos[1] = a.ty; pos[2] = a.tz; }
    else {
        float k = a.moving ? 0.25f : 0.15f;
        pos[0] = cur[0] + dx * k; pos[1] = cur[1] + dy * k; pos[2] = cur[2] + dz * k;
    }
    (void)last;
    reinterpret_cast<void (*)(void*, const float*, bool)>(g_setPosition)(ref, pos, true);
}

/* Runs on the engine thread that pumps the VM's functor queue (where the game itself runs
   SetPosition's MoveRefrToPosition): Havok is set up there, so animation-graph calls are safe. */
void ApplyAnimations() {
    AnimWanted todo[MAX_REMOTE];
    AnimLock();
    for (int i = 0; i < MAX_REMOTE; i++) {
        todo[i] = g_animWanted[i];
        g_animWanted[i].eventDue = false;
        g_animWanted[i].aiOffDue = false;
        g_animWanted[i].drawDue = false;
    }
    AnimUnlock();
    static u64 aiOffAt[MAX_REMOTE];
    static u64 lastCheck;
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    for (int k = 0; k < MAX_REMOTE; k++) {
        auto& a = todo[k];
        void* ref = LookupRef(a.handle);
        if (!ref) continue;
        void* carrier = mount::Carrier(ref);                        // riding: the horse follows, the puppet sits on it
        bool core = actors::DrivesPuppet(ref);                    // (08.10) the actor core moves it (actors.cpp)
        if (a.hasTarget && g_setPosition && !core) MoveSmooth(carrier, a, now);
        if (carrier != ref) {
            if (a.eventDue) AnimEvent(carrier, a.moving ? "moveStart" : "moveStop");
            if (a.moving) { AnimFloat(carrier, "Speed", a.speed); AnimFloat(carrier, "SpeedSampled", a.speed); }
        }
        if (aiOffAt[k] && now >= aiOffAt[k]) { aiOffAt[k] = 0; EnableAi(ref, false); }
        if (a.aiOffDue) {
            EnableAi(ref, false);                                   // (kept on, see KEEP_AI)
            if (g_dontMove)                                         // its AI animates but never walks on its own
                reinterpret_cast<void (*)(void*, u32, void*, bool)>(g_dontMove)(nullptr, 0, ref, true);
        }
        if (a.eventDue && !core) AnimEvent(ref, a.moving ? "moveStart" : "moveStop");
        /* (07.10, his: "она приседает, а у меня она не приседает") sneaking held from the state every update, the Skyrim
           Together way: the one-off SneakStart event got lost (puppet not there yet, its own AI standing it up). The
           actor state bit (+0xF8 bit 9) and the graph event, again while they differ (at most twice a second) */
        {
            static u64 sneakAt[MAX_REMOTE];
            bool is = (Field<u32>(ref, 0xF8) >> 9) & 1;
            if (!core && is != a.sneak && now - sneakAt[k] > 500) {
                sneakAt[k] = now;
                u32& st1 = *reinterpret_cast<u32*>(static_cast<u8*>(ref) + 0xF8);
                st1 = a.sneak ? (st1 | (1u << 9)) : (st1 & ~(1u << 9));
                AnimEvent(ref, a.sneak ? "SneakStart" : "SneakStop");
            }
        }
        if (drawRef[k] != ref && !a.drawDue) wantDrawn[k] = false;  // a new puppet in this slot: not the old one's wish
        if (a.drawDue) wantDrawn[k] = a.drawn;
        drawRef[k] = ref;
        if (!a.drawDue && g_drawWeapon && now - lastCheck > 1000) {  // its own AI must not draw / sheathe for them
            u64 st = (u64)Field<u16>(ref, g_res.offActorState) << 32;
            bool drawn = (st & 0xE000000000ull) > 0x4000000000ull;
            static u8 misses[MAX_REMOTE];
            static u64 restUntil[MAX_REMOTE];
            if (drawn == wantDrawn[k]) misses[k] = 0;
            else if (now >= restUntil[k]) {                          // 5 tries a second apart, then 10 s rest
                a.drawDue = true; a.drawn = wantDrawn[k];
                if (++misses[k] >= 5) { misses[k] = 0; restUntil[k] = now + 10000; }
            }
        }
        static u64 lastDraw[MAX_REMOTE];
        if (a.drawDue && now - lastDraw[k] < 600) a.drawDue = false;  // at most ~2 a second; the check below catches up
        if (a.drawDue) lastDraw[k] = now;
        if (a.drawDue && g_drawWeapon) {                           // Actor::DrawWeaponMagicHands(bool)
            EnableAi(ref, true);                                   // the draw animation runs through its AI update
            aiOffAt[k] = now + 2000;
            auto fn = reinterpret_cast<void (*)(void*, bool)>((*reinterpret_cast<uintptr_t**>(ref))[0x578 / 8]);
            g_ourDraw = true;
            fn(ref, a.drawn);
            g_ourDraw = false;
            AnimEvent(ref, a.drawn ? "WeapEquip" : "Unequip");      // the graph does not follow on its own with AI off
            Debug("puppet weapon %s", a.drawn ? "drawn" : "sheathed");
        }
        if (a.moving) {                       // walking is driven by us; standing, its own graph idles naturally
            AnimFloat(ref, "Speed", a.speed);
            AnimFloat(ref, "SpeedSampled", a.speed);
            /* where it walks relative to where it faces (0 forward, 0.25 right, 0.5 back, 0.75 left):
               strafing and walking backwards play their own animations, not a forward walk */
            static float lastX[MAX_REMOTE], lastY[MAX_REMOTE], dirOut[MAX_REMOTE];
            float mx = a.tx - lastX[k], my = a.ty - lastY[k];
            lastX[k] = a.tx; lastY[k] = a.ty;
            if (mx * mx + my * my > 4.0f && mx * mx + my * my < 250000.0f) {
                float d = (atan2f(mx, my) - a.rot) / 6.2831853f;
                d -= floorf(d);
                float diff = d - dirOut[k];                   // smooth, the short way round
                diff -= floorf(diff + 0.5f);
                dirOut[k] += diff * 0.5f;
                dirOut[k] -= floorf(dirOut[k]);
            }
            AnimFloat(ref, "Direction", dirOut[k]);
        }
    }
    if (now - lastCheck > 1000) lastCheck = now;
}

/* The local player's animation graph events worth repeating on the other consoles (Skyrim Together
   replicates the same kind): attacks, power attacks, blocking, bows, jumps, spells, shouts. The generic
   IAnimationGraphManagerHolder::NotifyAnimationGraph (1.1.177: 0x532db4, reached from every actor's
   holder vtable slot 2 through a one-instruction thunk) is hooked and filtered to the player's holder. */
constexpr AnimFn GRAPH_NOTIFY{0x532db4, {0xD100C3FF, 0xA9017BFD, 0x910043FD, 0xA9024FF4}};
void* volatile g_playerHolder;

bool Replicated(const char* e) {
    static const char* const PREFIX[] = {
        "attack", "Attack", "bash", "block", "Block", "bow", "Bow", "jumpStart", "JumpStart", "jumpLand",
        "BeginCast", "MRh_", "MLh_", "Dual", "shout", "Shout", "Voice", "reload", "arrow", "Arrow",
        "Sneak", "sneak", "Sprint", "sprint",
    };
    for (const char* p : PREFIX)
        if (!strncmp(e, p, strlen(p))) return true;
    return false;
}

/* Attacks / blocking / sneaking read from the actor state (CommonLib ActorState: +0xF8 state1 with sneaking bit 9
   and meleeAttackState bits 28..31, +0xFC state2 with wantBlocking bit 8). The graph events do not reach the
   holder hook in first person, the state does in any view; a graph event sent in the last 0.6 s wins. */
volatile u64 g_lastGraphAttack;
void StateEvents(void* player, u64 now, proto::State* s) {
    static u32 lastAttack, lastBlock, lastSneak;
    u32 s1 = Field<u32>(player, 0xF8), s2 = Field<u32>(player, 0xFC);
    u32 attack = s1 >> 28, block = (s2 >> 8) & 1, sneak = (s1 >> 9) & 1;
    static u32 ls1, ls2; static int logged;
    if ((s1 != ls1 || s2 != ls2) && logged < 60) { logged++; Debug("state %08x %08x", s1, s2); }
    ls1 = s1; ls2 = s2;
    if (sneak) s->flags |= proto::FLAG_SNEAKING;
    if (attack && !lastAttack && now - g_lastGraphAttack > 600)
        PushEvent(attack == 6 ? "bashStart" : attack >= 8 ? "bowAttackStart" : "attackStart");
    if (block != lastBlock) PushEvent(block ? "blockStart" : "blockStop");
    if (sneak != lastSneak) PushEvent(sneak ? "SneakStart" : "SneakStop");
    lastAttack = attack; lastBlock = block; lastSneak = sneak;
}

HOOK_DEFINE_TRAMPOLINE(GraphNotify) {
    static bool Callback(void* holder, FixedString* ev) {
        if (holder && holder == g_playerHolder && ev && ev->data && (strstr(ev->data, "hout") || strstr(ev->data, "rrow") || strstr(ev->data, "Spell"))) {
            static int n;
            if (n++ < 60) Debug("player graph event: %s", ev->data);           // names used by magic.cpp
        }
        if (holder && holder == g_playerHolder && ev && ev->data && Replicated(ev->data)) {
            /* block and sneak come from the state (StateEvents): not twice */
            if (strncmp(ev->data, "block", 5) && strncmp(ev->data, "Block", 5) && strncasecmp(ev->data, "sneak", 5)) {
                if (!strncmp(ev->data, "attack", 6) || !strncmp(ev->data, "bash", 4))
                    g_lastGraphAttack = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
                PushEvent(ev->data);
            }
        }
        return Orig(holder, ev);
    }
};

/* Other players' events on their puppets (game thread). */
void ApplyRemoteEvents() {
    u16 id; char name[40];
    for (int i = 0; i < 16 && TakeEvent(&id, name, sizeof name); i++) {
        if (!strncmp(name, "coopweather ", 12)) {                          // weather.cpp: the host's sky
            weather::Received(strtoul(name + 12, nullptr, 16));
            continue;
        }
        if (!strncmp(name, "coopheal ", 9)) {                              // magic.cpp: a friend's heal
            char* end;
            u16 to = (u16)strtoul(name + 9, &end, 10);
            if (to == MyId()) death::Healed(id, strtof(end, nullptr));
            continue;
        }
        if (!strncmp(name, "cooprevive ", 11)) {                           // death.cpp: a friend got somebody up
            if ((u16)strtoul(name + 11, nullptr, 10) == MyId()) death::RevivedBy(id);
            continue;
        }
        if (!strncmp(name, "coophorse ", 10)) {                            // mount.cpp: riding or not
            mount::Received(id, strtoul(name + 10, nullptr, 16));
            continue;
        }
        void* ref = PuppetRef(id);
        if (!ref) {
            if (!strncmp(name, "coop", 4)) Debug("event %s from %u: no puppet here", name, id);   // (07.10) lost casts
            continue;
        }
        if (!strncmp(name, "coopcast ", 9)) {                               // magic.cpp: a released spell
            char* end;
            u32 hand = strtoul(name + 9, &end, 10);
            magic::CastOn(ref, hand, strtoul(end, nullptr, 16));
            continue;
        }
        if (!strncmp(name, "cooparrow ", 10)) {                             // magic.cpp: an arrow shot
            char* end;
            u32 bow = strtoul(name + 10, &end, 16);
            magic::FireOn(ref, bow, strtoul(end, nullptr, 16));
            continue;
        }
        AnimEvent(ref, name);
        if (!strncmp(name, "attack", 6) || !strncmp(name, "bash", 4)) Debug("puppet %u: %s", id, name);
    }
}

/* Shared world: deaths. Actor::KillImpl (1.1.177: 0x60a154, actor, killer, damage, sendEvent, ragdoll;
   reached from the Kill native and from combat) is hooked; an actor from the plugins (FormID not 0xFF...,
   the same on every console) that dies here is announced, and the others kill their copy once it is loaded. */
constexpr AnimFn KILL_IMPL{0x60a154, {0xFC1C0FE8, 0xA9017BFD, 0x910043FD, 0xA90257F6}};
volatile bool g_remoteKill;
u32 g_announced[128];
int g_announcedCount;

HOOK_DEFINE_TRAMPOLINE(KillImplHook) {
    static void Callback(void* actor, void* killer, float damage, bool sendEvent, bool ragdoll) {
        Orig(actor, killer, damage, sendEvent, ragdoll);
        if (g_remoteKill || !actor || actor == Player() || Status() != NetStatus::Connected) return;
        if (!quests::IntroDone()) return;                        // (07.10) deaths in our intro (Helgen) stay ours
        world::DragonDied(actor);                                 // (any dragon, runtime ones too: the soul is the party's)
        u32 id = FormId(actor);
        if (!id || (id >> 24) == 0xFF) return;                 // made at runtime: differs between consoles
        for (int i = 0; i < g_announcedCount; i++) if (g_announced[i] == id) return;
        g_announced[g_announcedCount++ % 128] = id;
        if (g_announcedCount > 128) g_announcedCount = 128;
        /* unique actors stay dead for good; the others come back by the server's timer (ACBS flags at TESNPC+0x30,
           0x20 = Unique: the player's base reads 0x30 there) */
        void* base = Field<void*>(actor, 0x40);
        u32 acbs = base && Readable(static_cast<u8*>(base) + 0x30, 4) ? *reinterpret_cast<u32*>(static_cast<u8*>(base) + 0x30) : 0x20;
        PushKill(id, (acbs & 0x20) ? 0 : proto::KILL_RESPAWNS);
        Debug("killed here: %08x (%s)", id, (acbs & 0x20) ? "unique" : "respawns");
    }
};

u32 g_pendingKills[128];
u16 g_pendingKillFlags[128];
int g_pendingKillCount;
constexpr AnimFn RESURRECT{0xa02670, {0xD10143FF, 0xA9017BFD, 0x910043FD, 0xF90013F7}};   // Actor.Resurrect (vm, stack, actor)
uintptr_t g_resurrect;

void ApplyRemoteKills() {
    u32 id; u16 fl;
    while (TakeKill(&id, &fl) && g_pendingKillCount < 128) {
        int at = -1;
        for (int i = 0; i < g_pendingKillCount; i++) if (g_pendingKills[i] == id) at = i;
        if (at < 0) at = g_pendingKillCount++;
        g_pendingKills[at] = id;
        g_pendingKillFlags[at] = fl;                         // the newest word wins (killed, then revived)
    }
    static u64 last;
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    if (now - last < 1000) return;
    last = now;
    for (int i = 0; i < g_pendingKillCount; i++) {
        auto actor = static_cast<u8*>(equip::Lookup(g_pendingKills[i]));
        if (!actor || actor[0x1A] != 0x3E) continue;             // not an actor reference (yet)
        if (!*reinterpret_cast<void**>(actor + 0x68)) continue;  // not loaded here: later, when it is
        auto vt = *reinterpret_cast<uintptr_t**>(actor);
        bool dead = reinterpret_cast<bool (*)(void*, bool)>(vt[g_res.vtIsDead / 8])(actor, true);
        if (g_pendingKillFlags[i] & proto::KILL_REVIVE) {      /* the server brought it back */
            if (dead && g_resurrect) {
                reinterpret_cast<void (*)(void*, u32, void*)>(g_resurrect)(FakeVmPtr(), 0, actor);
                Debug("revived by the server: %08x", g_pendingKills[i]);
            }
            for (int a = 0; a < g_announcedCount; a++) if (g_announced[a] == g_pendingKills[i]) g_announced[a] = 0;
        } else if (!dead) {
            g_remoteKill = true;
            KillImplHook::Orig(actor, nullptr, 0.0f, true, false);
            g_remoteKill = false;
            Debug("killed by another player: %08x", g_pendingKills[i]);
        }
        g_pendingKills[i--] = g_pendingKills[--g_pendingKillCount];
    }
}

/* Time of day: GameHour (global 0x38; TESGlobal: type char +0x30, float value +0x34) goes out every
   5 s; the host's clock (the server relays only the first player's) is taken when 10+ game minutes off. */
/* (08.10, the Skyrim Together way — CalendarService) the server keeps the campaign's game clock: hour, time scale and
   date. Here it is followed every update (the game's own clock ticks, we put it right): on a new connection the hour
   glides from ours to the server's in 5 s, then it is the server's. Ours goes to the server every 10 s — taken only
   while the campaign has no clock yet (the first player's). Sleeping / waiting does not move the shared clock.
   Globals (GLOB 'f', value at +0x34): GameYear 0x35, GameMonth 0x36, GameDay 0x37, GameHour 0x38, TimeScale 0x3A */
float* GlobalF(u32 id) {
    auto g = static_cast<u8*>(equip::Lookup(id));
    if (!g || g[0x1A] != 0x09) return nullptr;            // (the value is a float whatever the display type: GameDay is 's')
    return reinterpret_cast<float*>(g + 0x34);
}

void SyncTime() {
    float *hour = GlobalF(0x38), *scale = GlobalF(0x3A), *day = GlobalF(0x37), *month = GlobalF(0x36), *year = GlobalF(0x35);
    if (!hour || !scale || !day || !month || !year || !(*hour >= 0 && *hour < 24.5f)) return;
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    static u64 lastSent;
    if (now - lastSent >= 10000) {
        lastSent = now;
        proto::ClockMsg m{*hour, *scale, *day, *month, *year};
        PushActorPacket(proto::CLOCK, &m, sizeof m);
    }
    proto::ClockMsg c; u64 at; u32 seq;
    if (!ServerClock(&c, &at, &seq)) return;
    static u32 lastSeq; static u64 fadeStart; static float fadeFrom;
    if (seq < lastSeq || !lastSeq) { fadeStart = now; fadeFrom = *hour; Log("время: часы сервера %.2f ч (масштаб %.0f)", c.hour, c.scale); }
    lastSeq = seq;
    float target = c.hour + (float)(now - at) / 1000.0f * c.scale / 3600.0f;
    float dayAdd = 0;
    while (target >= 24.0f) { target -= 24.0f; dayAdd += 1; }
    float h = target;
    if (now - fadeStart < 5000) {                      // the glide: forward the short way round
        float d = target - fadeFrom;
        if (d < 0) d += 24.0f;
        if (d > 12.0f) d -= 24.0f;
        h = fadeFrom + d * (float)(now - fadeStart) / 5000.0f;
        if (h < 0) h += 24.0f;
        if (h >= 24.0f) h -= 24.0f;
    }
    *hour = h;
    *scale = c.scale > 0 ? c.scale : 0.0001f;          // fixed mode: (almost) standing
    *day = c.day + dayAdd;
    *month = c.month;
    *year = c.year;
}

/* Our look goes out once in the world (and again if it changes); theirs is built here too. */
/* ---- world level (03.10, the user: one level for the whole world = the highest player online) ----
   TESActorBaseData::GetLevel (1.1.177 0xc6120: level u16 at +0x10, PCLevelMult flag bit 7 at +8) is what leveled
   lists, encounter zones and PCLevelMult NPCs ask. For the player's own data it answers max(own level, the highest
   level among the players in the room); the stored level (and the save) stays the player's own. */
constexpr AnimFn ACTOR_DATA_LEVEL{0xc6120, {0x79402009, 0x39402008, 0x37380068, 0x2A0903E8}};
volatile u16 g_worldLevel;
void* volatile g_playerData;
bool g_levelRaw;                                      // game thread: our own real level is being read (plates.cpp)
HOOK_DEFINE_TRAMPOLINE(LevelHook) {
    static u16 Callback(void* data) {
        u16 v = Orig(data);
        if (!g_levelRaw && data && data == g_playerData && g_worldLevel > v) return g_worldLevel;
        return v;
    }
};

}  // namespace
/* our own level as the character has it — not the world level the game sees while others are stronger (04.10: the
   nameplate sent the raised level, which then came back as "the strongest player" — level 37 at the Helgen cart) */
u16 RealLevel(uintptr_t getLevel, void* player) {
    g_levelRaw = true;
    u16 v = (u16)reinterpret_cast<u32 (*)(void*, u32, void*)>(getLevel)(FakeVmPtr(), 0, player);
    g_levelRaw = false;
    return v;
}
namespace {

void PumpWorldLevel(void* player) {
    static u64 at;
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    if (now - at < 2000) return;
    at = now;
    g_playerData = static_cast<u8*>(Field<void*>(player, 0x40)) + 0x28;
    u16 top = 0;
    if (Status() == NetStatus::Connected) {
        for (u16 id = 1; id < 256; id++) {              /* everyone the server told us stats of (not only those in sight) */
            proto::StatsMsg st;
            if (id != MyId() && StatsOf(id, &st) && st.level > top && st.level < 1000) top = st.level;
        }
    }
    if (top != g_worldLevel) {
        g_worldLevel = top;
        if (top) {
            char msg[96];
            snprintf(msg, sizeof msg, "Уровень мира: %u (по самому сильному игроку)", top);
            Notify(msg);
        }
        Log("уровень мира: %u", top);
    }
}

/* ---- menus without pause in the online game (03.10; Skyrim Together's UI allow list) ----
   Each menu's constructor stores its IMenu flags (+0x1C) from one MOVZ: bit 0 kPausesGame. While we are in a room the
   bit is cleared in those MOVZs (the world keeps running behind the inventory, like any MMO); back when we leave.
   Map / Journal / Book / crafting keep their pause (ST: the map breaks first person, the journal's saving crashes). */
struct MenuFlagSite { u32 off; u16 flags; const char* name; };
constexpr MenuFlagSite MENU_FLAG_SITES[] = {
    {0x9060a8, 0xa481, "InventoryMenu"}, {0x8ce500, 0xa489, "ContainerMenu"}, {0x916fdc, 0xa489, "MagicMenu"},
    {0x957448, 0xcc01, "TweenMenu"},     {0x8eadb0, 0xa401, "FavoritesMenu"}, {0x8c2af8, 0xa489, "BarterMenu"},
    {0x8edb80, 0xa489, "GiftMenu"},
};
bool g_menusUnpaused;

u32 MovzW8(u16 imm) { return 0x52800008u | ((u32)imm << 5); }

void SetMenusPause(bool pause) {
    if (g_menusUnpaused == !pause) return;
    int done = 0;
    for (const auto& m : MENU_FLAG_SITES) {
        auto at = reinterpret_cast<const u32*>(exl::util::modules::GetTargetOffset(m.off));
        u32 want = MovzW8(pause ? (u16)(m.flags & ~1) : m.flags), set = MovzW8(pause ? m.flags : (u16)(m.flags & ~1));
        if (*at != want) continue;                     /* not the instruction we know: leave it */
        exl::patch::CodePatcher(m.off).WriteInst(set);
        done++;
    }
    g_menusUnpaused = !pause;
    Log("меню %s паузы (%d из %d)", pause ? "снова с" : "без", done, (int)(sizeof MENU_FLAG_SITES / sizeof MENU_FLAG_SITES[0]));
}

/* (08.10) the first pumps after (re)connecting: each step logged (a hang after reconnecting) */
int g_trace;
#define TRACE(n) do { g_gameStep = n; if (g_trace > 0) Log("трасса: %s", n); } while (0)

void PumpAppearance() {
    void* player = Player();
    if (!player || !Field<void*>(player, g_res.offCell)) return;
#ifndef SSW_OFFLINE
    SetMenusPause(Status() != NetStatus::Connected);
    if (quests::IntroDone()) PumpWorldLevel(player);   // (07.10, his wife's intro crashed in one place) the intro is ours alone
#endif
#ifdef SSW_OFFLINE
    /* the offline build (SWITCH_СБОРКА): the game mechanics only, the same code the online mod runs */
    scrolls::Pump();
    glow::Pump();
    return;
#endif
    {
        static bool was;
        bool is = Status() == NetStatus::Connected;
        if (g_trace > 0) g_trace--;
        (void)is; (void)was;                             // (the step trace after connecting: off; g_gameStep stays)
        was = is;
    }
    const void* data; u32 size, flags;
    if (appearance::Collect(&data, &size, &flags)) PublishAppearance(flags, data, size);
    appearance::Pump();
    g_playerHolder = static_cast<u8*>(player) + 0x38;
    if (quests::IntroDone()) SyncTime();                // (07.10) no host clock inside our intro
    {
        static bool pluginsSent;
        static char list[3000];
        if (!pluginsSent && PluginList(list, sizeof list)) { pluginsSent = true; PublishPlugins(list); }
    }
    TRACE("if (quests::IntroDone()) ApplyRemoteKills()"); if (quests::IntroDone()) ApplyRemoteKills();         // (07.10) the others' kills wait until we are out of Helgen
    TRACE("ApplyRemoteEvents()"); ApplyRemoteEvents();
    TRACE("actions::Pump()"); actions::Pump();
    TRACE("trade::Pump()"); trade::Pump();
    TRACE("pvp::Pump()"); pvp::Pump();
    TRACE("followers::Pump()"); followers::Pump();
    TRACE("quests::Pump()"); quests::Pump();
    npcsync::PumpTest(); TRACE("actors::Pump()"); actors::Pump();                                  // (08.10) the actor core replaces npcsync / npcpos
    TRACE("avatar::Pump()"); avatar::Pump();
    TRACE("weather::Pump()"); weather::Pump();
    TRACE("mount::Pump()"); mount::Pump();
    TRACE("plates::Pump()"); plates::Pump();
    TRACE("scrolls::Pump()"); scrolls::Pump();
    TRACE("glow::Pump()"); glow::Pump();
    TRACE("chest::Pump()"); chest::Pump();
    TRACE("magic::Pump()"); magic::Pump();
    TRACE("world::Pump()"); world::Pump();
    TRACE("death::Pump()"); death::Pump();
    TRACE("party::Pump()"); party::Pump();
    TRACE("items::Pump()"); items::Pump();
    TRACE("PumpTeleport()"); PumpTeleport();
    TRACE("equip");
    const equip::Item* items; int count;
    if (equip::Collect(&items, &count)) PublishEquipment(items, count, sizeof(equip::Item));
    TRACE("equip::Pump()"); equip::Pump();
}

/* Debug.MessageBox -> CreateMessage (1.1.177), used by the co-op menu. */
constexpr AnimFn CREATE_MESSAGE{0x92a390, {0xD10603FF, 0xA9127BFD, 0x910483FD, 0xA9136FFC}};

#ifndef SSW_OFFLINE
/* (04.10) the online campaign is one world on the server, begun with a new game: its saves carry the server's
   campaign number in the global SSW_CampaignId (SwitchPatch 0x12E0). A game begun online (in its intro) gets the
   number; a save without it (or with another one) is a personal save — nothing syncs with it (client.cpp). */
void PumpCampaign() {
    static int idx = -2;
    void* player = Player();
    void* cell = player ? Field<void*>(player, g_res.offCell) : nullptr;
    if (!player || !cell) { SetCampaignState(-1); return; }          // the title screen
    if (!Field<void*>(player, 0x68)) { SetCampaignState(2); return; }   // loading
    if (idx == -2) idx = PluginIndex("SwitchPatch.esp");
    auto g = idx >= 0 ? static_cast<u8*>(equip::Lookup(((u32)idx << 24) | 0x12E0)) : nullptr;
    u32 id = CampaignId();
    if (!g || g[0x1A] != 0x09) { SetCampaignState(0); return; }        // (GLOB) no patch: never the campaign
    if (!id) { SetCampaignState(2); return; }                         // the server has not said its number yet
    /* (the number stays known while the link reconnects: the save is still the campaign's) */
    float& v = *reinterpret_cast<float*>(g + 0x34);
    if (v == 0.0f && !quests::IntroDone() && LinkStatus() == NetStatus::Connected) {
        v = (float)id;                                               // a new game begun online: this campaign's
        Log("кампания %u: новая игра — персонаж кампании", id);
    }
    SetCampaignState((u32)v == id ? 1 : 0);
}
#endif

#ifndef SSW_OFFLINE
/* (04.10) the test tools' picture of the game (playtest/play.py), 5 times a second, as text: so a test player can
   react to the game without a screenshot per step. Line 1: "P x y z heading cell world hp mp sp menu dead";
   then one line per actor near: "A formId dist bearing hp dead name" (bearing: degrees from where we look, + right). */
void PumpTelemetry() {
    static u64 at;
    static uintptr_t pct, inMenu;
    struct FS { const char* data; };
    static FS names[3];
    static bool made;
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    if (now - at < 200 || LinkStatus() != NetStatus::Connected) return;
    at = now;
    void* player = Player();
    if (!player || !Field<void*>(player, g_res.offCell) || !Field<void*>(player, 0x68)) return;
    if (!made) {
        made = true;
        static const u32 PCT_SIG[4] = {0xD10783FF, 0xA91A7BFD, 0x910683FD, 0xF900DBFC};
        static const u32 MENU_SIG[4] = {0xF000B6C8, 0xF000B6C9, 0xF9442908, 0xF9449D29};
        static const u32 FS_SIG[4] = {0xA9BE7BFD, 0xF9000BF3, 0x910003FD, 0xF900001F};
        pct = Checked177(0x9fe2f0, PCT_SIG);
        inMenu = Checked177(0xa77404, MENU_SIG);
        uintptr_t ctor = Checked177(0xae6da8, FS_SIG);
        const char* n[3] = {"Health", "Magicka", "Stamina"};
        for (int i = 0; ctor && i < 3; i++) reinterpret_cast<void (*)(FS*, const char*)>(ctor)(&names[i], n[i]);
        if (!ctor) pct = 0;
    }
    auto av = [&](void* a, int i) { return pct ? reinterpret_cast<float (*)(void*, u32, void*, FS*)>(pct)(FakeVmPtr(), 0, a, &names[i]) : -1.0f; };
    auto dead = [&](void* a) {
        auto vt = *reinterpret_cast<uintptr_t**>(a);
        return reinterpret_cast<bool (*)(void*, bool)>(vt[g_res.vtIsDead / 8])(a, true);
    };
    static char out[1400];
    u32 cell = 0, world = 0; float pos[3] = {};
    WhereAmI(&cell, &world, pos);
    float rot = Field<float>(player, g_res.offRotZ);
    bool menu = inMenu && reinterpret_cast<bool (*)(void*, u32)>(inMenu)(FakeVmPtr(), 0);
    int o = snprintf(out, sizeof out, "P %.0f %.0f %.0f %.0f %08x %08x %.0f %.0f %.0f %d %d\n", pos[0], pos[1], pos[2],
                     rot * 57.29578f, cell, world, av(player, 0) * 100, av(player, 1) * 100, av(player, 2) * 100, menu ? 1 : 0,
                     dead(player) ? 1 : 0);
    void* actors[128];
    int na = HighActors(actors, 128);
    struct Near { void* a; float d; };
    Near near[24]; int nn = 0;
    for (int i = 0; i < na; i++) {
        void* a = actors[i];
        if (!a || a == player) continue;
        const float* q = RefPos(a);
        float dx = q[0] - pos[0], dy = q[1] - pos[1], d = sqrtf(dx * dx + dy * dy);
        if (d > 6000.0f) continue;
        if (nn < 24) near[nn++] = Near{a, d};
        else {                                              // keep the 24 nearest
            int far = 0;
            for (int k = 1; k < 24; k++) if (near[k].d > near[far].d) far = k;
            if (d < near[far].d) near[far] = Near{a, d};
        }
    }
    for (int i = 0; i < nn && o < (int)sizeof out - 100; i++) {
        void* a = near[i].a;
        const float* q = RefPos(a);
        float bearing = atan2f(q[0] - pos[0], q[1] - pos[1]) * 57.29578f - rot * 57.29578f;
        while (bearing > 180.0f) bearing -= 360.0f;
        while (bearing < -180.0f) bearing += 360.0f;
        auto base = Field<u8*>(a, 0x40);
        const char* name = base && Readable(base + 0xD0, 8) ? *reinterpret_cast<const char**>(base + 0xD0) : nullptr;
        if (name && !Readable(name, 1)) name = nullptr;
        o += snprintf(out + o, sizeof out - o, "A %08x %.0f %.0f %.0f %d %.40s\n", Field<u32>(a, 0x14), near[i].d, bearing,
                      av(a, 0) * 100, dead(a) ? 1 : 0, name && *name ? name : "?");
    }
    PushTelemetry(out, (u32)o);
    static bool said;
    if (!said) { said = true; Log("телеметрия: %d байт, актёров %d", o, nn); }
}
#endif

/* SkyrimVM functor-queue pump (1.1.177: 0x9c3978, called every update). */
constexpr AnimFn VM_PUMP{0x9c3978, {0xA9BC7BFD, 0xF9000BF7, 0x910003FD, 0xA90257F6}};
volatile u32 g_pumpCalls;
HOOK_DEFINE_TRAMPOLINE(VmPump) {
    static void Callback(void* vm) {
        Orig(vm);
        __atomic_add_fetch(&g_pumpCalls, 1u, __ATOMIC_RELAXED);
#ifndef SSW_OFFLINE
        TRACE("vm: campaign"); PumpCampaign();          // first: whether this game syncs at all
        TRACE("vm: telemetry"); PumpTelemetry();
        TRACE("vm: delete puppets"); DeletePuppets();
        TRACE("vm: kill puppets"); KillPuppets();
        TRACE("vm: animations"); if (g_anim.ok) ApplyAnimations();
        TRACE("vm: menu"); menu::PumpUI();
#endif
#if !defined(SSW_OFFLINE) || defined(SSW_TESTCTL)
        console::PumpBuiltins();
#endif
        TRACE("vm: appearance"); PumpAppearance();
        TRACE("vm: booksort"); booksort::Pump();
        TRACE("vm: test channel");                                // (06.10) LotD's KID keywords on the books, once
        updates::PumpTestChannel();                      // (06.10) Eden test commands from a file (no server)
    }
};

/* ------------------------------------------------------------------ per-frame work */


float g_lastX, g_lastY, g_lastZ;
u64 g_lastTick;
u32 g_tick;
NetStatus g_shownStatus = NetStatus::Idle;
u16 g_known[MAX_REMOTE];
int g_knownCount;
u64 g_lastDistanceMsg;

u64 NowMs() { return nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds(); }

void ReportStatus() {
    NetStatus st = Status();
    if (st == g_shownStatus || st == NetStatus::Connecting) return;
    char msg[128];
    if (st == NetStatus::Connected) {
        snprintf(msg, sizeof msg, "Кооп: подключено — %s", ServerName());
        Notify(msg);
    } else if (g_shownStatus == NetStatus::Connected) Notify("Кооп: связь с сервером потеряна");
    else if (st == NetStatus::NoNetwork) Notify("Кооп: нет сети");
    else if (st == NetStatus::Searching && g_shownStatus == NetStatus::Idle)
        Notify("Кооп: ищу комнату в сети… (меню: зажми оба стика)");
    g_shownStatus = st;
}

void ReportRemotes(void* player) {
    RemotePlayer rs[MAX_REMOTE];
    int n = SnapshotRemotes(rs, MAX_REMOTE);
    char msg[96];
    for (int i = 0; i < n; i++) {                      // joined
        bool known = false;
        for (int k = 0; k < g_knownCount; k++) known |= g_known[k] == rs[i].state.id;
        if (!known) {
            char name[32];
            if (!PlayerName(rs[i].state.id, name, sizeof name)) snprintf(name, sizeof name, "Игрок %u", rs[i].state.id);
            snprintf(msg, sizeof msg, "Кооп: %s в игре", name);
            Notify(msg);
        }
    }
    for (int k = 0; k < g_knownCount; k++) {           // left
        bool still = false;
        for (int i = 0; i < n; i++) still |= g_known[k] == rs[i].state.id;
        if (!still) {
            char name[32];
            if (!PlayerName(g_known[k], name, sizeof name)) snprintf(name, sizeof name, "Игрок %u", g_known[k]);
            snprintf(msg, sizeof msg, "Кооп: %s вышел", name);
            Notify(msg);
        }
    }
    g_knownCount = n;
    for (int i = 0; i < n; i++) g_known[i] = rs[i].state.id;

    char chat[160];
    if (TakeMessage(chat, sizeof chat)) Notify(chat);          // one per call: the HUD queues them anyway
    return;
    u64 now = NowMs();
    if (n == 0 || now - g_lastDistanceMsg < 15000) return;
    g_lastDistanceMsg = now;
    const float* p = reinterpret_cast<const float*>(static_cast<u8*>(player) + g_res.offPos);
    u32 cell = FormId(Field<void*>(player, g_res.offCell));
    for (int i = 0; i < n; i++) {
        const auto& s = rs[i].state;
        if (s.cell != cell && (s.worldspace == 0 || s.worldspace != FormId(reinterpret_cast<WorldFn>(g_res.getWorldspace)(player)))) {
            snprintf(msg, sizeof msg, "Игрок %u: в другом месте", s.id);
        } else {
            float dx = s.x - p[0], dy = s.y - p[1], dz = s.z - p[2];
            snprintf(msg, sizeof msg, "Игрок %u: %d м", s.id, (int)(sqrtf(dx * dx + dy * dy + dz * dz) * 0.01428f));  // 70 units ~ 1 m
        }
        Notify(msg);
    }
}

void OnFrame() {
    void* player = Player();
    if (!player) return;          // main menu / loading: no HUD to show messages on yet
    ReportStatus();
    u64 now = NowMs();
    const float* p = reinterpret_cast<const float*>(static_cast<u8*>(player) + g_res.offPos);
    proto::State s{};
    s.x = p[0]; s.y = p[1]; s.z = p[2];
    s.rotZ = Field<float>(player, g_res.offRotZ);
    s.cell = FormId(Field<void*>(player, g_res.offCell));
    s.worldspace = FormId(reinterpret_cast<WorldFn>(g_res.getWorldspace)(player));
    float dt = g_lastTick ? (now - g_lastTick) / 1000.0f : 0;
    float dx = s.x - g_lastX, dy = s.y - g_lastY;
    s.speed = dt > 0 ? sqrtf(dx * dx + dy * dy) / dt : 0;
    if (s.speed > 5) s.flags |= proto::FLAG_MOVING;
    u64 st = (u64)Field<u16>(player, g_res.offActorState) << 32;
    if ((st & 0xE000000000ull) > 0x4000000000ull) s.flags |= proto::FLAG_WEAPON_DRAWN;
    StateEvents(player, now, &s);
    auto vt = Field<uintptr_t*>(player, 0);
    if (reinterpret_cast<IsDeadFn>(vt[g_res.vtIsDead / 8])(player, true)) s.flags |= proto::FLAG_DEAD;
    if (!quests::IntroDone()) s.flags |= proto::FLAG_INTRO;
    if (death::IsDown()) s.flags |= proto::FLAG_DOWN;
    s.tick = ++g_tick;
    g_lastX = s.x; g_lastY = s.y; g_lastZ = s.z; g_lastTick = now;
    PublishLocalState(s);
    ReportRemotes(player);
    if (g_puppets) {
        RemotePlayer rs[MAX_REMOTE];
        int n = SnapshotRemotes(rs, MAX_REMOTE);
        UpdatePuppets(player, rs, n, now);     // spawn + move (safe on this thread, as before)
        // animation graph calls are applied from the VM pump hook (ApplyAnimations)
    }
    static u64 lastStats;
    if (now - lastStats > 10000) { lastStats = now; Debug("vm pump calls: %u", g_pumpCalls); }
}

volatile u64 g_nextFrameMs;
volatile u32 g_busy;

/* Both sticks pressed (L3 + R3) for 1.5 s opens the address keyboard. NpadButtonSet is the
   u64 at +8 in all three Npad*State structs; StickL = bit 4, StickR = bit 5. */
constexpr u64 COMBO = (1ull << 4) | (1ull << 5);
volatile u64 g_comboSince;
volatile bool g_comboFired;

/* Npad*State: u64 sampling, u64 buttons (+8), s32 lx, ly (+0x10), s32 rx, ry (+0x18). */
volatile u64 g_ownButtons, g_ownUntil;          // the module's own presses (Press): closing a menu and the like

void ApplyInjected(void* state) {
    if (g_ownUntil && NowMs() < g_ownUntil) *reinterpret_cast<u64*>(static_cast<u8*>(state) + 8) |= g_ownButtons;
    InjectedInput in = Injected();
    if (!in.untilMs || NowMs() > in.untilMs) return;
    auto p = static_cast<u8*>(state);
    *reinterpret_cast<u64*>(p + 8) |= in.buttons;
    auto st = reinterpret_cast<s32*>(p + 0x10);
    if (in.lx || in.ly) { st[0] = in.lx; st[1] = in.ly; }
    if (in.rx || in.ry) { st[2] = in.rx; st[3] = in.ry; }
}

void CheckCombo(const void* state) {
    u64 buttons = *reinterpret_cast<const u64*>(static_cast<const u8*>(state) + 8);
    if ((buttons & COMBO) != COMBO) { g_comboSince = 0; g_comboFired = false; return; }
    u64 now = NowMs();
    if (!g_comboSince) g_comboSince = now;
    if (!g_comboFired && now - g_comboSince >= 1500) {
        g_comboFired = true;
        Log("L3+R3: меню кооператива");          // (07.10) his console: "L3 + R3 не открылось" — seen in the server log
        menu::Open();
    }
}

void MaybeFrame() {
    if (!g_ready) return;
    u64 now = NowMs();
    if (now < g_nextFrameMs) return;
    if (__atomic_exchange_n(&g_busy, 1u, __ATOMIC_ACQUIRE)) return;   // controller polled from several threads
    g_nextFrameMs = now + 33;
    OnFrame();
    __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE);
}

#if defined(SSW_OFFLINE) && !defined(SSW_TESTCTL)
/* the offline build (03.10): only the module's own presses (scrolls.cpp shuts the scroll's page) — no network, no menu */
#define POLL_WORK(out) do { if (g_ownUntil && NowMs() < g_ownUntil) *reinterpret_cast<u64*>(reinterpret_cast<u8*>(out) + 8) |= g_ownButtons; } while (0)
#else
#define POLL_WORK(out) do { ApplyInjected(out); CheckCombo(out); MaybeFrame(); } while (0)
#endif
HOOK_DEFINE_TRAMPOLINE(PollFullKey) {
    static void Callback(nn::hid::NpadFullKeyState* out, const u32& id) { Orig(out, id); POLL_WORK(out); }
};
HOOK_DEFINE_TRAMPOLINE(PollHandheld) {
    static void Callback(nn::hid::NpadHandheldState* out, const u32& id) { Orig(out, id); POLL_WORK(out); }
};
HOOK_DEFINE_TRAMPOLINE(PollJoyDual) {
    static void Callback(nn::hid::NpadJoyDualState* out, const u32& id) { Orig(out, id); POLL_WORK(out); }
};

}  // namespace

constexpr AnimFn CENTER_ON_CELL_FN{0x6fa428, {0xD10143FF, 0xA9027BFD, 0x910083FD, 0xA90357F6}};
uintptr_t g_coc;                  // resolved in Resolve(): no function-local statics (their guard froze the game)

bool Resolve() {
    enum { GET_PLAYER, NOTIFICATION, GET_POS_Y, GET_ANGLE_Z, GET_CELL, GET_FORM_ID, GET_WORLD, WEAPON_DRAWN, IS_DEAD,
           PLACE_AT_ME, SET_POSITION, DELETE, COUNT };
    Wanted w[COUNT] = {
        {"GetPlayer", {}, 0, 0}, {"Notification", {}, 0, 0}, {"GetPositionY", {}, 0, 0}, {"GetAngleZ", {}, 0, 0},
        {"GetParentCell", {}, 0, 0}, {"GetFormID", {}, 0, 0}, {"GetWorldSpace", {}, 0, 0},
        {"IsWeaponDrawn", {}, 0, 0}, {"IsDead", {}, 0, 0},
        {"PlaceAtMe", {}, 0, 0}, {"SetPosition", {}, 0, 0},      // puppets only: optional
        {"Delete", {}, 0, 0},
    };
    FindNatives(w, COUNT);
    for (int i = 0; i < PLACE_AT_ME; i++)
        if (!w[i].fn) { Log("native not found: %s", w[i].name); return false; }

    Resolved r{};
    u32 posY = 0, vt = 0;
    bool ok = DecodeGetPlayer(w[GET_PLAYER].fn, &r.playerSlot)
        && TailCall(w[NOTIFICATION].fn, &r.notify)
        && FieldOffset(w[GET_POS_Y].fn, IsLdrS, 4, &posY)
        && FieldOffset(w[GET_ANGLE_Z].fn, IsLdrS, 4, &r.offRotZ)
        && FieldOffset(w[GET_CELL].fn, IsLdrX, 8, &r.offCell)
        && FieldOffset(w[GET_FORM_ID].fn, IsLdrW, 4, &r.offFormId)
        && TailCall(w[GET_WORLD].fn, &r.getWorldspace, 2)
        && FieldOffset(w[WEAPON_DRAWN].fn, IsLdrH, 2, &r.offActorState, 2);
    /* IsDead: ldr x8, [x2]; mov x0, x2; mov w1, #1; ldr x2, [x8, #off]; br x2 */
    for (int i = 0; ok && i < 6; i++) {
        u32 c = Code(w[IS_DEAD].fn)[i];
        if (IsLdrX(c) && Rn(c) == 8) { vt = LdrImm(c) * 8; break; }
    }
    if (!ok || !vt) { Log("decode failed"); return false; }
    r.offPos = posY - 4;
    r.vtIsDead = vt;
    g_res = r;
    g_ready = true;
    g_puppets = w[PLACE_AT_ME].fn && w[SET_POSITION].fn && ResolvePuppets(w[PLACE_AT_ME].fn, w[SET_POSITION].fn);
    ResolveAnim();
    g_drawWeapon = CheckedFn(DRAW_WEAPON);
    g_dontMove = CheckedFn(DONT_MOVE);
    g_setPosition = CheckedFn(ACTOR_SET_POSITION);
    g_coc = CheckedFn(CENTER_ON_CELL_FN);
#ifdef SSW_OFFLINE
    /* offline: no puppets, no network; only what the game mechanics (scrolls.cpp, glow.cpp) use */
    Log("offline: trade %s, scrolls %s, equipment %s", trade::Resolve() ? "on" : "off",
        scrolls::Resolve() ? "on" : "off", equip::Resolve() ? "on" : "off");
    uintptr_t createMessage = 0;
#ifdef SSW_TESTCTL
    console::Resolve();
    g_killImpl = CheckedFn(KILL_IMPL);            // the test console's "kill" (no hook)
#endif
#else
    if (uintptr_t a = CheckedFn(CHAR_DRAW)) CharDrawHook::InstallAtPtr(a);
    if (uintptr_t a = CheckedFn(GRAPH_NOTIFY)) GraphNotify::InstallAtPtr(a);
    if ((g_killImpl = CheckedFn(KILL_IMPL))) KillImplHook::InstallAtPtr(g_killImpl);
    g_resurrect = CheckedFn(RESURRECT);
    if (uintptr_t a = CheckedFn(ACTOR_DATA_LEVEL)) LevelHook::InstallAtPtr(a);
    world::Install();
    Log("actions %s, trade %s", actions::Resolve() ? "on (Skyrim Together style)" : "off", trade::Resolve() ? "on" : "off");
    Log("pvp %s, followers %s, quests %s", pvp::Resolve() ? "on" : "off", followers::Resolve() ? "on" : "off",
        quests::Resolve() ? "on" : "off");
    Log("shared npc health %s", npcsync::Resolve() ? "on" : "off");
    actors::Resolve();
    projectiles::Resolve();
    Log("avatar %s", avatar::Resolve() ? "on" : "off");
    Log("spells on puppets %s", magic::Resolve() ? "on" : "off");
    Log("shared weather %s", weather::Resolve() ? "on" : "off");
    Log("riding %s", mount::Resolve() ? "on" : "off");
    Log("nameplates %s", plates::Resolve() ? "on" : "off");
    Log("spell scrolls %s", scrolls::Resolve() ? "on" : "off");
    g_deleteNative = w[DELETE].fn;
    if (g_deleteNative) Debug("Delete native at +%lx", g_deleteNative - exl::util::modules::GetTargetStart());
    uintptr_t createMessage = CheckedFn(CREATE_MESSAGE);
    menu::SetCreateMessage(createMessage);
    console::Resolve();
    Log("appearance %s, equipment %s", appearance::Resolve() ? "on" : "off", equip::Resolve() ? "on" : "off");
#endif
    truehud::Install();                           // (05.10) TrueHUD bars: the plain game and the online one
    Log("menu %s, console %s", createMessage ? "on" : "off", console::Available() ? "on" : "off");
#ifdef SSW_OFFLINE
    if (true) {                                   // the VM pump drives scrolls/glow
#else
    if (g_anim.ok || createMessage) {
#endif
        uintptr_t pump = CheckedFn(VM_PUMP);
        if (pump) VmPump::InstallAtPtr(pump);
        else { g_anim.ok = false; menu::SetCreateMessage(0); Log("vm pump not found: animation and menu off"); }
    }
    Log("puppets %s, animation %s", g_puppets ? "on" : "off", g_anim.ok ? "on (1.1.177)" : "off (other build)");
    Debug("resolved: pos %x rotZ %x cell %x formId %x vtIsDead %x", r.offPos, r.offRotZ, r.offCell, r.offFormId, r.vtIsDead);
    return true;
}

const Resolved& Get() { return g_res; }
bool Ready() { return g_ready; }

void* Player() {
    if (!g_ready) return nullptr;
    auto var = *reinterpret_cast<void***>(g_res.playerSlot);
    return var ? *var : nullptr;
}

void Notify(const char* msg) {
    if (g_ready) reinterpret_cast<NotifyFn>(g_res.notify)(msg, nullptr, true);
}

void InstallFrameHook() {
    using FullFn = void (*)(nn::hid::NpadFullKeyState*, const u32&);
    using HandFn = void (*)(nn::hid::NpadHandheldState*, const u32&);
    using DualFn = void (*)(nn::hid::NpadJoyDualState*, const u32&);
    PollFullKey::InstallAtPtr(reinterpret_cast<uintptr_t>(static_cast<FullFn>(&nn::hid::GetNpadState)));
    PollHandheld::InstallAtPtr(reinterpret_cast<uintptr_t>(static_cast<HandFn>(&nn::hid::GetNpadState)));
    PollJoyDual::InstallAtPtr(reinterpret_cast<uintptr_t>(static_cast<DualFn>(&nn::hid::GetNpadState)));
}

/* a pointer found by guessing must be checked against the real memory map before it is read: a value that only
   looked like a heap address crashed the console (02.10, scrolls Calibrate). The last region is cached. */
bool Readable(const void* p, u32 len) {
    static uintptr_t lo, hi;
    uintptr_t v = reinterpret_cast<uintptr_t>(p);
    if (!v || (v & 3)) return false;
    if (v >= lo && v + len <= hi) return true;
    MemoryInfo mi{}; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, v)) || !(mi.perm & 1) || mi.type == 0 || mi.addr + mi.size < v + len) {
#ifdef SSW_TESTCTL
        /* in Eden this read would have been harmless or unseen; on the console it is a Data Abort. Logged so the
           emulator test catches it (grep "UNSAFE READ") */
        static int logged;
        if (logged < 20) { logged++; Log("UNSAFE READ %p (+%x) from %p", p, len, __builtin_return_address(0)); }
#endif
        return false;
    }
    lo = mi.addr; hi = mi.addr + mi.size;
    return true;
}

uintptr_t ModuleAddr(u32 offset) { return exl::util::modules::GetTargetOffset(offset); }

uintptr_t Checked177(u32 offset, const u32* sig) {
    AnimFn f{offset, {sig[0], sig[1], sig[2], sig[3]}};
    return CheckedFn(f);
}

void* Singleton(uintptr_t gotSlot) { return SingletonImpl(gotSlot); }
void* RefFromHandle(u32 handle) { return handle ? LookupRef(handle) : nullptr; }

void* GameAlloc(u64 size) { return g_pup.alloc ? reinterpret_cast<AllocFn>(g_pup.alloc)(size) : nullptr; }

const char* DistanceText(const proto::State& s) {
    static char buf[48];
    void* player = Player();
    if (!player) return "?";
    const float* p = reinterpret_cast<const float*>(static_cast<u8*>(player) + g_res.offPos);
    u32 cell = FormId(Field<void*>(player, g_res.offCell));
    u32 world = FormId(reinterpret_cast<WorldFn>(g_res.getWorldspace)(player));
    if (s.cell != cell && (s.worldspace == 0 || s.worldspace != world)) return "в другом месте";
    float dx = s.x - p[0], dy = s.y - p[1], dz = s.z - p[2];
    snprintf(buf, sizeof buf, "%d м", (int)(sqrtf(dx * dx + dy * dy + dz * dz) * 0.01428f));
    return buf;
}

u32 PuppetFormId(u16 playerId) {
    for (int k = 0; k < g_puppetCount; k++)
        if (g_puppetsList[k].id == playerId)
            if (void* ref = LookupRef(g_puppetsList[k].handle)) return FormId(ref);
    return 0;
}

void PuppetAi(void* ref, bool on) { EnableAi(ref, on); }

/* TESDataHandler (SE 1.5 layout, as the Switch build): u32 loadedModCount then TESFile* loadedMods[0xFF];
   TESFile::fileName at +0x58. The offset is found once by looking for Skyrim.esm as the first mod. */
u32 g_fileNameOff = 0x58, g_filesPtr = 8;
bool PluginList(char* out, u32 size) {
    auto dh = static_cast<u8*>(SingletonImpl(g_pup.dataHandlerSlot));
    if (!dh || !size) return false;
    static u32 off;
    if (!off) {
        static int tries;
        if (tries++ > 3) return false;                       // not found: stop looking (logged once)
        int cand = 0;
        for (u32 o = 0; o < 0x2000 && !off; o += 8) {
            u32 n = *reinterpret_cast<u32*>(dh + o);
            if (n < 2 || n > 255) continue;
            for (u32 po = 8; po <= 16 && !off; po += 8) {           // array right after the count, or a pointer to it
                auto first = *reinterpret_cast<u8**>(dh + o + po);
                MemoryInfo mi{}; u32 pi;
                uintptr_t fp = reinterpret_cast<uintptr_t>(first);
                if ((fp & 7) || R_FAILED(svcQueryMemory(&mi, &pi, fp)) || !(mi.perm & 1) || mi.addr + mi.size < fp + 0x110) continue;
                cand++;
                for (u32 fo = 0x20; fo < 0x100; fo += 4)
                    if (!memcmp(first + fo, "Skyrim.esm", 11)) { off = o; g_fileNameOff = fo; g_filesPtr = po; break; }
            }
        }
        Logging.Log("[coop] plugin list: %s (dh %p, %d candidates, count +%x, files +%x, name +%x)", off ? "found" : "not found",
                    dh, cand, off, g_filesPtr, g_fileNameOff);
    }
    if (!off) return false;
    u32 n = *reinterpret_cast<u32*>(dh + off);
    u32 len = 0;
    out[0] = 0;
    for (u32 i = 0; i < n && len + 70 < size; i++) {
        auto f = *reinterpret_cast<u8**>(dh + off + g_filesPtr + i * 8);
        if (f) len += snprintf(out + len, size - len, "%s\n", reinterpret_cast<const char*>(f + g_fileNameOff));
    }
    return true;
}

u16 PuppetOwner(const void* ref) {
    if (!ref) return 0;
    for (int k = 0; k < g_puppetCount; k++)
        if (g_puppetsList[k].id && LookupRef(g_puppetsList[k].handle) == ref) return g_puppetsList[k].id;
    return 0;
}
void* FakeVm() { return FakeVmPtr(); }
const float* RefPos(const void* ref) { return reinterpret_cast<const float*>(static_cast<const u8*>(ref) + g_res.offPos); }

/* test aid (console "face"): turn the player towards a reference */
void FaceTowards(void* ref) {
    void* player = Player();
    if (!player || !ref) return;
    auto pp = reinterpret_cast<float*>(static_cast<u8*>(player) + g_res.offPos);
    auto tp = reinterpret_cast<float*>(static_cast<u8*>(ref) + g_res.offPos);
    reinterpret_cast<float*>(static_cast<u8*>(player) + g_res.offRotZ)[0] = atan2f(tp[0] - pp[0], tp[1] - pp[1]);
}
bool SendAnimEvent(void* ref, const char* event) { return AnimEvent(ref, event); }

void Press(u64 buttons, u32 ms) { g_ownButtons = buttons; g_ownUntil = NowMs() + ms; }

int PluginIndex(const char* file) {
    static char list[8192];
    if (!PluginList(list, sizeof list)) return -1;
    int i = 0;
    for (char* p = list; *p; i++) {
        char* e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == strlen(file) && !strncasecmp(p, file, n)) return i;
        if (!e) break;
        p = e + 1;
    }
    return -1;
}

bool Settled() {
    static void* lastCell;
    static u64 since;
    static float last[3];
    void* player = Player();
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    void* cell = player ? Field<void*>(player, g_res.offCell) : nullptr;
    if (!player || !cell || !Field<void*>(player, 0x68)) { lastCell = nullptr; return false; }
    const float* p = reinterpret_cast<const float*>(static_cast<u8*>(player) + g_res.offPos);
    float dx = p[0] - last[0], dy = p[1] - last[1], dz = p[2] - last[2];
    last[0] = p[0]; last[1] = p[1]; last[2] = p[2];
    bool jumped = dx * dx + dy * dy + dz * dz > 600.0f * 600.0f;   // a teleport inside the cell (tp, MoveTo)
    if (cell != lastCell || jumped) { lastCell = cell; since = now; return false; }
    return now - since >= 5000;
}

void* DataHandler() { return g_pup.dataHandlerSlot ? SingletonImpl(g_pup.dataHandlerSlot) : nullptr; }

volatile bool g_tpWanted;
u32 g_tpCell, g_tpWorld;
float g_tpPos[3];
const char* g_tpArrived = "";
void TeleportTo(u32 cell, u32 world, float x, float y, float z, const char* arrived) {
    g_tpArrived = arrived;
    g_tpCell = cell; g_tpWorld = world; g_tpPos[0] = x; g_tpPos[1] = y; g_tpPos[2] = z;
    g_tpWanted = true;
    Logging.Log("[coop] teleport requested %x %x", cell, world);
}

/* a reference's worldspace (null inside an interior) */
void* WorldOf(void* ref) { return ref ? reinterpret_cast<WorldFn>(g_res.getWorldspace)(ref) : nullptr; }

bool WhereAmI(u32* cell, u32* world, float* pos) {
    void* player = Player();
    if (!player || !Field<void*>(player, g_res.offCell)) return false;
    *cell = FormId(Field<void*>(player, g_res.offCell));
    *world = FormId(reinterpret_cast<WorldFn>(g_res.getWorldspace)(player));
    memcpy(pos, static_cast<u8*>(player) + g_res.offPos, 12);
    return true;
}

/* Teleport = Debug.CenterOnCell's engine function 0x6fa428(player, name, TESObjectCELL*) — it takes the cell
   directly — then, once that cell is ours, Actor::SetPosition to the exact spot. (MoveTo_Impl from the VM pump
   thread froze the game.) */
u64 g_tpUntil;
void PumpTeleport() {
    void* player = Player();
    if (!player) return;
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    if (g_tpWanted) {
        g_tpWanted = false;
        uintptr_t coc = g_coc;
        void* cell = equip::Lookup(g_tpCell);
        Debug("teleport to cell %x (%p) at %.0f %.0f %.0f", g_tpCell, cell, g_tpPos[0], g_tpPos[1], g_tpPos[2]);
        if (!coc || !cell) { Notify("Кооп: туда не попасть"); return; }
        if (FormId(Field<void*>(player, g_res.offCell)) != g_tpCell)
            reinterpret_cast<void (*)(void*, const char*, void*)>(coc)(player, "", cell);
        g_tpUntil = now + 60000;
        return;
    }
    if (!g_tpUntil) return;
    if (now > g_tpUntil) { g_tpUntil = 0; return; }
    /* arrived: same cell (interiors) or same world (exteriors), and loaded */
    void* cell = Field<void*>(player, g_res.offCell);
    u32 world = FormId(reinterpret_cast<WorldFn>(g_res.getWorldspace)(player));
    bool there = cell && (FormId(cell) == g_tpCell || (g_tpWorld && world == g_tpWorld));
    if (!there || !g_setPosition) return;
    g_tpUntil = 0;
    reinterpret_cast<void (*)(void*, const float*, bool)>(g_setPosition)(player, g_tpPos, true);
    Notify(g_tpArrived);
}

bool KillActor(u32 formId) {
    auto actor = static_cast<u8*>(equip::Lookup(formId));
    if (!actor || actor[0x1A] != 0x3E || !g_killImpl) return false;
    reinterpret_cast<void (*)(void*, void*, float, bool, bool)>(g_killImpl)(actor, Player(), 0.0f, true, false);
    return true;
}

/* A plain reference of any base at this position (chest.cpp's hidden container); held like the puppets. */
void* PlaceObject(void* base, float x, float y, float z, bool persist) {
    void* player = Player();
    if (!player || !base || !g_pup.create) return nullptr;
    float pos[3] = {x, y, z}, rot[3] = {0, 0, 0};
    void* cell = Field<void*>(player, g_res.offCell);
    void* ws = reinterpret_cast<WorldFn>(g_res.getWorldspace)(player);
    RefHandle h = reinterpret_cast<CreateFn>(g_pup.create)(SingletonImpl(g_pup.dataHandlerSlot), base, pos, rot, cell, ws,
                                                           nullptr, nullptr, 0, persist, true);
    void* ref = LookupRef(h.v);
    if (ref) __atomic_add_fetch(reinterpret_cast<u32*>(static_cast<u8*>(ref) + 0x28), 1u, __ATOMIC_ACQ_REL);
    return ref;
}

void DeleteRef(void* ref) {
    if (!ref || !g_deleteNative) return;
    FakeVmPtr();
    reinterpret_cast<bool (*)(void*, u32, void*)>(g_deleteNative)(&g_fakeVm, 0, ref);
}

/* ---- zones (npcpos.cpp): the world's NPCs.
   ProcessLists (1.1.177: *GOT 0x2144b80, the Game.FindClosestActor native 0xa29c10 walks it): high-process actor
   handles u32 at +0x50, count u32 at +0x5C. */
constexpr AnimFn FIND_CLOSEST_ACTOR{0xa29c10, {0xD100C3FF, 0xA9017BFD, 0x910043FD, 0xF90013F3}};
constexpr u32 PROCESS_LISTS_GOT = 0x2144b80;
int HighActors(void** out, int max) {
    static int ok = -1;
    if (ok < 0) ok = CheckedFn(FIND_CLOSEST_ACTOR) != 0;
    auto pl = ok ? static_cast<u8*>(SingletonImpl(ModuleAddr(PROCESS_LISTS_GOT))) : nullptr;
    if (!pl) return 0;
    u32 n = Field<u32>(pl, 0x5C);
    auto hs = Field<u32*>(pl, 0x50);
    int k = 0;
    for (u32 i = 0; hs && i < n && i < 2048 && k < max; i++)
        if (hs[i])
            if (void* ref = LookupRef(hs[i])) out[k++] = ref;
    return k;
}

/* an NPC of this world as the zone owner sees it; false = not one to share (the player, a puppet, made at runtime) */
bool NpcDescribe(void* ref, proto::NpcState* s) {
    void* player = Player();
    if (!ref || ref == player || PuppetOwner(ref) || !Field<void*>(ref, 0x68)) return false;
    u32 id = FormId(ref);
    if (!id || (id >> 24) == 0xFF) return false;
    const float* p = reinterpret_cast<const float*>(static_cast<u8*>(ref) + g_res.offPos);
    *s = proto::NpcState{id, p[0], p[1], p[2], Field<float>(ref, g_res.offRotZ), 0.0f, 0, {}};
    auto vt = *reinterpret_cast<uintptr_t**>(ref);
    if (reinterpret_cast<IsDeadFn>(vt[g_res.vtIsDead / 8])(ref, true)) s->flags |= proto::NPC_DEAD;
    u64 st = (u64)Field<u16>(ref, g_res.offActorState) << 32;
    if ((st & 0xE000000000ull) > 0x4000000000ull) s->flags |= proto::NPC_DRAWN;
    return true;
}

/* the zone owner's NPC state on our copy of it: moved (Actor::SetPosition, smoothly), turned, walking animation,
   weapon out or not; its own AI keeps animating it but must not walk it (SetDontMove) */
void NpcFollow(void* ref, const proto::NpcState& s, bool first, bool movingChanged, float direction) {
    if (!ref || !g_setPosition) return;
    if (first && g_dontMove) reinterpret_cast<void (*)(void*, u32, void*, bool)>(g_dontMove)(nullptr, 0, ref, true);
    bool moving = s.flags & proto::NPC_MOVING;
    AnimWanted a{};
    a.moving = moving; a.speed = s.speed; a.tx = s.x; a.ty = s.y; a.tz = s.z; a.rot = s.rot; a.hasTarget = true;
    MoveSmooth(ref, a, 0);
    if (moving) {
        AnimFloat(ref, "Speed", s.speed);
        AnimFloat(ref, "SpeedSampled", s.speed);
        AnimFloat(ref, "Direction", direction);
    }
    if (movingChanged) AnimEvent(ref, moving ? "moveStart" : "moveStop");
    u64 st = (u64)Field<u16>(ref, g_res.offActorState) << 32;
    bool drawn = (st & 0xE000000000ull) > 0x4000000000ull, want = s.flags & proto::NPC_DRAWN;
    if (drawn != want && g_drawWeapon) reinterpret_cast<void (*)(void*, bool)>((*reinterpret_cast<uintptr_t**>(ref))[0x578 / 8])(ref, want);
}

void NpcRelease(void* ref) {
    if (ref && g_dontMove) reinterpret_cast<void (*)(void*, u32, void*, bool)>(g_dontMove)(nullptr, 0, ref, false);
}

void* PuppetRef(u16 playerId) {
    for (int k = 0; k < g_puppetCount; k++)
        if (g_puppetsList[k].id == playerId) return LookupRef(g_puppetsList[k].handle);
    return nullptr;
}

/* ---- (08.10) the actor core (actors.cpp) */
bool ForcePosition(void* ref, const float* pos) {
    if (!ref || !g_setPosition) return false;
    auto vt = *reinterpret_cast<uintptr_t**>(ref);
    if (vt[VT_SET_POSITION / 8] != g_setPosition) return false;          // not an actor of the expected class
    reinterpret_cast<void (*)(void*, const float*, bool)>(g_setPosition)(ref, pos, true);
    return true;
}

void SetHeading(void* ref, float rot) { *reinterpret_cast<float*>(static_cast<u8*>(ref) + g_res.offRotZ) = rot; }

float Heading(const void* ref) { return *reinterpret_cast<const float*>(static_cast<const u8*>(ref) + g_res.offRotZ); }

void DontMove(void* ref, bool on) {
    if (ref && g_dontMove) reinterpret_cast<void (*)(void*, u32, void*, bool)>(g_dontMove)(nullptr, 0, ref, on);
}

bool IsDeadActor(void* ref) {
    auto vt = *reinterpret_cast<uintptr_t**>(ref);
    return reinterpret_cast<IsDeadFn>(vt[g_res.vtIsDead / 8])(ref, true);
}

void PlaceOf(void* ref, u32* cell, u32* world) {
    *cell = FormId(Field<void*>(ref, g_res.offCell));
    *world = FormId(reinterpret_cast<WorldFn>(g_res.getWorldspace)(ref));
}

u32 RefFormId(const void* ref) { return ref ? *reinterpret_cast<const u32*>(static_cast<const u8*>(ref) + g_res.offFormId) : 0; }

}  // namespace coop::game
