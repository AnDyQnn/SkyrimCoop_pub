#include "actors.hpp"
#include "actions.hpp"
#include "client.hpp"
#include "quests.hpp"
#include "mount.hpp"
#include "death.hpp"
#include "equip.hpp"
#include "projectiles.hpp"
#include "game.hpp"
#include "lib.hpp"
#include <nn/os.hpp>
#include <cstring>
#include <cmath>

/* The actor core, the Skyrim Together way (08.10.2026; notes/ST_РАЗБОР.md, server/actors.py).

   Every actor loaded here (a plugin NPC or creature) is asked for at the server (ACT_ASSIGN). The first console to
   ask owns it: its AI is the real one, and ten times a second it sends a snapshot of all its actors (ACT_MOVES:
   position, heading, cell, health, actor state, animation graph variables by name — the humanoid tables of ST's
   AnimationGraphDescriptor_Master_Behavior) and every action they perform (ACT_ACTION, ActorMediator::PerformAction).
   On every other console the same actor is REMOTE:
     - its own actions do not run (actions.cpp asks IsRemote before PerformAction), only the owner's replayed ones;
     - its AI does not walk it (SetDontMove), and right after the game updates it each frame (Actor::Update, hooked)
       it is put where the owner had it 250 ms ago — interpolated between the two snapshots around that moment —
       with the owner's heading and graph variables (ST: InterpolationSystem + AnimationSystem). Whatever its own
       update did that frame is overwritten before it is drawn: no fighting, no jitter.
     - its health follows the owner's; damage done here goes to the owner (ACT_HEALTH), death comes from the owner.
   When the owner unloads it or leaves, the server hands it to another console that has it (ACT_OWNER).
   Players are the same: each console sends its own body as a row (sid ACT_PLAYER | id) and the others drive the
   puppet with it. 1.1.177 addresses (tools/xrefs.py, the Papyrus natives; Actor vtable slots are SE's + 9):
     Actor::Update(float) 0x61c930 (vtable +0x5B0); graph holder = ref + 0x38, its vtable +0x88 / +0x90 / +0x98 get
     a bool / int / float variable; set: 0x5339f4 / 0x533abc / 0x533b80 (holder, const BSFixedString&, value). */
