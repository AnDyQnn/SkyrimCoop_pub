#include "quests.hpp"
#include "client.hpp"
#include "equip.hpp"
#include "game.hpp"
#include "lib.hpp"
#include <nn/os.hpp>
#include <cstdio>

/* Shared quest progress (a server setting, off by default). A quest stage set here (TESQuest::SetStage,
   1.1.177: 0x28e0e8(quest, stage) -> bool, used by the SetCurrentStageID native and the quest scripts) goes
   to the server; with shared quests on, the other consoles set the same stage on their copy of the quest
   when theirs is lower. Current stage: u16 at quest +0x1B8 (GetCurrentStageID native -> 0x28e1c8). */
namespace coop::quests {
namespace {

struct Fn { u32 off; u32 sig[4]; };
constexpr Fn SET_STAGE{0x28e0e8, {0xA9BE7BFD, 0xA9014FF4, 0x910003FD, 0x39431008}};
/* The start the SetCurrentStageID native does first for a quest that is not running: (quest, bool& queued,
   bool) -> false = cannot start; queued = the start waits for its aliases, the stage has to wait too. */
constexpr Fn ENSURE_STARTED{0x28df8c, {0xD100C3FF, 0xA9017BFD, 0x910043FD, 0xA9024FF4}};
constexpr u32 CURRENT_STAGE = 0x1B8;
constexpr u32 FLAGS = 0xC4;                  // bit 0 running, bit 1 completed
constexpr u32 MQ101 = 0x0003372B;            // "Unbound" — the intro (cart, Helgen); done when the player leaves the cave

volatile bool g_applying;
bool g_heldLogged;
bool g_ok;
uintptr_t g_setStage, g_ensureStarted;
volatile u32 g_localQuest;
volatile u16 g_localStage;

struct Stage { u32 quest; u16 stage; };
struct Waiting { u32 quest; u16 stage, tries; };
Waiting g_wait[32];                                   // game thread only: quests whose start is still queued
int g_waitN;
Stage g_in[1024];                            // a clan with 100+ hours played: hundreds of quests at once
volatile int g_inHead, g_inTail;

HOOK_DEFINE_TRAMPOLINE(SetStageHook) {
    static bool Callback(void* quest, u16 stage) {
        bool r = Orig(quest, stage);
        if (r && !g_applying && quest) {
            u32 id = *reinterpret_cast<u32*>(static_cast<u8*>(quest) + 0x14);
            if ((id >> 24) != 0xFF) PushQuest(id, stage);
        }
        return r;
    }
};

/* Another clan member's stage on our copy of the quest: started first when it is not running (like the
   SetCurrentStageID native), so the stage fragments — objectives, rewards — run here too. */
void Apply(u32 id, u16 stage) {
    auto quest = static_cast<u8*>(equip::Lookup(id));
    if (!quest || quest[0x1A] != 0x4D) return;                           // formType QUST
    u16 cur = *reinterpret_cast<u16*>(quest + CURRENT_STAGE);
    if (cur >= stage) return;
    if (!(quest[FLAGS] & 1)) {
        bool queued = false;
        bool started = g_ensureStarted && reinterpret_cast<bool (*)(void*, bool*, bool)>(g_ensureStarted)(quest, &queued, false);
        if (!started) { Debug("quest %08x: cannot start (stage %u, flags %x)", id, cur, quest[FLAGS]); return; }
        if (queued || !(quest[FLAGS] & 1)) {
            for (int i = 0; i < g_waitN; i++)
                if (g_wait[i].quest == id) { if (g_wait[i].stage < stage) g_wait[i].stage = stage; return; }
            if (g_waitN < 32) g_wait[g_waitN++] = Waiting{id, stage, 0};
            Debug("quest %08x: starting, stage %u waits", id, stage);
            return;
        }
    }
    g_applying = true;
    bool ok = SetStageHook::Orig(quest, stage);
    g_applying = false;
    Debug("quest %08x: stage %u -> %u (%s)", id, cur, stage, ok ? "ok" : "refused");
}

}  // namespace

bool Resolve() {
    uintptr_t a = game::Checked177(SET_STAGE.off, SET_STAGE.sig);
    g_setStage = a;
    g_ensureStarted = game::Checked177(ENSURE_STARTED.off, ENSURE_STARTED.sig);
    if (a) SetStageHook::InstallAtPtr(a);
    g_ok = a != 0;
    return g_ok;
}

void Received(u32 questId, u16 stage) {
    int next = (g_inHead + 1) % 1024;
    if (next == g_inTail) return;
    g_in[g_inHead] = Stage{questId, stage};
    g_inHead = next;
}

bool IntroDone() {
    auto intro = static_cast<u8*>(equip::Lookup(MQ101));
    return !intro || intro[0x1A] != 0x4D || (intro[FLAGS] & 2) || *reinterpret_cast<u16*>(intro + CURRENT_STAGE) >= 1000;
}

bool SetStage(void* quest, u16 stage) {
    return g_setStage && quest && reinterpret_cast<bool (*)(void*, u16)>(g_setStage)(quest, stage);
}

void SetLocal(u32 questId, u16 stage) { g_localStage = stage; g_localQuest = questId; }

/* test aid: running quests with a small stage (candidates for a stage test) */
void Scan() {
    int n = 0;
    for (u32 id = 0x10000; id < 0x110000 && n < 12; id++) {
        auto q = static_cast<u8*>(equip::Lookup(id));
        if (!q || q[0x1A] != 0x4D) continue;
        u16 cur = *reinterpret_cast<u16*>(q + CURRENT_STAGE);
        if ((q[0xC4] & 1) && cur > 0 && cur < 60) { Debug("running quest %08x stage %u flags %x", id, cur, q[0xC4]); n++; }
    }
}

void Pump() {
    if (!g_ok) return;
    /* A new character: once out of Helgen, tell how to get to the others (the spawn point of a co-op game) */
    {
        static bool inIntro;
        static u64 nextCheck;
        u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
        if (now > nextCheck) {
            nextCheck = now + 3000;
            bool done = IntroDone();
            if (!done) inIntro = true;
            else if (inIntro) {
                inIntro = false;
                game::Notify("Кооп: вступление пройдено! L3+R3 → Кампания → «В дом кампании» или Игроки → к игроку");
                game::Notify("Кооп: есть герой на сервере? Сохранения → «Перенести персонажа сюда»");
            }
        }
    }
    if (u32 q = __atomic_exchange_n(&g_localQuest, 0u, __ATOMIC_ACQ_REL)) {
        if (q == 1) { Scan(); return; }
        auto quest = static_cast<u8*>(equip::Lookup(q));
        u16 cur = quest ? *reinterpret_cast<u16*>(quest + CURRENT_STAGE) : 0;
        u8 flags = quest ? quest[0xC4] : 0;
        bool ok = quest && g_localStage != 0xFFFF &&
                  reinterpret_cast<bool (*)(void*, u16)>(g_setStage)(quest, g_localStage);   // through the hook
        Debug("quest %08x (type %02x, stage %u, flags %x): set %u here (%s)", q, quest ? quest[0x1A] : 0, cur, flags,
              g_localStage, ok ? "ok" : "refused");
    }
    /* A new character (the intro still running) keeps the others' stages until it has left Helgen: then the
       clan's quests — and their rewards — come at once. The intro itself is never pushed on anybody. */
    if (g_inTail != g_inHead && !IntroDone()) {
        {
            if (!g_heldLogged) { g_heldLogged = true; Debug("quests: intro not finished, clan stages held"); }
            return;
        }
    }
    for (int i = 0; i < g_waitN;) {                  // started meanwhile? (or given up after ~30 s)
        auto quest = static_cast<u8*>(equip::Lookup(g_wait[i].quest));
        bool running = quest && (quest[FLAGS] & 1);
        if (running || ++g_wait[i].tries > 1800) {
            if (running) Apply(g_wait[i].quest, g_wait[i].stage);
            else Debug("quest %08x: never started, stage %u dropped", g_wait[i].quest, g_wait[i].stage);
            g_wait[i] = g_wait[--g_waitN];
        } else {
            i++;
        }
    }
    for (int k = 0; k < 8 && g_inTail != g_inHead; k++) {
        Stage s = g_in[g_inTail];
        g_inTail = (g_inTail + 1) % 1024;
        if (s.quest != MQ101) Apply(s.quest, s.stage);
    }
}

}  // namespace coop::quests
