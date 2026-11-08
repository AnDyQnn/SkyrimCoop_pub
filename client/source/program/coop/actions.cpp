#include "actions.hpp"
#include "quests.hpp"
#include "mount.hpp"
#include "client.hpp"
#include "equip.hpp"
#include "game.hpp"
#include "lib.hpp"
#include "npcsync.hpp"
#include "actors.hpp"
#include <cstring>

/* Actions the Skyrim Together way (Code/client/Games/Animation.cpp, Systems/AnimationSystem.cpp):
   the local player's ActorMediator::PerformAction(TESActionData*) is hooked and every successful action
   (attack, block, draw, sneak, jump, shout, idle...) goes out with its BGSAction, target, event name, idle and
   the actor state flags; another console rebuilds the TESActionData for the puppet and forces it
   (PerformComplexAction + ApplyAnimationVariables), bypassing the AI's own checks.
   1.1.177 (tools/rtti.py + xrefs.py):
     PerformAction 0x67d7bc(mediator = *GOT 0x21522b0, data); inside it the complex action 0x68138c(*GOT 0x21522c8,
     data) and the variables 0x52fd18(*GOT 0x2147a50, data) -> ForceAction
     TESActionData (0x60): vtable (*GOT 0x2146340 + 0x10), actor +8, target +0x10, BGSAction +0x18, input +0x20,
       ActionOutput at +0x28: eventName +0x28, targetEventName +0x30, result +0x38, idle +0x48; flags +0x54
     ActionInput ctor 0xda9ac(this, input, actor, action, target), ActionOutput ctor 0x12724c(this + 0x28),
     complete dtor 0x157504
     actor state flags: actor + 0xF8 / + 0xFC (the IsWeaponDrawn native reads + 0xFC) */