namespace coop::actors {
namespace {

struct Fn { u32 off; u32 sig[4]; };
constexpr Fn UPDATE{0x61c930, {0xD10443FF, 0xFD004BEA, 0x6D0A23E9, 0xA90B7BFD}};
constexpr Fn SET_BOOL{0x5339f4, {0xD10103FF, 0xA9027BFD, 0x910083FD, 0xA9034FF4}};
constexpr Fn SET_INT{0x533abc, {0xD10103FF, 0xA9027BFD, 0x910083FD, 0xA9034FF4}};
constexpr Fn SET_FLOAT{0x533b80, {0xD10103FF, 0xFD0013E8, 0xF9001FF3, 0xA902FBFD}};
constexpr Fn FS_CTOR{0xae6da8, {0xA9BE7BFD, 0xF9000BF3, 0x910003FD, 0xF900001F}};
constexpr Fn GET_AV{0x9fe0f0, {0xD10783FF, 0xA91A7BFD, 0x910683FD, 0xF900DBFC}};
constexpr Fn DAMAGE_AV{0x9fd2c0, {0xD107C3FF, 0xFD00D3E8, 0xA91B7BFD, 0x9106C3FD}};
constexpr Fn RESTORE_AV{0xa02820, {0xD10783FF, 0xFD00D3E8, 0xF900DFFC, 0xA91AFBFD}};
/* Actor's change of an actor value (this, modifier, av, cause, value); health damage (av 0x18, value < 0) goes on to
   HandleHealthDamage (vtable +0x870 = SE 0x104 + 9). Every hit — weapon, spell, fall, our own natives — passes here */
constexpr Fn MOD_AV{0x663114, {0xD10183FF, 0xFD000BE8, 0xA9027BFD, 0x910083FD}};
constexpr u32 AV_HEALTH = 0x18;
/* ActorEquipManager::EquipObject (mgr, actor, form, extra, count, slot, queue, force, sounds, applyNow) — equip.cpp */
constexpr Fn EQUIP_OBJECT{0x67a9f8, {0xD10243FF, 0xA9037BFD, 0x9100C3FD, 0xA9046FFC}};

constexpr int MAX = 96, PLAYERS = 8, BUF = 10;
constexpr u64 DELAY_MS = 250, SEND_MS = 100, SCAN_MS = 250;
constexpr u32 STATE1 = 0xF8, STATE2 = 0xFC;

/* graph variables, ST's humanoid tables (the ones a graph does not have are skipped, by the "has" bits) */
const char* const FLOATS[proto::ACT_FLOATS] = {"Speed", "Direction", "TurnDelta", "SpeedSampled", "Pitch", "weapAdj",
                                               "CastBlend", "VelocityZ"};
const char* const INTS[proto::ACT_INTS] = {"iRightHandEquipped", "iLeftHandEquipped", "i1HMState", "iState",
                                           "iLeftHandType", "iRightHandType", "iSyncIdleLocomotion", "iIsInSneak"};
const char* const BOOLS[32] = {"IsSneaking", "IsBleedingOut", "IsCastingDual", "IsCastingRight", "IsCastingLeft",
                               "IsBlockHit", "bWantCastLeft", "bWantCastRight", "bWantCastVoice", "bAnimationDriven",
                               "bCastReady", "IsAttacking", "bMagicDraw", "bMLh_Ready", "bMRh_Ready", "bInMoveState",
                               "bSprintOK", "bIdlePlaying", "bEquipUnequip", "bIsRiding", "bInJumpState",
                               "bLeftHandAttack", "IsBlocking", "IsBashing", "IsStaggering", "IsRecoiling",
                               "IsEquipping", "IsUnequipping", "bBowDrawn", "IsShouting", "bMotionDriven",
                               "bAllowRotation"};

/* (08.10) creatures: ST's WolfRoot / BearRoot / HorseRoot / Draugr tables (a graph without iRightHandEquipped) */
const char* const C_FLOATS[proto::ACT_FLOATS] = {"Speed", "Direction", "TurnDelta", "SpeedSampled", "SampledSpeed",
                                                 "TurnDeltaDamped", "walkBackSpeedMult", "HorseSpeedSampled"};
const char* const C_INTS[proto::ACT_INTS] = {"iSyncIdleLocomotion", "iSyncTurnState", "iSyncForwardState", "iCombatStance",
                                             "iMovementSpeed", "iSyncSprintState", "iState", "iRightHandType"};
const char* const C_BOOLS[32] = {"bHeadTrackingOn", "bAnimationDriven", "bAllowRotation", "bHeadTracking",
                                 "bDisableHeadTrack", "IsRecoiling", "IsStaggering", "IsAttacking", "bSkeeverLunge",
                                 "bFootIKEnable", "isMoving", "IsSprinting", "bHorseFootIKEnable", "IsFiringBow",
                                 "IsAttackReady", "bWeapReady", "bEquipOk", "bMLh_Ready", "bCastReady", "bCanHeadTrack",
                                 "IsEquipping", "IsUnequipping", "IsBlocking", "IsBashing", "bInJumpState", "IsShouting",
                                 "bMotionDriven", "bIdlePlaying", "IsSneaking", "bBowDrawn", "bWantCastLeft", "bWantCastRight"};

struct FixedString { const char* data; };
FixedString g_fsF[proto::ACT_FLOATS], g_fsI[proto::ACT_INTS], g_fsB[32], g_fsHealth, g_fsMagicka, g_fsStamina;
FixedString g_cF[proto::ACT_FLOATS], g_cI[proto::ACT_INTS], g_cB[32];
constexpr u32 HAS_CREATURE = 1u << 17;          // the row carries the creature tables

uintptr_t g_setBool, g_setInt, g_setFloat, g_fsCtor, g_getAv, g_damageAv, g_restoreAv;
volatile bool g_ourDamage;                 // our own health corrections pass the damage rules
volatile bool g_equipOverride;             // our own equipping of remote actors passes
bool g_ok;

struct Sample { s64 t; proto::ActMove m; };

struct Entry {
    void* ref;
    u32 formId, cookie, sid, epoch;
    u16 owner, playerId;
    bool used, assigned, mine, player, seen, deadSent, applied;
    bool spawned, dm, killDue;             // our copy of someone's temporary actor; SetDontMove done; dead on arrival
    u32 equipHash; u64 equipCheckMs;       // owned: what it wears, last sent
    int equipN; bool equipDue;             // remote: what its owner says it wears
    equip::Item equipWant[equip::MAX_ITEMS];
    float lastHealth;
    u64 avLogMs;                           // the last magicka / stamina correction logged
    u64 localHitMs, lastRowMs, hookMs, bornMs;
    volatile u32 lock;                     // the buffer: the game thread writes, Update (any thread) reads
    Sample buf[BUF];
    int n;
    u32 lastBools; s16 lastInts[proto::ACT_INTS]; u32 lastHas;
};
Entry g_e[MAX + PLAYERS];
/* read from other threads (actions, Update): the remote actors' references, the owned ones' and their sids */
void* volatile g_remoteRef[MAX + PLAYERS];
void* volatile g_ownedRef[MAX];
volatile u32 g_ownedSid[MAX];

s64 g_off[256]; bool g_offSet[256];        // per owner player id: our clock minus theirs (smallest seen)
u32 g_cookie;
nn::os::ThreadType* g_pumpThread;
bool g_live;
u64 g_hookOn, g_hookOff;                   // Update hook calls on the pump thread / elsewhere (diagnostics)

/* actions of owned actors, from any thread (PerformAction runs on AI threads too): a lock-free ring */
struct QAct { u32 sid; proto::ActionMsg a; };
QAct g_qa[64];
u32 g_qaHead, g_qaTail;
volatile u32 g_qaLock;
void QLock() { while (__atomic_exchange_n(&g_qaLock, 1u, __ATOMIC_ACQUIRE)) {} }
void QUnlock() { __atomic_store_n(&g_qaLock, 0u, __ATOMIC_RELEASE); }

template <class T> T& At(void* p, u32 off) { return *reinterpret_cast<T*>(static_cast<u8*>(p) + off); }
u64 NowMs() { return nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds(); }
void* Holder(void* ref) { return static_cast<u8*>(ref) + 0x38; }
bool Has3D(void* ref) { return ref && At<void*>(ref, 0x68); }
/* the NPC to make a copy of a temporary actor from: the leveled list's pick (ExtraLeveledCreature.templateBase, as ST's
   Actor::GetLeveledPick), else its base when that is a plugin NPC; 0 = none (made by a script from a temporary base) */
constexpr u32 VT_EXTRA_LEVELED = 0x19ca1b8;              // ExtraLeveledCreature vtable (RTTI, 1.1.177)
constexpr u8 FT_NPC = 0x2B;
u32 PlainNpc(void* npc) {
    if (!npc || static_cast<u8*>(npc)[0x1A] != FT_NPC) return 0;
    u32 id = game::RefFormId(npc);
    return (id >> 24) == 0xFF ? 0 : id;
}
u32 LeveledPick(void* ref) {
    if (!VT_EXTRA_LEVELED) return 0;
    uintptr_t vt = game::ModuleAddr(VT_EXTRA_LEVELED);
    auto x = At<u8*>(ref, 0x70);                          // ExtraDataList: the first BSExtraData
    for (int i = 0; x && i < 64; i++, x = At<u8*>(x, 8)) {
        if (!game::Readable(x, 0x20)) return 0;
        if (At<uintptr_t>(x, 0) == vt) return PlainNpc(At<void*>(x, 0x18));
    }
    return 0;
}

void Lock(Entry& e) { while (__atomic_exchange_n(&e.lock, 1u, __ATOMIC_ACQUIRE)) {} }
void Unlock(Entry& e) { __atomic_store_n(&e.lock, 0u, __ATOMIC_RELEASE); }

bool IsPuppetRef(const void* a) { return a && game::PuppetOwner(a); }

/* (08.10, the Skyrim Together rules, Actor.cpp HookDamageActor) a hit counts on ONE console only:
     - our player hit by another player: only in PvP, and then it comes as HIT from their console;
     - another player's puppet: never damaged here (our hit on it goes to them as HIT; the server drops it outside PvP);
     - hit by another player's puppet: not here — their console applies it to its copy and sends it;
     - hit by our player: applied here (a remote actor's owner gets it as ACT_HEALTH);
     - our own (owned) actors: applied here; a remote actor hit by anything else: not here, its owner decides. */
HOOK_DEFINE_TRAMPOLINE(ModAvHook) {
    static void Callback(void* a, u32 mod, u32 av, void* cause, float v) {
        if (av != AV_HEALTH || v >= 0.0f || g_ourDamage || !g_live || !a) { Orig(a, mod, av, cause, v); return; }
        void* player = game::Player();
        if (a == player) {
            if (cause && IsPuppetRef(cause)) return;                 // PvP hits come as HIT (pvp.cpp)
            Orig(a, mod, av, cause, v); return;
        }
        if (IsPuppetRef(a)) {
            if (cause == player) PushHit(game::PuppetOwner(a), -v);   // dealt to them on their console (PvP rooms)
            return;
        }
        if (cause == player) { Orig(a, mod, av, cause, v); return; }
        if (cause && IsPuppetRef(cause)) {
            static u32 said;
            if (said++ < 20) Debug("урон: от куклы %08x по %08x не считается здесь (%.0f)", game::RefFormId(cause), game::RefFormId(a), v);
            return;
        }
        if (IsRemote(a)) {
            static u32 said;
            if (said++ < 20) Log("урон: %08x по чужому %08x отклонён (%.0f, вид %u)", game::RefFormId(cause), game::RefFormId(a), v, mod);
            return;
        }
        Orig(a, mod, av, cause, v);
    }
};

/* (08.10, Skyrim Together's EquipManager hooks) a remote actor does not change its own equipment: its owner says what
   it wears (ACT_EQUIP) */
HOOK_DEFINE_TRAMPOLINE(EquipHook) {
    static void Callback(void* mgr, void* actor, void* form, void* extra, u32 count, void* slot, bool queue, bool force,
                         bool sounds, bool applyNow) {
        if (g_live && !g_equipOverride && actor && IsRemote(actor)) return;
        Orig(mgr, actor, form, extra, count, slot, queue, force, sounds, applyNow);
    }
};

u32 HashItems(const equip::Item* it, int n) {
    u32 h = 2166136261u;
    for (int i = 0; i < n; i++) { h = (h ^ it[i].formId) * 16777619u; h = (h ^ it[i].left) * 16777619u; }
    return h ^ (u32)n;
}

float Av(void* a, FixedString* fs) {
    return reinterpret_cast<float (*)(void*, u32, void*, FixedString*)>(g_getAv)(game::FakeVm(), 0, a, fs);
}
void Damage(void* a, float v, FixedString* fs = &g_fsHealth) {
    g_ourDamage = true;
    reinterpret_cast<void (*)(void*, u32, void*, FixedString*, float)>(g_damageAv)(game::FakeVm(), 0, a, fs, v);
    g_ourDamage = false;
}
void Restore(void* a, float v, FixedString* fs = &g_fsHealth) {
    reinterpret_cast<void (*)(void*, u32, void*, FixedString*, float)>(g_restoreAv)(game::FakeVm(), 0, a, fs, v);
}
float Health(void* a) { return Av(a, &g_fsHealth); }

bool GetF(void* ref, FixedString* fs, float* v) {
    void* h = Holder(ref);
    auto fn = reinterpret_cast<bool (*)(void*, FixedString*, float*)>((*reinterpret_cast<uintptr_t**>(h))[0x98 / 8]);
    return fn(h, fs, v);
}
bool GetI(void* ref, FixedString* fs, s32* v) {
    void* h = Holder(ref);
    auto fn = reinterpret_cast<bool (*)(void*, FixedString*, s32*)>((*reinterpret_cast<uintptr_t**>(h))[0x90 / 8]);
    return fn(h, fs, v);
}
bool GetB(void* ref, FixedString* fs, bool* v) {
    void* h = Holder(ref);
    auto fn = reinterpret_cast<bool (*)(void*, FixedString*, bool*)>((*reinterpret_cast<uintptr_t**>(h))[0x88 / 8]);
    return fn(h, fs, v);
}
void SetF(void* ref, FixedString* fs, float v) { reinterpret_cast<bool (*)(void*, FixedString*, float)>(g_setFloat)(Holder(ref), fs, v); }
void SetI(void* ref, FixedString* fs, s32 v) { reinterpret_cast<bool (*)(void*, FixedString*, s32)>(g_setInt)(Holder(ref), fs, v); }
void SetB(void* ref, FixedString* fs, bool v) { reinterpret_cast<bool (*)(void*, FixedString*, bool)>(g_setBool)(Holder(ref), fs, v); }

void Send(u16 type, const void* p, u16 size) {
    if (!PushActorPacket(type, p, size)) {
        static u64 said;
        u64 now = NowMs();
        if (now - said > 5000) { said = now; Debug("actors: очередь пакетов полна (%u)", type); }
    }
}

/* ---- the table */
void Publish(int k) {
    Entry& e = g_e[k];
    bool remote = e.used && (e.player || (e.assigned && !e.mine));
    g_remoteRef[k] = remote ? e.ref : nullptr;
    if (k < MAX) {
        bool owned = e.used && e.assigned && e.mine && !e.player;
        g_ownedSid[k] = owned ? e.sid : 0;
        g_ownedRef[k] = owned ? e.ref : nullptr;
    }
}

void BecomeRemote(Entry& e, bool remote) {
    if (!remote && e.dm && e.ref && Has3D(e.ref)) game::DontMove(e.ref, false);
    e.dm = false;                                          // remote: Pump does it once the 3D is there
    Lock(e);
    e.n = 0; e.lastHas = 0; e.applied = false; e.lastHealth = -1.0f;
    Unlock(e);
}

void Drop(int k, bool tellServer) {
    Entry& e = g_e[k];
    if (!e.used) return;
    if (tellServer && e.assigned && !e.player) { proto::ActSid m{e.sid, e.epoch}; Send(proto::ACT_RELEASE, &m, sizeof m); }
    bool valid = !e.player && e.ref && equip::Lookup(e.formId) == e.ref;
    if (valid && e.spawned) game::DeleteRef(e.ref);       // our copy of someone's temporary actor goes with it
    else if (valid && e.dm && Has3D(e.ref)) game::DontMove(e.ref, false);
    g_remoteRef[k] = nullptr;                              // the Update hook lets go of it first
    if (k < MAX) { g_ownedRef[k] = nullptr; g_ownedSid[k] = 0; }
    Lock(e);
    e = Entry{};                                           // (unlocks)
}

void DropAll(bool tellServer) {
    for (int k = 0; k < MAX + PLAYERS; k++) Drop(k, tellServer);
}

int BySid(u32 sid) {
    for (int k = 0; k < MAX; k++) if (g_e[k].used && g_e[k].assigned && g_e[k].sid == sid) return k;
    return -1;
}
int ByRef(void* ref) {
    for (int k = 0; k < MAX; k++) if (g_e[k].used && g_e[k].ref == ref) return k;
    return -1;
}
int PlayerSlot(u16 id, bool make) {
    int free = -1;
    for (int k = MAX; k < MAX + PLAYERS; k++) {
        if (g_e[k].used && g_e[k].playerId == id) return k;
        if (!g_e[k].used && free < 0) free = k;
    }
    if (!make || free < 0) return -1;
    g_e[free] = Entry{};
    g_e[free].used = g_e[free].player = true;
    g_e[free].playerId = id;
    g_e[free].lastHealth = -1.0f;
    return free;
}

/* ---- snapshots */
void Describe(void* ref, u32 sid, bool withHealth, proto::ActMove* m) {
    memset(m, 0, sizeof *m);
    m->sid = sid;
    const float* p = game::RefPos(ref);
    m->x = p[0]; m->y = p[1]; m->z = p[2];
    m->rot = game::Heading(ref);
    game::PlaceOf(ref, &m->cell, &m->world);
    m->health = withHealth ? Health(ref) : 0.0f;
    m->magicka = withHealth ? Av(ref, &g_fsMagicka) : 0.0f;
    m->stamina = withHealth ? Av(ref, &g_fsStamina) : 0.0f;
    m->state1 = At<u32>(ref, STATE1);
    m->state2 = At<u32>(ref, STATE2);
    if (game::IsDeadActor(ref)) m->has |= 1u << 31;
    if (ref == game::Player() && death::IsDown()) m->has |= 1u << 30;   // knocked down: lying where it fell
    if (!Has3D(ref)) return;
    s32 probe;
    bool creature = !GetI(ref, &g_fsI[0], &probe);       // iRightHandEquipped: humanoids only
    FixedString *F = creature ? g_cF : g_fsF, *I = creature ? g_cI : g_fsI, *B = creature ? g_cB : g_fsB;
    if (creature) m->has |= HAS_CREATURE;
    for (int i = 0; i < proto::ACT_FLOATS; i++)
        if (GetF(ref, &F[i], &m->f[i])) m->has |= 1u << i;
    for (int i = 0; i < proto::ACT_INTS; i++) {
        s32 v;
        if (GetI(ref, &I[i], &v)) { m->i[i] = (s16)v; m->has |= 1u << (8 + i); }
    }
    for (int i = 0; i < 32; i++) {
        bool v;
        if (GetB(ref, &B[i], &v)) { m->boolHas |= 1u << i; if (v) m->bools |= 1u << i; }
    }
    m->has |= 1u << 16;
}

void Received(Entry& e, u16 ownerId, u32 tick, const proto::ActMove& m, u64 now) {
    s64 t;
    if (tick) {
        s64 d = (s64)now - (s64)tick;
        auto& off = g_off[ownerId & 255];
        if (!g_offSet[ownerId & 255] || d < off) { off = d; g_offSet[ownerId & 255] = true; }
        else off += 1;                                     // creeps up so a one-off fast packet does not stick
        t = (s64)tick;
    } else t = (s64)now - g_off[ownerId & 255] - (s64)DELAY_MS;   // the server's copy: show it now
    Lock(e);
    if (e.n && t <= e.buf[e.n - 1].t) {
        if (tick) { Unlock(e); return; }
        t = e.buf[e.n - 1].t + 1;
    }
    if (e.n == BUF) { memmove(e.buf, e.buf + 1, sizeof(Sample) * (BUF - 1)); e.n--; }
    e.buf[e.n++] = Sample{t, m};
    Unlock(e);
    e.lastRowMs = now;
}

float LerpAngle(float a, float b, float k) {
    float d = fmodf(b - a, 6.2831853f);
    if (d > 3.1415927f) d -= 6.2831853f;
    if (d < -3.1415927f) d += 6.2831853f;
    return a + d * k;
}

/* the remote actor where its owner had it DELAY_MS ago */
void ApplyLocked(Entry& e, void* ref, u64 now);
void Apply(Entry& e, void* ref, u64 now) {
    Lock(e);
    ApplyLocked(e, ref, now);
    Unlock(e);
}

void ApplyLocked(Entry& e, void* ref, u64 now) {
    if (!e.n || !Has3D(ref)) return;
    s64 rt = (s64)now - g_off[e.owner & 255] - (s64)DELAY_MS;
    int b = 0;
    while (b < e.n && e.buf[b].t < rt) b++;
    const proto::ActMove *A, *B;
    float k;
    if (b == 0) { A = B = &e.buf[0].m; k = 0; }
    else if (b == e.n) { A = B = &e.buf[e.n - 1].m; k = 0; }
    else {
        A = &e.buf[b - 1].m; B = &e.buf[b].m;
        k = (float)(rt - e.buf[b - 1].t) / (float)(e.buf[b].t - e.buf[b - 1].t);
    }
    if ((A->has >> 30) || (B->has >> 30) || game::IsDeadActor(ref)) return;   // dead / knocked down: lies where it fell
    float pos[3] = {A->x + (B->x - A->x) * k, A->y + (B->y - A->y) * k, A->z + (B->z - A->z) * k};
    /* (08.10, his: "все фигурки мигают") only a real move: a SetPosition every frame onto the same spot redoes the 3D */
    const float* cur = game::RefPos(ref);
    float dx = pos[0] - cur[0], dy = pos[1] - cur[1], dz = pos[2] - cur[2];
    if (dx * dx + dy * dy + dz * dz > 0.25f) game::ForcePosition(ref, pos);
    game::SetHeading(ref, LerpAngle(A->rot, B->rot, k));
    const proto::ActMove* S = k < 0.5f ? A : B;            // whole values: the nearer snapshot
    bool creature = S->has & HAS_CREATURE;
    FixedString *TF = creature ? g_cF : g_fsF, *TI = creature ? g_cI : g_fsI, *TB = creature ? g_cB : g_fsB;
    for (int i = 0; i < proto::ACT_FLOATS; i++)
        if (A->has & B->has & (1u << i) && (A->has & HAS_CREATURE) == (B->has & HAS_CREATURE))
            SetF(ref, &TF[i], A->f[i] + (B->f[i] - A->f[i]) * k);
    for (int i = 0; i < proto::ACT_INTS; i++)
        if ((S->has & (1u << (8 + i))) && (!e.applied || S->i[i] != e.lastInts[i] || !(e.lastHas & (1u << (8 + i)))))
            SetI(ref, &TI[i], S->i[i]);
    u32 changed = e.applied ? (S->bools ^ e.lastBools) : 0xFFFFFFFFu;
    for (int i = 0; i < 32; i++)
        if ((S->boolHas & (1u << i)) && (changed & (1u << i))) SetB(ref, &TB[i], (S->bools >> i) & 1);
    /* sneaking also lives in the actor state (the puppet's own AI reads it) */
    u32& st1 = At<u32>(ref, STATE1);
    st1 = (st1 & ~(1u << 9)) | (S->state1 & (1u << 9));
    memcpy(e.lastInts, S->i, sizeof e.lastInts);
    e.lastBools = S->bools; e.lastHas = S->has; e.applied = true;
}

void* EntryRef(Entry& e) {
    if (e.player) {
        void* p = game::PuppetRef(e.playerId);
        if (p && mount::Carrier(p) != p) p = nullptr;      // riding: game.cpp moves the horse with it
        return p;
    }
    return e.ref;
}

HOOK_DEFINE_TRAMPOLINE(UpdateHook) {
    static void Callback(void* actor, float dt) {
        __atomic_add_fetch(&g_hookIn[0], 1u, __ATOMIC_RELAXED);
        Orig(actor, dt);
        CallbackAfter(actor);
        __atomic_add_fetch(&g_hookOut[0], 1u, __ATOMIC_RELAXED);
    }
    static void CallbackAfter(void* actor) {
        if (!g_live) return;
        int k = -1;
        for (int i = 0; i < MAX + PLAYERS; i++) if (g_remoteRef[i] == actor) { k = i; break; }
        if (k < 0) return;
        /* (08.10, Eden: 2 of 3 actor updates run on the game's worker threads) the actor's own thread, right after
           its own update: the only moment nothing else moves it */
        if (nn::os::GetCurrentThread() != g_pumpThread) g_hookOff++; else g_hookOn++;
        u64 now = NowMs();
        g_e[k].hookMs = now;
        Apply(g_e[k], actor, now);
    }
};

/* ---- packets from the server */
void Handle(u16 type, const u8* p, u16 size, u64 now) {
    u16 me = MyId();
    if (type == proto::ACT_RESET) {
        DropAll(false);
        for (auto& b : g_offSet) b = false;
        Debug("actors: новое подключение, таблица актёров очищена");
    } else if (type == proto::ACT_ASSIGNED && size == sizeof(proto::ActAssigned)) {
        proto::ActAssigned a; memcpy(&a, p, sizeof a);
        for (int k = 0; k < MAX; k++) {
            Entry& e = g_e[k];
            if (!e.used || e.assigned || e.cookie != a.cookie) continue;
            e.assigned = true; e.sid = a.sid; e.epoch = a.epoch; e.owner = a.owner; e.mine = a.mine;
            if (!e.mine) {
                BecomeRemote(e, true);
                if (a.dead && !game::IsDeadActor(e.ref)) game::KillActor(e.formId);
            }
            Publish(k);
            static int logged;
            if (logged++ < 30) Debug("actors: %08x -> sid %u, %s (владелец %u)", e.formId, e.sid, e.mine ? "наш" : "удалённый", e.owner);
            break;
        }
    } else if (type == proto::ACT_OWNER && size == sizeof(proto::ActOwner)) {
        proto::ActOwner o; memcpy(&o, p, sizeof o);
        int k = BySid(o.sid);
        if (k < 0) return;
        Entry& e = g_e[k];
        bool mine = o.owner == me;
        e.epoch = o.epoch; e.owner = o.owner;
        if (mine != e.mine) { e.mine = mine; BecomeRemote(e, !mine); e.deadSent = game::IsDeadActor(e.ref); }
        Publish(k);
        Debug("actors: %08x — теперь владелец %u%s", e.formId, o.owner, mine ? " (мы)" : "");
    } else if (type == proto::ACT_MOVES && size >= sizeof(proto::ActMovesHead)) {
        proto::ActMovesHead h; memcpy(&h, p, sizeof h);
        if (sizeof h + h.count * sizeof(proto::ActMove) != size) return;
        for (int r = 0; r < h.count; r++) {
            proto::ActMove m; memcpy(&m, p + sizeof h + r * sizeof m, sizeof m);
            if (m.sid & proto::ACT_PLAYER) {
                u16 id = (u16)(m.sid & 0xFFFF);
                if (id == me) continue;
                int k = PlayerSlot(id, true);
                if (k < 0) continue;
                g_e[k].owner = id;
                Received(g_e[k], id, h.tick, m, now);
                continue;
            }
            int k = BySid(m.sid);
            if (k >= 0 && !g_e[k].mine) Received(g_e[k], g_e[k].owner, h.tick, m, now);
        }
    } else if (type == proto::ACT_ACTION && size == sizeof(proto::ActAction)) {
        proto::ActAction a; memcpy(&a, p, sizeof a);
        int k = BySid(a.sid);
        if (k < 0 || g_e[k].mine || !Has3D(g_e[k].ref)) return;
        u8 r = actions::Replay(g_e[k].ref, a.a);
        static int logged;
        if (logged++ < 40) Debug("actors: %08x действие владельца %08x '%s' -> %u", g_e[k].formId, a.a.action, a.a.len ? a.a.name : "", r);
    } else if (type == proto::ACT_SPAWN && size == sizeof(proto::ActSpawn)) {
        proto::ActSpawn sp; memcpy(&sp, p, sizeof sp);           // someone's temporary actor near us: one like it here
        if (BySid(sp.sid) >= 0) return;
        void* npc = equip::Lookup(sp.pick ? sp.pick : sp.base);
        if (!npc || static_cast<u8*>(npc)[0x1A] != FT_NPC) { Debug("actors: спавн sid %u — нет NPC %08x/%08x", sp.sid, sp.pick, sp.base); return; }
        int k = 0;
        for (; k < MAX && g_e[k].used; k++) {}
        if (k == MAX) return;
        void* ref = game::PlaceObject(npc, sp.x, sp.y, sp.z);
        if (!ref) return;
        game::SetHeading(ref, sp.rot);
        Entry& e = g_e[k];
        e = Entry{};
        e.used = e.assigned = e.spawned = e.seen = true;
        e.ref = ref; e.formId = game::RefFormId(ref); e.sid = sp.sid; e.epoch = sp.epoch; e.owner = sp.owner;
        e.mine = sp.owner == me; e.killDue = sp.dead; e.lastHealth = -1.0f; e.bornMs = now;
        Publish(k);
        Debug("actors: копия временного актёра sid %u (%08x) -> %08x", sp.sid, sp.pick ? sp.pick : sp.base, e.formId);
    } else if (type == proto::ACT_REMOVE && size >= 4) {
        u32 sid; memcpy(&sid, p, 4);
        int k = BySid(sid);
        if (k >= 0 && g_e[k].spawned) Drop(k, false);
    } else if (type == proto::ACT_PROJ && size == sizeof(proto::ProjMsg)) {
        proto::ProjMsg pm; memcpy(&pm, p, sizeof pm);
        void* shooter = nullptr;
        if (pm.sid & proto::ACT_PLAYER) shooter = game::PuppetRef((u16)(pm.sid & 0xFFFF));
        else if (int k = BySid(pm.sid); k >= 0 && !g_e[k].mine) shooter = g_e[k].ref;
        if (shooter && Has3D(shooter)) projectiles::Received(pm, shooter);
    } else if (type == proto::ACT_EQUIP && size >= 8) {
        u32 sid; u16 cnt; memcpy(&sid, p, 4); memcpy(&cnt, p + 4, 2);
        int k = BySid(sid);
        if (k < 0 || g_e[k].mine || cnt > equip::MAX_ITEMS || size != 8 + cnt * sizeof(equip::Item)) return;
        memcpy(g_e[k].equipWant, p + 8, cnt * sizeof(equip::Item));
        g_e[k].equipN = cnt; g_e[k].equipDue = true;
    } else if (type == proto::ACT_HEALTH && size == sizeof(proto::ActHealth)) {
        proto::ActHealth hm; memcpy(&hm, p, sizeof hm);
        int k = BySid(hm.sid);
        if (k < 0 || !g_e[k].mine || !Has3D(g_e[k].ref) || hm.value <= 0 || hm.value > 100000) return;
        Damage(g_e[k].ref, hm.value);
        Debug("actors: %08x урон от другого игрока %.0f", g_e[k].formId, hm.value);
    } else if (type == proto::ACT_DEATH && size == sizeof(proto::ActDeath)) {
        proto::ActDeath d; memcpy(&d, p, sizeof d);
        int k = BySid(d.sid);
        if (k < 0 || g_e[k].mine) return;
        if (d.dead && !game::IsDeadActor(g_e[k].ref)) game::KillActor(g_e[k].formId);
    }
}

/* ---- what we have loaded */
void Scan(u64 now) {
    void* player = game::Player();
    void* list[256];
    int n = game::HighActors(list, 256);
    for (auto& e : g_e) e.seen = false;
    for (int i = 0; i < n; i++) {
        auto ref = static_cast<u8*>(list[i]);
        if (ref == player || ref[0x1A] != 0x3E || !Has3D(ref) || game::PuppetOwner(ref)) continue;
        u32 id = game::RefFormId(ref);
        if (!id) continue;
        int k = ByRef(ref);
        if (k >= 0) { g_e[k].seen = true; continue; }
        bool temp = (id >> 24) == 0xFF;
        u32 pick = LeveledPick(ref), base = PlainNpc(At<void*>(ref, 0x40));
        if (temp && !pick && !base) continue;                         // nothing the others could make it from
        for (k = 0; k < MAX && g_e[k].used; k++) {}
        if (k == MAX) break;
        Entry& e = g_e[k];
        e = Entry{};
        e.used = e.seen = true; e.ref = ref; e.formId = id; e.cookie = ++g_cookie; e.lastHealth = -1.0f;
        proto::ActAssign a{};
        a.cookie = e.cookie; a.ref = id; a.temp = temp; a.base = base; a.pick = pick;
        game::PlaceOf(ref, &a.cell, &a.world);
        const float* p = game::RefPos(ref);
        a.x = p[0]; a.y = p[1]; a.z = p[2]; a.rot = game::Heading(ref);
        a.dead = game::IsDeadActor(ref);
        Send(proto::ACT_ASSIGN, &a, sizeof a);
    }
    for (int k = 0; k < MAX; k++) {
        Entry& e = g_e[k];
        if (!e.used) continue;
        if (e.spawned && !Has3D(e.ref) && now - e.bornMs < 15000 && equip::Lookup(e.formId) == e.ref) continue;   // its 3D is coming
        if (!e.seen || equip::Lookup(e.formId) != e.ref || !Has3D(e.ref)) Drop(k, true);
    }
    for (int k = MAX; k < MAX + PLAYERS; k++) {                       // players: no rows for 5 s, the puppet is game.cpp's
        Entry& e = g_e[k];
        if (!e.used) continue;
        e.ref = EntryRef(e);
        if (now - e.lastRowMs > 5000) Drop(k, false);
        else Publish(k);
    }
}

void SendMoves(u64 now) {
    alignas(8) u8 buf[sizeof(proto::ActMovesHead) + proto::ACT_ROWS * sizeof(proto::ActMove)];
    proto::ActMovesHead h{(u32)now, 0, 0};
    auto flush = [&]() {
        if (!h.count) return;
        memcpy(buf, &h, sizeof h);
        Send(proto::ACT_MOVES, buf, sizeof h + h.count * sizeof(proto::ActMove));
        h.count = 0;
    };
    auto row = [&](void* ref, u32 sid, bool health) {
        proto::ActMove m;
        Describe(ref, sid, health, &m);
        memcpy(buf + sizeof h + h.count * sizeof m, &m, sizeof m);
        if (++h.count == proto::ACT_ROWS) flush();
    };
    if (void* player = game::Player()) row(player, proto::ACT_PLAYER | MyId(), false);
    for (int k = 0; k < MAX; k++) {
        Entry& e = g_e[k];
        if (!e.used || !e.assigned || !e.mine || !Has3D(e.ref)) continue;
        row(e.ref, e.sid, true);
        if (now - e.equipCheckMs > 2000) {                     // what it wears, when that changed
            e.equipCheckMs = now;
            const equip::Item* items; int n;
            if (equip::Worn(e.ref, &items, &n)) {
                u32 hsh = HashItems(items, n);
                if (hsh != e.equipHash) {
                    e.equipHash = hsh;
                    alignas(8) u8 eb[8 + sizeof(equip::Item) * equip::MAX_ITEMS];
                    u16 cnt = (u16)(n < equip::MAX_ITEMS ? n : equip::MAX_ITEMS), pad = 0;
                    memcpy(eb, &e.sid, 4); memcpy(eb + 4, &cnt, 2); memcpy(eb + 6, &pad, 2);
                    memcpy(eb + 8, items, cnt * sizeof(equip::Item));
                    Send(proto::ACT_EQUIP, eb, (u16)(8 + cnt * sizeof(equip::Item)));
                }
            }
        }
        bool dead = game::IsDeadActor(e.ref);
        if (dead != e.deadSent) {
            e.deadSent = dead;
            proto::ActDeath d{e.sid, (u8)dead, 0, 0};
            Send(proto::ACT_DEATH, &d, sizeof d);
        }
    }
    flush();
}

/* remote actors' health: ours hit here goes to the owner; the owner's value wins */
void SyncHealth(u64 now) {
    for (int k = 0; k < MAX; k++) {
        Entry& e = g_e[k];
        if (!e.used || !e.assigned || e.mine || !Has3D(e.ref)) continue;
        float cur = Health(e.ref);
        if (e.lastHealth >= 0 && cur < e.lastHealth - 0.5f) {
            proto::ActHealth m{e.sid, e.lastHealth - cur};
            Send(proto::ACT_HEALTH, &m, sizeof m);
            e.localHitMs = now;
        }
        if (e.n) {
            const auto& m = e.buf[e.n - 1].m;
            float want = m.health;
            if (cur > want + 1.0f && !(m.has >> 31)) Damage(e.ref, cur - want);
            else if (cur < want - 1.0f && now - e.localHitMs > 1500 && !game::IsDeadActor(e.ref)) Restore(e.ref, want - cur);
        }
        e.lastHealth = Health(e.ref);
        if (e.n) {                                         // magicka and stamina: the owner's value, no rules
            const auto& m = e.buf[e.n - 1].m;
            FixedString* fs[2] = {&g_fsMagicka, &g_fsStamina};
            float want[2] = {m.magicka, m.stamina};
            for (int j = 0; j < 2; j++) {
                float c = Av(e.ref, fs[j]);
                if (c > want[j] + 1.0f) Damage(e.ref, c - want[j], fs[j]);
                else if (c < want[j] - 1.0f) Restore(e.ref, want[j] - c, fs[j]);
                else continue;
                if (now - e.avLogMs > 5000) {
                    e.avLogMs = now;
                    Debug("actors: %08x %s %.0f -> %.0f (так у владельца)", e.formId, j ? "запас сил" : "магия", c, want[j]);
                }
            }
        }
    }
}

void Fixed(FixedString* fs, const char* s) { reinterpret_cast<void (*)(FixedString*, const char*)>(g_fsCtor)(fs, s); }

}  // namespace

bool Resolve() {
    uintptr_t update = game::Checked177(UPDATE.off, UPDATE.sig);
    g_setBool = game::Checked177(SET_BOOL.off, SET_BOOL.sig);
    g_setInt = game::Checked177(SET_INT.off, SET_INT.sig);
    g_setFloat = game::Checked177(SET_FLOAT.off, SET_FLOAT.sig);
    g_fsCtor = game::Checked177(FS_CTOR.off, FS_CTOR.sig);
    g_getAv = game::Checked177(GET_AV.off, GET_AV.sig);
    g_damageAv = game::Checked177(DAMAGE_AV.off, DAMAGE_AV.sig);
    g_restoreAv = game::Checked177(RESTORE_AV.off, RESTORE_AV.sig);
    g_ok = update && g_setBool && g_setInt && g_setFloat && g_fsCtor && g_getAv && g_damageAv && g_restoreAv;
    if (g_ok) UpdateHook::InstallAtPtr(update);
    if (uintptr_t m = game::Checked177(MOD_AV.off, MOD_AV.sig)) { if (g_ok) ModAvHook::InstallAtPtr(m); }
    if (uintptr_t m = game::Checked177(EQUIP_OBJECT.off, EQUIP_OBJECT.sig)) { if (g_ok) EquipHook::InstallAtPtr(m); }
    else Log("ядро актёров: изменение значений актёра не найдено (правила урона выключены)");
    Log("ядро актёров: %s (update %lx bool %lx int %lx float %lx av %lx/%lx/%lx)", g_ok ? "есть" : "НЕТ", update, g_setBool,
        g_setInt, g_setFloat, g_getAv, g_damageAv, g_restoreAv);
    return g_ok;
}

bool IsRemote(const void* actor) {
    if (!actor || !g_live) return false;
    for (int k = 0; k < MAX + PLAYERS; k++) if (g_remoteRef[k] == actor) return true;
    return false;
}

bool OwnedSid(const void* actor, u32* sid) {
    if (!actor || !g_live) return false;
    for (int k = 0; k < MAX; k++)
        if (g_ownedRef[k] == actor) { *sid = g_ownedSid[k]; return *sid != 0; }
    return false;
}

void SendAction(u32 sid, const proto::ActionMsg& m) {
    QLock();
    if (g_qaHead - g_qaTail < 64) { g_qa[g_qaHead % 64] = QAct{sid, m}; g_qaHead++; }   // full: dropped
    QUnlock();
}

bool DrivesPuppet(const void* ref) {
    if (!ref || !g_live) return false;
    for (int k = MAX; k < MAX + PLAYERS; k++) if (g_remoteRef[k] == ref) return g_e[k].n > 0;
    return false;
}

int g_tr;
#define TR(n) do { if (g_tr > 0) Log("трасса ядра: %s", n); } while (0)

void Pump() {
    if (!g_ok) return;
    if (g_tr > 0) g_tr--;
    g_pumpThread = nn::os::GetCurrentThread();
    u64 now = NowMs();
    bool on = Status() == NetStatus::Connected && game::Player() && quests::IntroDone();
    /* packets in (also while off: a reset must not be missed) */
    static u8 buf[2048];
    u16 type, size;
    for (int i = 0; i < 64 && TakeActorPacket(&type, buf, &size); i++) {
        if (g_tr > 0) Log("трасса ядра: пакет %u (%u байт)", type, size);
        if (on || type == proto::ACT_RESET) Handle(type, buf, size, now);
    }
    if (!on) {
        if (g_live) { DropAll(Status() == NetStatus::Connected); g_live = false; Debug("actors: выключено"); }
        return;
    }
    if (!g_fsHealth.data) {
        Fixed(&g_fsHealth, "Health");
        Fixed(&g_fsMagicka, "Magicka");
        Fixed(&g_fsStamina, "Stamina");
        for (int i = 0; i < proto::ACT_FLOATS; i++) Fixed(&g_fsF[i], FLOATS[i]);
        for (int i = 0; i < proto::ACT_INTS; i++) Fixed(&g_fsI[i], INTS[i]);
        for (int i = 0; i < 32; i++) Fixed(&g_fsB[i], BOOLS[i]);
        for (int i = 0; i < proto::ACT_FLOATS; i++) Fixed(&g_cF[i], C_FLOATS[i]);
        for (int i = 0; i < proto::ACT_INTS; i++) Fixed(&g_cI[i], C_INTS[i]);
        for (int i = 0; i < 32; i++) Fixed(&g_cB[i], C_BOOLS[i]);
    }
    (void)g_tr;
    g_live = true;
    TR("actions out");
    projectiles::Pump();                                    // our launches out
    /* owned actors' actions out */
    for (int i = 0; i < 64; i++) {
        QLock();
        bool any = g_qaTail != g_qaHead;
        QAct q = any ? g_qa[g_qaTail++ % 64] : QAct{};
        QUnlock();
        if (!any) break;
        proto::ActAction a{q.sid, q.a};
        Send(proto::ACT_ACTION, &a, sizeof a);
    }
    static u64 nextScan, nextSend, nextHealth, nextLog;
    TR("scan");
    if (now >= nextScan) { nextScan = now + SCAN_MS; Scan(now); }
    TR("send");
    if (now >= nextSend) { nextSend = now + SEND_MS; SendMoves(now); }
    TR("health");
    if (now >= nextHealth) { nextHealth = now + 200; SyncHealth(now); }
    TR("apply");
    /* remote actors the Update hook did not reach this frame (another thread updates them): here, after it */
    for (int k = 0; k < MAX + PLAYERS; k++) {
        Entry& e = g_e[k];
        if (!e.used || !(e.player || (e.assigned && !e.mine))) continue;
        void* ref = e.player ? EntryRef(e) : e.ref;
        if (e.player && ref != e.ref) { e.ref = ref; Publish(k); }
        if (!e.player && ref && Has3D(ref)) {
            if (!e.dm) { game::DontMove(ref, true); e.dm = true; }
            if (e.killDue) { e.killDue = false; if (!game::IsDeadActor(ref)) game::KillActor(e.formId); }
            if (e.equipDue && !game::IsDeadActor(ref)) {      // what its owner says it wears (objects; spells come with actions)
                e.equipDue = false;
                const equip::Item* worn; int wn;
                if (equip::Worn(ref, &worn, &wn)) {
                    equip::Item have[equip::MAX_ITEMS]; int hn = wn < equip::MAX_ITEMS ? wn : equip::MAX_ITEMS;
                    memcpy(have, worn, hn * sizeof(equip::Item));
                    g_equipOverride = true;
                    for (int w = 0; w < e.equipN; w++) {
                        if (e.equipWant[w].left >= 2) continue;
                        bool on = false;
                        for (int h = 0; h < hn && !on; h++) on = have[h].formId == e.equipWant[w].formId;
                        if (!on) if (void* f = equip::Lookup(e.equipWant[w].formId)) {
                            equip::EquipOn(ref, f);
                            Debug("actors: %08x надевает %08x (так у владельца)", e.formId, e.equipWant[w].formId);
                        }
                    }
                    g_equipOverride = false;
                }
            }
        }
        if (ref && now - e.hookMs > 60) Apply(e, ref, now);
    }
    if (now >= nextLog) {
        nextLog = now + 10000;
        int own = 0, rem = 0, pl = 0;
        for (int k = 0; k < MAX + PLAYERS; k++) {
            if (!g_e[k].used) continue;
            if (g_e[k].player) pl++; else if (g_e[k].assigned && g_e[k].mine) own++; else if (g_e[k].assigned) rem++;
        }
        Debug("actors: наших %d, удалённых %d, игроков %d; Update на нашем потоке %lu, на других %lu", own, rem, pl,
              g_hookOn, g_hookOff);
    }
}

void EquipOverride(bool on) { g_equipOverride = on; }

}  // namespace coop::actors