namespace coop::actions {
namespace {

struct Fn { u32 off; u32 sig[4]; };
constexpr Fn PERFORM{0x67d7bc, {0xA9BD7BFD, 0xA90157F6, 0x910003FD, 0xA9024FF4}};
constexpr Fn COMPLEX{0x68138c, {0xD100C3FF, 0xA9017BFD, 0x910043FD, 0xA9024FF4}};
constexpr Fn VARIABLES{0x52fd18, {0xD10583FF, 0xA9117BFD, 0x910443FD, 0xF90093FC}};
constexpr Fn INPUT_CTOR{0xda9ac, {0xF9000402, 0xF0010348, 0xF9465908, 0x91004108}};
constexpr Fn OUTPUT_CTOR{0x12724c, {0xA9BE7BFD, 0xA9014FF4, 0x910003FD, 0xB00100F4}};
constexpr Fn DTOR{0x157504, {0xA9BE7BFD, 0xA9014FF4, 0x910003FD, 0xAA0003F3}};
constexpr Fn FS_CTOR{0xae6da8, {0xA9BE7BFD, 0xF9000BF3, 0x910003FD, 0xF900001F}};
constexpr u32 FS_DTOR = 0xae6ab0;
constexpr u32 COMPLEX_GOT = 0x21522c8, VARIABLES_GOT = 0x2147a50, DATA_VTABLE_GOT = 0x2146340;
constexpr u32 STATE1 = 0xF8, STATE2 = 0xFC;

uintptr_t g_perform, g_complex, g_variables, g_inputCtor, g_outputCtor, g_dtor, g_fsCtor, g_fsDtor;
bool g_ok;
volatile bool g_applying;                 // our own forced actions are not sent back out

template <class T> T& At(void* p, u32 off) { return *reinterpret_cast<T*>(static_cast<u8*>(p) + off); }
u32 IdOf(void* form) { return form ? At<u32>(form, 0x14) : 0; }

HOOK_DEFINE_TRAMPOLINE(PerformHook) {
    static u8 Callback(void* mediator, u8* data) {
        __atomic_add_fetch(&g_hookIn[1], 1u, __ATOMIC_RELAXED);
        u8 r = Body(mediator, data);
        __atomic_add_fetch(&g_hookOut[1], 1u, __ATOMIC_RELAXED);
        return r;
    }
    static u8 Body(void* mediator, u8* data) {
        void* actor = At<void*>(data, 0x08);
        /* (08.10, the actor core) a remote actor does nothing of its own here: only its owner's actions, replayed */
        if (!g_applying && actor && actors::IsRemote(actor)) return 0;
        u8 r = Orig(mediator, data);
        if (actor && actor != game::Player()) npcsync::Noticed(IdOf(actor));
        if (r && actor) {                              // riding: the game's own getting on / off (mount.cpp)
            const char* nm = At<const char*>(data, 0x28);
            if (nm && (!strcmp(nm, "HorseEnter") || !strcmp(nm, "HorseExit"))) mount::ActorAction(actor, nm);
        }
        if (actor == game::Player()) {
            static int n;
            if (n++ < 40) Debug("player action %08x r=%u flags=%x name=%s", IdOf(At<void*>(data, 0x18)), r, data[0x54],
                                At<const char*>(data, 0x28) ? At<const char*>(data, 0x28) : "-");
        }
        u32 sid;
        if (r && !g_applying && actor && actor != game::Player() && !(data[0x54] & 1) && IdOf(At<void*>(data, 0x18))
            && actors::OwnedSid(actor, &sid)) {
            proto::ActionMsg a{};                      // an actor we own: its actions are the real ones
            a.type = static_cast<u8>(At<u32>(data, 0x20) & 3);
            a.action = IdOf(At<void*>(data, 0x18));
            a.target = IdOf(At<void*>(data, 0x10));
            a.idle = IdOf(At<void*>(data, 0x48));
            a.state1 = At<u32>(actor, STATE1);
            a.state2 = At<u32>(actor, STATE2);
            const char* nm = At<const char*>(data, 0x28);
            if (nm) { u32 l = strlen(nm); if (l >= sizeof a.name) l = sizeof a.name - 1; memcpy(a.name, nm, l); a.len = static_cast<u8>(l); }
            actors::SendAction(sid, a);
        }
        if (!r || g_applying || !actor || actor != game::Player()) return r;
        if (data[0x54] & 1) return r;     // Skyrim Together: the spammed "someFlag" actions do nothing useful
        proto::ActionMsg m{};
        m.type = static_cast<u8>(At<u32>(data, 0x20) & 3);
        m.action = IdOf(At<void*>(data, 0x18));
        m.target = IdOf(At<void*>(data, 0x10));
        m.idle = IdOf(At<void*>(data, 0x48));
        m.state1 = At<u32>(actor, STATE1);
        m.state2 = At<u32>(actor, STATE2);
        const char* name = At<const char*>(data, 0x28);
        if (name) {
            u32 n = strlen(name);
            if (n >= sizeof m.name) n = sizeof m.name - 1;
            memcpy(m.name, name, n);
            m.len = static_cast<u8>(n);
        }
        /* attacks, blocks, jumps... also reach the graph and go out as EVENTs (game.cpp): not twice */
        bool viaEvent = false;
        static const char* const EV[] = {"attack", "Attack", "bash", "block", "Block", "bow", "Bow", "jump", "Jump",
                                         "BeginCast", "MRh_", "MLh_", "Dual", "shout", "Shout", "Voice", "Sneak", "sneak",
                                         "Sprint", "sprint", "Weap", "weap", "Unequip"};
        for (const char* e : EV) viaEvent |= m.len && !strncmp(m.name, e, strlen(e));
        if (m.action && !viaEvent) PushAction(m);
        return r;
    }
};

}  // namespace

bool Resolve() {
    g_perform = game::Checked177(PERFORM.off, PERFORM.sig);
    g_complex = game::Checked177(COMPLEX.off, COMPLEX.sig);
    g_variables = game::Checked177(VARIABLES.off, VARIABLES.sig);
    g_inputCtor = game::Checked177(INPUT_CTOR.off, INPUT_CTOR.sig);
    g_outputCtor = game::Checked177(OUTPUT_CTOR.off, OUTPUT_CTOR.sig);
    g_dtor = game::Checked177(DTOR.off, DTOR.sig);
    g_fsCtor = game::Checked177(FS_CTOR.off, FS_CTOR.sig);
    g_fsDtor = g_fsCtor ? game::ModuleAddr(FS_DTOR) : 0;
    g_ok = g_perform && g_complex && g_variables && g_inputCtor && g_outputCtor && g_dtor && g_fsCtor;
    if (g_ok) PerformHook::InstallAtPtr(g_perform);
    return g_ok;
}

bool Available() { return g_ok; }

/* an action forced on an actor: a player's on its puppet, a zone owner's NPC's on our copy */
u8 Replay(void* puppet, const proto::ActionMsg& m) {
        void* action = equip::Lookup(m.action);
        if (!puppet || !action) return 0;
        void* target = m.target ? equip::Lookup(m.target) : nullptr;
        if (target == game::Player() || (target && static_cast<u8*>(target)[0x1A] != 0x3E && static_cast<u8*>(target)[0x1A] != 0x3D))
            target = nullptr;                              // their target on our side: only real references
        alignas(16) u8 d[0x60] = {};
        reinterpret_cast<void (*)(void*, u32, void*, void*, void*)>(g_inputCtor)(d, m.type & 3u, puppet, action, target);
        reinterpret_cast<void (*)(void*)>(g_outputCtor)(d + 0x28);
        At<uintptr_t>(d, 0) = *reinterpret_cast<uintptr_t*>(game::ModuleAddr(DATA_VTABLE_GOT)) + 0x10;
        if (m.len) {
            char name[sizeof m.name + 1] = {};
            memcpy(name, m.name, m.len < sizeof m.name ? m.len : sizeof m.name);
            reinterpret_cast<void (*)(void*)>(g_fsDtor)(d + 0x28);
            reinterpret_cast<void (*)(void*, const char*)>(g_fsCtor)(d + 0x28, name);
        }
        At<void*>(d, 0x48) = m.idle ? equip::Lookup(m.idle) : nullptr;
        At<u32>(puppet, STATE1) = m.state1;
        At<u32>(puppet, STATE2) = m.state2;
        g_applying = true;
        void* complexObj = game::Singleton(game::ModuleAddr(COMPLEX_GOT));
        void* variablesObj = game::Singleton(game::ModuleAddr(VARIABLES_GOT));
        u8 r = 0;
        if (complexObj) r = reinterpret_cast<u8 (*)(void*, void*)>(g_complex)(complexObj, d);
        if (variablesObj) reinterpret_cast<void (*)(void*, void*)>(g_variables)(variablesObj, d);
        g_applying = false;
        reinterpret_cast<void (*)(void*)>(g_dtor)(d);
        return r;
}

/* game thread (VM pump): the other players' actions on their puppets, the zone owner's NPC actions on ours */
void Pump() {
    if (!g_ok) return;
    u16 id;
    proto::ActionMsg m;
    for (int i = 0; i < 16 && TakeAction(&id, &m); i++) {
        u8 r = Replay(game::PuppetRef(id), m);
        Debug("puppet %u action %08x '%s' -> %u", id, m.action, m.len ? m.name : "", r);
    }
    proto::NpcActionMsg n;
    for (int i = 0; i < 16 && TakeNpcAction(&n); i++) {
        if (ZoneOwner() == MyId() || !quests::IntroDone()) continue;   // ours are the real ones (and in our intro: ours alone)
        auto ref = static_cast<u8*>(equip::Lookup(n.formId));
        if (!ref || ref[0x1A] != 0x3E || !*reinterpret_cast<void**>(ref + 0x68)) continue;
        u8 r = Replay(ref, n.a);
        static int logged;
        if (logged++ < 40) Debug("npc %08x action %08x '%s' -> %u", n.formId, n.a.action, n.a.len ? n.a.name : "", r);
    }
}

}  // namespace coop::actions
