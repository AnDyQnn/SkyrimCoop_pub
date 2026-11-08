#include "client.hpp"
#include "updates.hpp"
#include "world.hpp"
#include "sdk_net.hpp"
#include "lib.hpp"
#include "settings.hpp"
#include "console.hpp"
#include "appearance.hpp"
#include "equip.hpp"
#include "trade.hpp"
#include "pvp.hpp"
#include "followers.hpp"
#include "quests.hpp"
#include "chest.hpp"
#include "npcsync.hpp"
#include "game.hpp"
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <nn/os.hpp>
#include <cstring>
#include <nn/fs.hpp>

namespace nn::fs {                    // present in the game, missing from the headers (as in settings.cpp)
    Result CommitSaveData(const char* mount);
    Result DeleteFile(const char* path);
    Result GetEntryType(DirectoryEntryType* type, const char* path);
}

#pragma GCC diagnostic ignored "-Wformat-truncation"   // truncating chat and log lines is fine

namespace coop {
namespace {

using namespace proto;

char g_name[32];
volatile bool g_online = false;    // (04.10) the network only when the player chooses «Играть по сети» this session — a plain game never connects
struct NameEntry { u16 id; char name[32]; };
NameEntry g_names[16];
InjectedInput g_input{};
char g_pendingCmd[200];
constexpr int LOG_LINES = 128;
char g_logRing[LOG_LINES][200];
int g_logHead, g_logCount;

alignas(8) u8 g_appearance[sizeof(AppearanceHead) + APPEARANCE_MAX];
u32 g_appearanceSize;               // 0 = nothing yet
volatile u32 g_appearanceVersion;
alignas(8) u8 g_equipment[sizeof(EquipHead) + 8 * 64];
u32 g_equipmentSize;
volatile u32 g_equipmentVersion;

constexpr int MSG_LINES = 6;
char g_msgRing[MSG_LINES][160];
int g_msgHead, g_msgCount;
char g_chatOut[160];
volatile bool g_chatPending;

volatile float g_timeOut = -1, g_timeIn = -1;
StatsMsg g_statsOut;
volatile bool g_statsDirty;
struct StatsIn { u16 id; StatsMsg m; };
StatsIn g_statsIn[16];
constexpr int ACT_LINES = 32;
ActionMsg g_actOut[ACT_LINES], g_actIn[ACT_LINES];
int g_actOutHead, g_actOutCount, g_actInHead, g_actInCount;
char g_plugins[3000];
volatile bool g_pluginsReady;
constexpr int DMG_LINES = 32;
DamageMsg g_dOut[DMG_LINES];
int g_dHead, g_dCount;
constexpr int QUEST_LINES = 32;
QuestMsg g_qOut[QUEST_LINES];
int g_qHead, g_qCount;
constexpr int FOLLOW_LINES = 16;
FollowMsg g_folOut[FOLLOW_LINES];
int g_folHead, g_folCount;
constexpr int HIT_LINES = 16;
HitMsg g_hitOut[HIT_LINES];
int g_hitHead, g_hitCount;
constexpr int GIVE_LINES = 32;
GiveMsg g_giveOut[GIVE_LINES];
int g_giveHead, g_giveCount;
constexpr int KILL_LINES = 64;
u32 g_killOut[KILL_LINES], g_killIn[KILL_LINES];
u16 g_killOutFlags[KILL_LINES], g_killInFlags[KILL_LINES];
int g_killOutHead, g_killOutCount, g_killInHead, g_killInCount;
volatile bool g_timeDirty;

constexpr int EV_LINES = 32;
struct Ev { u16 id; char name[40]; };
Ev g_evOut[EV_LINES], g_evIn[EV_LINES];
int g_evOutHead, g_evOutCount, g_evInHead, g_evInCount;

/* ---- online saves */
constexpr const char* SAVES_DIR = "save:/Saves";
struct CloudUp { char name[96]; s64 size; s64 sent; bool active; } g_up;
char g_upDone[96];                    // newest file already on the server (this session)
s64 g_upDoneSize = -1;
char g_stableName[96];                // newest file seen at the last scan, and its size then
s64 g_stableSize = -1;
u64 g_lastScanMs;
char g_listText[1024];
char g_updText[8192], g_updPath[160], g_updLocal[200];
alignas(8) u8 g_avOut[AVATAR_MAX], g_avIn[AVATAR_MAX];
u32 g_avOutSize, g_avSent, g_avInSize;
volatile bool g_avPutActive, g_avGetWanted;
volatile int g_avState;
volatile bool g_updListWanted, g_updListReady, g_updGetWanted, g_updDone, g_updFailed;
nn::fs::FileHandle g_updFile;
bool g_updOpen;
char g_socialOut[160], g_socialIn[1600];
volatile bool g_socialWanted, g_socialReady, g_socialAsync, g_socialGoHouse, g_socialLog;
volatile bool g_listReady, g_listWanted;
char g_getName[96];
volatile bool g_getWanted, g_getDone, g_getFailed;
nn::fs::FileHandle g_getFile;
bool g_getOpen;
char g_cloudStatus[96];
alignas(8) u8 g_chunk[sizeof(UpdHead) + SAVE_CHUNK];          // the bigger header (saves, avatar)

char g_roomName[48];
char g_server[80];

nn::os::MutexType g_lock;
bool g_lockReady;

void EnsureLock() {                     // Log() can run before StartClient (resolver at boot)
    /* (08.10) recursive: a handler run under it (followers::Received) logs, and Log takes it too — with a plain mutex
       the network thread locked itself out on a reconnect and the game waited on it for ever */
    if (!g_lockReady) { nn::os::InitializeMutex(&g_lock, true, 0); g_lockReady = true; }
}
State g_local{};
bool g_localValid = false;
RemotePlayer g_remotes[MAX_REMOTE];
volatile NetStatus g_status = NetStatus::Idle;
/* 04.10, the campaign (one world on the server, from a new game): the server's number of it (WK_CAMPAIGN on join) and
   whether the loaded game is a save of it (game.cpp reads SSW_CampaignId): -1 no game loaded (title), 0 a personal
   save — connected, but nothing goes to the campaign or comes from it, 1 a campaign save, 2 loading / not known yet */
volatile u32 g_campaignId;
volatile int g_campaignState = -1;
bool g_upBaseline;                       // network thread: the newest save when the campaign game began (not uploaded)
volatile bool g_connJoined;              // this connection began with a campaign game loaded (the world came to it)
volatile bool g_welcomed;                // this connection got its WELCOME
volatile u16 g_myId = 0;
volatile u16 g_zoneOwner;
proto::NpcState g_npcOut[proto::NPC_MAX], g_npcIn[proto::NPC_MAX];
constexpr int WO_MAX = 16, WO_SIZE = 1024;
char g_tele[1400]; volatile u32 g_teleLen; volatile bool g_teleDirty;   // the latest telemetry text (TELE)
u8 g_woOut[WO_MAX][WO_SIZE]; u16 g_woLen[WO_MAX];
volatile u32 g_woHead, g_woTail;
volatile int g_npcOutN = -1, g_npcInN = -1;              // -1 = nothing new
proto::NpcActionMsg g_naOut[32], g_naIn[32];
volatile ClockMsg g_clockIn; volatile u64 g_clockAt; volatile u32 g_clockSeq;   // (08.10) the server's game clock
/* (08.10) the actor core's packets (actors.cpp), both ways: one producer and one consumer each */
constexpr int AQ_OUT = 48, AQ_IN = 96, AQ_SIZE = 2048;
struct AqSlot { u16 type, size; u8 data[AQ_SIZE]; };
AqSlot g_aqOut[AQ_OUT], g_aqIn[AQ_IN];
volatile u32 g_aqOutHead, g_aqOutTail, g_aqInHead, g_aqInTail;
void AqIn(u16 type, const void* p, u16 size) {           // network thread
    u32 h = g_aqInHead;
    if (h - __atomic_load_n(&g_aqInTail, __ATOMIC_ACQUIRE) >= AQ_IN || size > AQ_SIZE) return;
    auto& q = g_aqIn[h % AQ_IN];
    q.type = type; q.size = size;
    if (size) memcpy(q.data, p, size);
    __atomic_store_n(&g_aqInHead, h + 1, __ATOMIC_RELEASE);
}
volatile u32 g_naOutHead, g_naOutTail, g_naInHead, g_naInTail;

alignas(0x1000) u8 g_threadStack[0x8000];
volatile bool g_socketsReady = false;

/* Skyrim calls nn::socket::Initialize from nnMain; a second initialisation aborts the process,
   so the network thread waits for the game's call instead of making its own. */
HOOK_DEFINE_TRAMPOLINE(SocketInit) {
    static Result Callback(const nn::socket::Config& config) {
        Result r = Orig(config);
        g_socketsReady = true;
        return r;
    }
};
nn::os::ThreadType g_thread;

/* (08.10) never a blocking send: on a dead / never-connected socket it hung Eden's socket service, and the whole game
   with it (server restarted: connect refused but reported as done). Would-block waits up to 3 s for room, then the
   connection is given up and made again. */
bool SendAll(s32 fd, const void* data, u64 len) {
    auto p = static_cast<const u8*>(data);
    int waits = 0;
    while (len) {
        s32 n = nn::socket::Send(fd, p, len, 0);               // the socket itself is non-blocking (Eden: no send flags)
        if (n > 0) { p += n; len -= n; continue; }
        if (n == 0) return false;
        u32 e = nn::socket::GetLastErrno();
        if (e != 11 && e != 35) return false;                  // not EAGAIN / EWOULDBLOCK: the connection is gone
        if (++waits > 600) return false;                         // 3 s without room: gone (no Poll: it hung Eden)
        nn::os::SleepThread(nn::TimeSpan::FromMilliSeconds(5));
    }
    return true;
}

bool SendPacket(s32 fd, u16 type, const void* payload, u16 size) {
    Header h{type, size};
    return SendAll(fd, &h, sizeof h) && (!size || SendAll(fd, payload, size));
}

/* Download: the file is created with its final size, chunks are written at their offsets, then committed. */
void ReceiveSaveChunk(const SaveHead& h, const u8* data, u32 n) {
    if (strcmp(h.name, g_getName) || g_getDone) return;
    char path[160];
    snprintf(path, sizeof path, "%s/%s", SAVES_DIR, h.name);
    if (h.offset == 0) {
        if (g_getOpen) { nn::fs::CloseFile(g_getFile); g_getOpen = false; }
        nn::fs::DirectoryEntryType t;
        if (R_SUCCEEDED(nn::fs::GetEntryType(&t, path))) nn::fs::DeleteFile(path);
        if (R_FAILED(nn::fs::CreateFile(path, h.total)) || R_FAILED(nn::fs::OpenFile(&g_getFile, path, nn::fs::OpenMode_Write))) {
            g_getFailed = true;
            g_getDone = true;
            return;
        }
        g_getOpen = true;
    }
    if (!g_getOpen) return;
    nn::fs::WriteFile(g_getFile, h.offset, data, n, nn::fs::WriteOption::CreateOption(0));
    snprintf(g_cloudStatus, sizeof g_cloudStatus, "скачиваю %u%%", (unsigned)((u64)(h.offset + n) * 100 / (h.total ? h.total : 1)));
    if (h.offset + n >= h.total) {
        nn::fs::FlushFile(g_getFile);
        nn::fs::CloseFile(g_getFile);
        g_getOpen = false;
        g_getFailed = R_FAILED(nn::fs::CommitSaveData("save"));
        g_getDone = true;
        Logging.Log("[coop] online save downloaded: %s (%u bytes)", h.name, h.total);
    }
}

void ReceiveUpdateChunk(const UpdHead& h, const u8* data, u32 n) {
    if (strcmp(h.path, g_updPath) || g_updDone) return;
    if (h.offset == 0) {
        if (g_updOpen) { nn::fs::CloseFile(g_updFile); g_updOpen = false; }
        nn::fs::DeleteFile(g_updLocal);
        if (R_FAILED(nn::fs::CreateFile(g_updLocal, h.total)) || R_FAILED(nn::fs::OpenFile(&g_updFile, g_updLocal, nn::fs::OpenMode_Write))) {
            g_updFailed = true; g_updDone = true;
            return;
        }
        g_updOpen = true;
    }
    if (!g_updOpen) return;
    nn::fs::WriteFile(g_updFile, h.offset, data, n, nn::fs::WriteOption::CreateOption(0));
    if (h.offset + n >= h.total) {
        nn::fs::FlushFile(g_updFile);
        nn::fs::CloseFile(g_updFile);
        g_updOpen = false;
        g_updDone = true;
    }
}

/* Newest save in the game's folder (names start with the date and time). */
bool NewestSave(char* name, s64* size) {
    nn::fs::DirectoryHandle d;
    Result r = nn::fs::OpenDirectory(&d, SAVES_DIR, nn::fs::OpenDirectoryMode_File);
    static int reported;
    if (R_FAILED(r)) { if (reported++ < 3) Log("saves folder: %x", r); return false; }
    static nn::fs::DirectoryEntry entries[16];
    s64 n = 0;
    bool found = false;
    name[0] = 0;
    while (R_SUCCEEDED(nn::fs::ReadDirectory(&n, entries, d, 16)) && n > 0) {
        for (s64 i = 0; i < n; i++) {
            const char* e = entries[i].m_Name;
            u32 len = strlen(e);
            if (len < 6 || len > 95 || strcmp(e + len - 5, ".sav0")) continue;
            if (strcmp(e, name) > 0) { snprintf(name, 96, "%s", e); *size = entries[i].m_FileSize; found = true; }
        }
    }
    nn::fs::CloseDirectory(d);
    return found;
}

/* Called every loop of the connected network thread. Uploads the newest save once it has kept its size for
   a whole scan (not while the game is writing it), a few chunks per call so the game traffic goes on. */
bool SendPacket(s32 fd, u16 type, const void* payload, u16 size);
bool CloudTick(s32 fd, u64 now) {
    bool ok = true;
    if (g_listWanted) { g_listWanted = false; ok = SendPacket(fd, SAVE_LIST_REQ, nullptr, 0); }
    if (ok && g_updListWanted) { g_updListWanted = false; ok = SendPacket(fd, UPD_LIST_REQ, nullptr, 0); }
    if (ok && g_updGetWanted) { g_updGetWanted = false; ok = SendPacket(fd, UPD_GET, g_updPath, strlen(g_updPath)); }
    while (ok && g_aqOutTail != __atomic_load_n(&g_aqOutHead, __ATOMIC_ACQUIRE)) {
        auto& q = g_aqOut[g_aqOutTail % AQ_OUT];
        ok = SendPacket(fd, q.type, q.data, q.size);
        __atomic_store_n(&g_aqOutTail, g_aqOutTail + 1, __ATOMIC_RELEASE);
    }
    while (ok && g_naOutTail != __atomic_load_n(&g_naOutHead, __ATOMIC_ACQUIRE)) {
        ok = SendPacket(fd, NPC_ACTION, &g_naOut[g_naOutTail % 32], sizeof(NpcActionMsg));
        __atomic_store_n(&g_naOutTail, g_naOutTail + 1, __ATOMIC_RELEASE);
    }
    if (ok && g_teleDirty) {
        static char copy[1400];
        nn::os::LockMutex(&g_lock);
        u32 n = g_teleLen; memcpy(copy, g_tele, n); g_teleDirty = false;
        nn::os::UnlockMutex(&g_lock);
        ok = SendPacket(fd, TELE, copy, (u16)n);
        static bool said;
        if (!said) { said = true; Logging.Log("[coop] telemetry sent: %u bytes, ok %d", n, ok); }
    }
    while (ok && g_woTail != __atomic_load_n(&g_woHead, __ATOMIC_ACQUIRE)) {
        ok = SendPacket(fd, WORLD, g_woOut[g_woTail % WO_MAX], g_woLen[g_woTail % WO_MAX]);
        __atomic_store_n(&g_woTail, g_woTail + 1, __ATOMIC_RELEASE);
    }
    if (ok && __atomic_load_n(&g_npcOutN, __ATOMIC_ACQUIRE) >= 0) {
        static u8 buf[4 + sizeof(NpcState) * NPC_MAX];
        u16 n = (u16)g_npcOutN, pad = 0;
        memcpy(buf, &n, 2); memcpy(buf + 2, &pad, 2);
        memcpy(buf + 4, g_npcOut, n * sizeof(NpcState));
        __atomic_store_n(&g_npcOutN, -1, __ATOMIC_RELEASE);
        ok = SendPacket(fd, NPC_STATE, buf, 4 + n * sizeof(NpcState));
    }
    if (ok && g_avGetWanted) { g_avGetWanted = false; ok = SendPacket(fd, AVATAR_GET, nullptr, 0); }
    for (int k = 0; ok && g_avPutActive && k < 2; k++) {
        u32 n = g_avOutSize - g_avSent < SAVE_CHUNK ? g_avOutSize - g_avSent : SAVE_CHUNK;
        UpdHead h{g_avOutSize, g_avSent, "avatar"};
        memcpy(g_chunk, &h, sizeof h);
        memcpy(g_chunk + sizeof h, g_avOut + g_avSent, n);
        ok = SendPacket(fd, AVATAR_PUT, g_chunk, sizeof h + n);
        g_avSent += n;
        if (g_avSent >= g_avOutSize) g_avPutActive = false;
    }
    if (ok && g_socialWanted) { g_socialWanted = false; ok = SendPacket(fd, SOCIAL, g_socialOut, strlen(g_socialOut)); }
    if (ok && g_getWanted) { g_getWanted = false; ok = SendPacket(fd, SAVE_GET, g_getName, strlen(g_getName)); }
    static bool said;
    if (!said) { said = true; Log("online saves: save data %s", settings::SaveMounted() ? "mounted" : "not mounted"); }
    if (!settings::SaveMounted()) return ok;
    /* only saves made while a campaign game runs go to the server — a personal save never does (04.10) */
    if (g_campaignState != 1) { g_upBaseline = false; return ok; }
    if (!g_upBaseline) {
        char name[96]; s64 size = 0;
        if (NewestSave(name, &size)) {
            snprintf(g_upDone, sizeof g_upDone, "%s", name); g_upDoneSize = size;
            snprintf(g_stableName, sizeof g_stableName, "%s", name); g_stableSize = size;
        }
        g_upBaseline = true;
        g_lastScanMs = now;
    }
    if (!g_up.active && now - g_lastScanMs > 15000) {
        g_lastScanMs = now;
        char name[96]; s64 size = 0;
        if (NewestSave(name, &size)) {
            bool stable = !strcmp(name, g_stableName) && size == g_stableSize;
            bool done = !strcmp(name, g_upDone) && size == g_upDoneSize;
            if (stable && !done) {
                g_up = CloudUp{};
                snprintf(g_up.name, sizeof g_up.name, "%s", name);
                g_up.size = size;
                g_up.active = true;
            }
            if (strcmp(name, g_stableName) || size != g_stableSize) Log("newest save %s %ld", name, (long)size);
            snprintf(g_stableName, sizeof g_stableName, "%s", name);
            g_stableSize = size;
        }
    }
    for (int k = 0; ok && g_up.active && k < 4; k++) {
        char path[160];
        snprintf(path, sizeof path, "%s/%s", SAVES_DIR, g_up.name);
        nn::fs::FileHandle f;
        if (R_FAILED(nn::fs::OpenFile(&f, path, nn::fs::OpenMode_Read))) { g_up.active = false; break; }
        u32 n = (u32)((g_up.size - g_up.sent) < (s64)SAVE_CHUNK ? g_up.size - g_up.sent : SAVE_CHUNK);
        SaveHead h{(u32)g_up.size, (u32)g_up.sent, {}};
        snprintf(h.name, sizeof h.name, "%s", g_up.name);
        memcpy(g_chunk, &h, sizeof h);
        Result r = nn::fs::ReadFile(f, g_up.sent, g_chunk + sizeof h, n);
        nn::fs::CloseFile(f);
        if (R_FAILED(r)) { g_up.active = false; break; }
        ok = SendPacket(fd, SAVE_PUT, g_chunk, sizeof h + n);
        g_up.sent += n;
        snprintf(g_cloudStatus, sizeof g_cloudStatus, "отправляю сохранение %u%%", (unsigned)(g_up.sent * 100 / (g_up.size ? g_up.size : 1)));
        if (g_up.sent >= g_up.size) {
            g_up.active = false;
            snprintf(g_upDone, sizeof g_upDone, "%s", g_up.name);
            g_upDoneSize = g_up.size;
            snprintf(g_cloudStatus, sizeof g_cloudStatus, "последнее сохранение на сервере");
            Logging.Log("[coop] online save uploaded: %s", g_up.name);
        }
    }
    return ok;
}

/* Non-blocking receive into a ring of bytes; parses whole packets. */
struct Reader {
    u8 buf[sizeof(Header) + sizeof(AppearanceHead) + APPEARANCE_MAX];
    u32 used;
    bool pendingCmd;
    bool refused;

    bool Pump(s32 fd) {                          // everything that has arrived (a save download is ~300 packets)
        for (int round = 0; round < 64; round++) {
            s32 n = nn::socket::Recv(fd, buf + used, sizeof buf - used, net::MSG_DONTWAIT);
            if (n == 0) return false;             // closed
            if (n < 0) return true;
            used += n;
            if (!Parse()) return false;
        }
        return true;
    }

    bool Parse() {
        while (used >= sizeof(Header)) {
            Header h; memcpy(&h, buf, sizeof h);
            if (sizeof h + h.size > sizeof buf) return false;
            if (used < sizeof h + h.size) break;
            Handle(h.type, buf + sizeof h, h.size);
            u32 total = sizeof h + h.size;
            memmove(buf, buf + total, used - total);
            used -= total;
        }
        return true;
    }

    void Handle(u16 type, const u8* p, u16 size) {
        if (type == WORLD && size >= sizeof(proto::WorldHead)) {
            proto::WorldHead w; memcpy(&w, p, sizeof w);
            if (w.kind == proto::WK_CAMPAIGN) { SetCampaign(w.ref); return; }
        }
        if (type == WELCOME) { g_connJoined = g_campaignState == 1; g_welcomed = true; g_clockSeq = 0;
            __atomic_store_n(&g_aqOutTail, __atomic_load_n(&g_aqOutHead, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);   // the old connection's requests
            AqIn(ACT_RESET, nullptr, 0); }
        if (type == CLOCK && size == sizeof(ClockMsg)) {                // (08.10) the server's game clock
            if (g_campaignState == 1) {
                memcpy((void*)&g_clockIn, p, sizeof g_clockIn);
                g_clockAt = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
                __atomic_add_fetch(&g_clockSeq, 1u, __ATOMIC_RELEASE);
            }
            return;
        }
        if (type >= ACT_ASSIGN && type <= ACT_PROJ) {                // the actor core (actors.cpp)
            if (g_campaignState == 1) AqIn(type, p, size);
            return;
        }
        /* (04.10) the campaign's world only reaches a campaign game: on the title screen or with a personal save
           loaded it is not taken (the console connects again once a campaign game runs, and gets it then) */
        if (g_campaignState != 1 && !(type == STATE && g_campaignState == -1))   // (the title lists who plays)
            switch (type) {
                case ACTION: case APPEARANCE: case EQUIP: case EVENT: case FOLLOW: case GIVE: case HIT: case KILL:
                case NPC_ACTION: case NPC_DAMAGE: case NPC_STATE: case QUEST: case STATE: case STATS: case TIME:
                case WORLD: case ZONE: case AVATAR_DATA: return;
                default: break;
            }
        if (type == APPEARANCE && size > sizeof(AppearanceHead)) {
            AppearanceHead a; memcpy(&a, p, sizeof a);
            appearance::Received(a.id, a.flags, p + sizeof a, size - sizeof a);
            return;
        }
        if (type == EQUIP && size >= sizeof(EquipHead)) {
            EquipHead h; memcpy(&h, p, sizeof h);
            if (sizeof h + h.count * sizeof(equip::Item) == size && h.count <= equip::MAX_ITEMS) {
                equip::Item items[equip::MAX_ITEMS];
                memcpy(items, p + sizeof h, h.count * sizeof(equip::Item));
                equip::Received(h.id, items, h.count);
            }
            return;
        }
        nn::os::LockMutex(&g_lock);
        if (type == WELCOME && size == sizeof(Welcome)) {
            Welcome w; memcpy(&w, p, sizeof w);
            g_myId = w.id;
            for (auto& s : g_statsIn) s = StatsIn{};         // (04.10) a new connection: old ids (our own of before) are gone
            g_zoneOwner = 0;                                  // the server tells the zone owner again
            if (!w.id) {                                  // refused: kicked, or another mod version
                refused = true;
                snprintf(g_msgRing[g_msgHead], sizeof g_msgRing[0], "%s", w.version != VERSION
                         ? "Кооп: на сервере другая версия мода — обнови мод и сервер"
                         : "Кооп: сервер не пускает (тебя выгнали). Подключиться снова — в меню");
                g_msgHead = (g_msgHead + 1) % MSG_LINES;
                if (g_msgCount < MSG_LINES) g_msgCount++;
            }
        } else if (type == STATE && size == sizeof(State)) {
            State s; memcpy(&s, p, sizeof s);
            int slot = -1;
            for (int i = 0; i < MAX_REMOTE; i++)
                if (g_remotes[i].active && g_remotes[i].state.id == s.id) { slot = i; break; }
            for (int i = 0; slot < 0 && i < MAX_REMOTE; i++)
                if (!g_remotes[i].active) slot = i;
            if (slot >= 0) {
                g_remotes[slot].active = true;
                g_remotes[slot].state = s;
                g_remotes[slot].receivedTick = nn::os::GetSystemTick().GetInt64Value();
            }
        } else if (type == INPUT && size == sizeof(Input)) {
            Input in; memcpy(&in, p, sizeof in);
            u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
            g_input = {in.buttons, in.lx, in.ly, in.rx, in.ry, now + in.ms};
        } else if (type == CMD && size > 0 && size < 200) {
            char cmd[200];
            memcpy(cmd, p, size);
            cmd[size] = 0;
            pendingCmd = true;
            memcpy(g_pendingCmd, cmd, size + 1);
        } else if (type == CHAT && size > sizeof(ChatHead) && size < 190) {
            ChatHead c; memcpy(&c, p, sizeof c);
            char text[192];
            memcpy(text, p + sizeof c, size - sizeof c);
            text[size - sizeof c] = 0;
            const char* who = "Сервер";
            for (auto& e : g_names) if (c.id && e.id == c.id) who = e.name;
            snprintf(g_msgRing[g_msgHead], sizeof g_msgRing[0], "%s: %s", who, text);
            g_msgHead = (g_msgHead + 1) % MSG_LINES;
            if (g_msgCount < MSG_LINES) g_msgCount++;
        } else if (type == EVENT && size > sizeof(EventHead) && size < sizeof(EventHead) + 40) {
            EventHead h; memcpy(&h, p, sizeof h);
            Ev& e = g_evIn[g_evInHead];
            e.id = h.id;
            u32 n = size - sizeof h;
            memcpy(e.name, p + sizeof h, n);
            e.name[n] = 0;
            g_evInHead = (g_evInHead + 1) % EV_LINES;
            if (g_evInCount < EV_LINES) g_evInCount++;
        } else if (type == ACTION && size == sizeof(ActionMsg)) {
            memcpy(&g_actIn[g_actInHead], p, sizeof(ActionMsg));
            g_actInHead = (g_actInHead + 1) % ACT_LINES;
            if (g_actInCount < ACT_LINES) g_actInCount++;
        } else if (type == NPC_DAMAGE && size == sizeof(DamageMsg)) {
            DamageMsg d; memcpy(&d, p, sizeof d);
            npcsync::Received(d.actor, d.amount);
        } else if (type == QUEST && size == sizeof(QuestMsg)) {
            QuestMsg q; memcpy(&q, p, sizeof q);
            quests::Received(q.quest, q.stage);
        } else if (type == FOLLOW && size == sizeof(FollowMsg)) {
            FollowMsg f; memcpy(&f, p, sizeof f);
            followers::Received(f.formId, f.leader);
        } else if (type == HIT && size == sizeof(HitMsg)) {
            HitMsg h; memcpy(&h, p, sizeof h);
            pvp::Received(h.id, h.amount);
        } else if (type == GIVE && size == sizeof(GiveMsg)) {
            GiveMsg g; memcpy(&g, p, sizeof g);
            trade::Received(g.id, g.formId, g.count);
        } else if (type == KILL && size == sizeof(KillMsg)) {
            KillMsg k; memcpy(&k, p, sizeof k);
            g_killIn[g_killInHead] = k.formId;
            g_killInFlags[g_killInHead] = k.flags;
            g_killInHead = (g_killInHead + 1) % KILL_LINES;
            if (g_killInCount < KILL_LINES) g_killInCount++;
        } else if (type == SOCIAL_INFO && size < sizeof g_socialIn) {
            memcpy(g_socialIn, p, size);
            g_socialIn[size] = 0;
            g_socialReady = true;
            if (!strncmp(g_socialIn, "#chestdeny ", 11)) chest::Denied(g_socialIn);
            else if (g_socialAsync && !strncmp(g_socialIn, "#chest ", 7)) chest::Open(g_socialIn);   // console "chest"
            Logging.Log("[coop] social in: async %d house %d", (int)g_socialAsync, (int)g_socialGoHouse);
            if (g_socialAsync) {
                g_socialAsync = false;
                g_socialLog = true;                                  // logged by the loop (g_lock is held here)
                if (g_socialGoHouse && !strncmp(g_socialIn, "#house ", 7)) {
                    char* q = g_socialIn + 7;
                    u32 c = strtoul(q, &q, 16), w = strtoul(q, &q, 16);
                    float x = strtof(q, &q), y = strtof(q, &q), z = strtof(q, &q);
                    game::TeleportTo(c, w, x, y, z);
                }
                g_socialGoHouse = false;
            }
        } else if (type == UPD_LIST && size < sizeof g_updText) {
            memcpy(g_updText, p, size);
            g_updText[size] = 0;
            g_updListReady = true;
        } else if (type == NPC_ACTION && size == sizeof(NpcActionMsg)) {
            u32 h = g_naInHead;
            if (h - g_naInTail < 32) { memcpy(&g_naIn[h % 32], p, sizeof(NpcActionMsg)); __atomic_store_n(&g_naInHead, h + 1, __ATOMIC_RELEASE); }
        } else if (type == ZONE && size >= sizeof(ZoneMsg)) {
            ZoneMsg z; memcpy(&z, p, sizeof z);
            g_zoneOwner = z.owner;
        } else if (type == WORLD) {
            world::Received(p, size);
        } else if (type == NPC_STATE && size >= 4) {
            u16 n; memcpy(&n, p, 2);
            if (n > NPC_MAX || 4 + n * sizeof(NpcState) > size) n = 0;
            if (__atomic_load_n(&g_npcInN, __ATOMIC_ACQUIRE) < 0) {        // the game thread took the last batch
                memcpy(g_npcIn, p + 4, n * sizeof(NpcState));
                __atomic_store_n(&g_npcInN, (int)n, __ATOMIC_RELEASE);
            }
        } else if (type == AVATAR_DATA && size >= sizeof(UpdHead)) {
            UpdHead h; memcpy(&h, p, sizeof h);
            u32 n = size - sizeof h;
            if (h.total == 0) g_avState = 3;
            else if (h.total <= AVATAR_MAX && h.offset + n <= h.total) {
                memcpy(g_avIn + h.offset, p + sizeof h, n);
                if (h.offset + n >= h.total) { g_avInSize = h.total; g_avState = 2; }
            }
        } else if (type == UPD_DATA && size >= sizeof(UpdHead)) {
            UpdHead h; memcpy(&h, p, sizeof h);
            h.path[159] = 0;
            ReceiveUpdateChunk(h, p + sizeof h, size - sizeof h);
        } else if (type == SAVE_LIST && size < sizeof g_listText) {
            memcpy(g_listText, p, size);
            g_listText[size] = 0;
            g_listReady = true;
        } else if (type == SAVE_DATA && size > sizeof(SaveHead)) {
            SaveHead h; memcpy(&h, p, sizeof h);
            h.name[95] = 0;
            ReceiveSaveChunk(h, p + sizeof h, size - sizeof h);
        } else if (type == STATS && size == sizeof(StatsMsg)) {
            StatsMsg m; memcpy(&m, p, sizeof m);                        // (Handle runs under g_lock)
            int slot = -1;
            for (int i = 0; i < 16; i++) if (g_statsIn[i].id == m.id) slot = i;
            for (int i = 0; i < 16 && slot < 0; i++) if (!g_statsIn[i].id) slot = i;
            if (slot >= 0) g_statsIn[slot] = StatsIn{m.id, m};
        } else if (type == TIME && size == sizeof(TimeSync)) {
            TimeSync t; memcpy(&t, p, sizeof t);
            if (t.hour >= 0 && t.hour < 24.0f) g_timeIn = t.hour;
        } else if (type == JOIN && size == sizeof(Join)) {
            Join j; memcpy(&j, p, sizeof j);
            int slot = -1;
            for (int i = 0; i < 16; i++) if (g_names[i].id == j.id) slot = i;
            for (int i = 0; slot < 0 && i < 16; i++) if (!g_names[i].id) slot = i;
            if (slot >= 0) { g_names[slot].id = j.id; memcpy(g_names[slot].name, j.name, 31); g_names[slot].name[31] = 0; }
        } else if (type == LEFT && size == sizeof(Left)) {
            Left l; memcpy(&l, p, sizeof l);
            for (auto& r : g_remotes)
                if (r.active && r.state.id == l.id) r.active = false;
            appearance::Forget(l.id);
            for (auto& s : g_statsIn) if (s.id == l.id) s = StatsIn{};   // (04.10) a gone player no longer sets the world level
            for (auto& e : g_names) if (e.id == l.id) e = NameEntry{};    // slot free for the next newcomer
            equip::Forget(l.id);
            for (auto& st : g_statsIn) if (st.id == l.id) st = StatsIn{};
        }
        nn::os::UnlockMutex(&g_lock);
    }
};

/* Room search: "SKYCOOP?" to the LAN broadcast address and to this machine (an emulator runs
   next to the server), the server answers "SKYCOOP!" + u16 TCP port + name. */
constexpr char DISCOVER_Q[8] = {'S', 'K', 'Y', 'C', 'O', 'O', 'P', '?'};
constexpr char DISCOVER_A[8] = {'S', 'K', 'Y', 'C', 'O', 'O', 'P', '!'};

int DiscoverAll(Room* out, int max, int seconds);

bool Discover(char* host, u32 hostSize, u16* port, int seconds) {
    Room r;
    if (DiscoverAll(&r, 1, seconds) < 1) return false;
    snprintf(host, hostSize, "%s", r.host);
    *port = r.port;
    snprintf(g_roomName, sizeof g_roomName, "%s", r.name);
    return true;
}

int DiscoverAll(Room* out, int max, int seconds) {
    s32 fd = nn::socket::Socket(net::AF_INET, net::SOCK_DGRAM, net::IPPROTO_UDP);
    if (fd < 0) return false;
    s32 one = 1;
    nn::socket::SetSockOpt(fd, net::SOL_SOCKET, net::SO_BROADCAST, &one, sizeof one);
    sockaddr any{}; any.family = net::AF_INET;
    nn::socket::Bind(fd, &any, sizeof any);
    /* the limited broadcast, the network's own broadcast (routers and some consoles drop 255.255.255.255)
       and this machine (an emulator next to the server) */
    in_addr targets[3] = {};
    nn::socket::InetAton("255.255.255.255", &targets[0]);
    nn::socket::InetAton("127.0.0.1", &targets[1]);
    int nTargets = 2;
    in_addr ip{}, mask{}, gw{}, d1{}, d2{};
    if (R_SUCCEEDED(nn::nifm::GetCurrentIpConfigInfo(&ip, &mask, &gw, &d1, &d2)) && ip.data && mask.data)
        targets[nTargets++].data = ip.data | ~mask.data;          // network byte order both
    int found = 0;
    for (int t = 0; t < seconds * 4 && found < max; t++) {
        if (t % 4 == 0) {
            for (int k = 0; k < nTargets; k++) {
                sockaddr to{}; to.family = net::AF_INET; to.port = nn::socket::InetHtons(proto::PORT);
                to.address = targets[k];
                nn::socket::SendTo(fd, DISCOVER_Q, sizeof DISCOVER_Q, 0, &to, sizeof to);
            }
        }
        nn::os::SleepThread(nn::TimeSpan::FromMilliSeconds(250));
        u8 buf[64];
        sockaddr from{}; u32 fromLen = sizeof from;
        s32 n;
        while ((n = nn::socket::RecvFrom(fd, buf, sizeof buf, net::MSG_DONTWAIT, &from, &fromLen)) >= 10) {
            if (memcmp(buf, DISCOVER_A, 8) != 0) continue;
            u16 p; memcpy(&p, buf + 8, 2);
            u32 ip = from.address.data;            // network byte order
            Room r{};
            snprintf(r.host, sizeof r.host, "%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, ip >> 24);
            r.port = p;
            memcpy(r.name, buf + 10, n - 10 < 47 ? n - 10 : 47);
            bool dup = false;                      // same room answers on broadcast and on 127.0.0.1
            for (int k = 0; k < found; k++) dup |= out[k].port == r.port && !strcmp(out[k].name, r.name);
            if (!dup && found < max) out[found++] = r;
        }
    }
    nn::socket::Close(fd);
    return found;
}

/* TCP connect that gives up after timeoutMs instead of hanging on an unreachable address. */
s32 ConnectTimeout(const char* host, u16 port, int timeoutMs) {
    s32 fd = nn::socket::Socket(net::AF_INET, net::SOCK_STREAM, net::IPPROTO_TCP);
    if (fd < 0) return -1;
    sockaddr addr{};
    addr.family = net::AF_INET;
    addr.port = nn::socket::InetHtons(port);
    if (!nn::socket::InetAton(host, &addr.address)) { nn::socket::Close(fd); return -1; }
    s32 flags = nn::socket::Fcntl(fd, net::F_GETFL, 0);
    nn::socket::Fcntl(fd, net::F_SETFL, flags | net::O_NONBLOCK);
    s32 rc = nn::socket::Connect(fd, &addr, sizeof addr);
    if (rc != 0) {
        pollfd p{fd, net::POLLOUT, 0};
        s32 err = -1; u32 len = sizeof err;
        if (nn::socket::Poll(&p, 1, timeoutMs) != 1 ||
            nn::socket::GetSockOpt(fd, net::SOL_SOCKET, net::SO_ERROR, &err, &len) != 0 || err != 0) {
            nn::socket::Close(fd);
            return -1;
        }
    }
    return fd;                                             // (08.10) stays non-blocking: SendAll waits in short sleeps
}

Reader reader;                          // too big for the network thread's stack

void NetThread(void*) {
    while (!g_socketsReady) nn::os::SleepThread(nn::TimeSpan::FromMilliSeconds(100));
    Logging.Log("[coop] sockets ready (game initialised them)");
    nn::nifm::Initialize();
    nn::nifm::SubmitNetworkRequest();
    while (nn::nifm::IsNetworkRequestOnHold()) nn::os::SleepThread(nn::TimeSpan::FromMilliSeconds(50));
    if (!nn::nifm::IsNetworkAvailable()) { Logging.Log("[coop] nifm: no network"); g_status = NetStatus::NoNetwork; return; }
    Logging.Log("[coop] network up");
    if (updates::TestAutoConnect()) { g_online = true; Logging.Log("[coop] test machine: connecting at boot"); }

    for (int i = 0; i < 200 && !settings::Loaded(); i++) {         // up to 20 s for the saved name / address
        settings::LoadIfNeeded();
        nn::os::SleepThread(nn::TimeSpan::FromMilliSeconds(100));
    }
    int manualFailures = 0;
    for (;;) {
        if (!g_online) { g_status = NetStatus::Idle; nn::os::SleepThread(nn::TimeSpan::FromMilliSeconds(300)); continue; }
        settings::LoadIfNeeded();
        settings::TakeChanged();
        char host[64];
        u16 port = proto::PORT;
        bool manual = manualFailures < 2 && settings::ManualAddress(host, sizeof host, &port);
        if (!manual) {
            g_status = NetStatus::Searching;
            g_roomName[0] = 0;
            if (!Discover(host, sizeof host, &port, 3)) { manualFailures = 0; continue; }
            Logging.Log("[coop] found room '%s' at %s:%u", g_roomName, host, port);
        }
        g_status = NetStatus::Connecting;
        g_welcomed = false;
        s32 fd = ConnectTimeout(host, port, 3000);
        if (fd < 0) {
            Logging.Log("[coop] connect to %s:%u failed", host, port);
            if (manual) manualFailures++;
            g_status = NetStatus::Failed;
            nn::os::SleepThread(nn::TimeSpan::FromSeconds(2));
            continue;
        }
        {   /* (08.10) Eden reports a refused connect as done: a peek tells (ENOTCONN on a dead socket, EAGAIN on a live one) */
            u8 peek;
            s32 n = nn::socket::Recv(fd, &peek, 0, net::MSG_DONTWAIT);
            u32 e = n < 0 ? nn::socket::GetLastErrno() : 0;
            if (n < 0 && e != 11 && e != 35) {
                Logging.Log("[coop] connect to %s:%u: not connected (%u)", host, port, e);
                nn::socket::Close(fd);
                g_status = NetStatus::Failed;
                nn::os::SleepThread(nn::TimeSpan::FromSeconds(2));
                continue;
            }
        }
        manualFailures = 0;
        s32 one = 1;
        nn::socket::SetSockOpt(fd, net::IPPROTO_TCP, net::TCP_NODELAY, &one, sizeof one);

        Hello hello{VERSION, {}};
        settings::GetName(g_name, sizeof g_name);
        strncpy(hello.name, g_name, sizeof hello.name);
        bool ok = SendPacket(fd, HELLO, &hello, sizeof hello);
        snprintf(g_server, sizeof g_server, "%s:%u", host, port);
        g_status = NetStatus::Connected;
        Logging.Log("[coop] connected to %s", g_server);

        reader.used = 0;
        reader.pendingCmd = false;
        reader.refused = false;
        u32 lastTick = ~0u;
        u32 sentAppearance = 0, sentEquipment = 0;
        bool sentPlugins = false;
        while (ok) {
            if (settings::TakeChanged() || !g_online) break; // new address / name, or switched offline
            if (g_welcomed && g_campaignState == 1 && !g_connJoined) break;   // a campaign game began: connect again for its world
            ok = reader.Pump(fd);
            if (reader.refused) { g_online = false; break; }        // stays offline until the menu says otherwise
            if (reader.pendingCmd) { reader.pendingCmd = false; console::Execute(g_pendingCmd); }   // outside g_lock
            {
                static u64 nextBeat;
                u64 t = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
                if (t >= nextBeat) {
                    nextBeat = t + 10000;
                    Logging.Log("[coop] пульс сети: шаг игры '%s', update %u/%u, perform %u/%u", (const char*)g_gameStep,
                                g_hookIn[0], g_hookOut[0], g_hookIn[1], g_hookOut[1]);
                }
            }
            if (g_socialLog) {
                g_socialLog = false;
                char first[120];
                snprintf(first, sizeof first, "%s", g_socialIn);
                if (char* nl = strchr(first, 10)) *nl = 0;
                Log("social answer: %s", first);
            }
            nn::os::LockMutex(&g_lock);
            State s = g_local;
            bool valid = g_localValid;
            nn::os::UnlockMutex(&g_lock);
            char line[200];
            for (;;) {                                        // queued log lines -> server console
                nn::os::LockMutex(&g_lock);
                bool have = g_logCount > 0;
                if (have) {
                    memcpy(line, g_logRing[(g_logHead - g_logCount + LOG_LINES) % LOG_LINES], sizeof line);
                    g_logCount--;
                }
                nn::os::UnlockMutex(&g_lock);
                if (!have || !ok) break;
                ok = SendPacket(fd, LOG, line, strlen(line));
            }
            if (ok && g_appearanceVersion != sentAppearance) {   // appearance: after connect and on change
                static u8 copy[sizeof g_appearance];
                nn::os::LockMutex(&g_lock);
                u32 n = g_appearanceSize;
                sentAppearance = g_appearanceVersion;
                memcpy(copy, g_appearance, n);
                nn::os::UnlockMutex(&g_lock);
                if (n) ok = SendPacket(fd, APPEARANCE, copy, n);
            }
            for (;;) {                                        // animation events, as they happened
                Ev e;
                nn::os::LockMutex(&g_lock);
                bool have = g_evOutCount > 0;
                if (have) { e = g_evOut[(g_evOutHead - g_evOutCount + EV_LINES) % EV_LINES]; g_evOutCount--; }
                nn::os::UnlockMutex(&g_lock);
                if (!have || !ok) break;
                u8 pkt[sizeof(EventHead) + 40];
                EventHead h{0, static_cast<u16>(strlen(e.name))};
                memcpy(pkt, &h, sizeof h);
                memcpy(pkt + sizeof h, e.name, h.len);
                ok = SendPacket(fd, EVENT, pkt, sizeof h + h.len);
            }
            if (ok && !sentPlugins && g_pluginsReady) {
                sentPlugins = true;
                ok = SendPacket(fd, PLUGINS, g_plugins, strlen(g_plugins));
            }
            for (;;) {                                        // damage to NPCs
                DamageMsg dm;
                nn::os::LockMutex(&g_lock);
                bool have = g_dCount > 0;
                if (have) { dm = g_dOut[(g_dHead - g_dCount + DMG_LINES) % DMG_LINES]; g_dCount--; }
                nn::os::UnlockMutex(&g_lock);
                if (!have || !ok) break;
                ok = SendPacket(fd, NPC_DAMAGE, &dm, sizeof dm);
            }
            for (;;) {                                        // quest stages
                QuestMsg qm;
                nn::os::LockMutex(&g_lock);
                bool have = g_qCount > 0;
                if (have) { qm = g_qOut[(g_qHead - g_qCount + QUEST_LINES) % QUEST_LINES]; g_qCount--; }
                nn::os::UnlockMutex(&g_lock);
                if (!have || !ok) break;
                ok = SendPacket(fd, QUEST, &qm, sizeof qm);
            }
            for (;;) {                                        // follower orders
                FollowMsg fm;
                nn::os::LockMutex(&g_lock);
                bool have = g_folCount > 0;
                if (have) { fm = g_folOut[(g_folHead - g_folCount + FOLLOW_LINES) % FOLLOW_LINES]; g_folCount--; }
                nn::os::UnlockMutex(&g_lock);
                if (!have || !ok) break;
                ok = SendPacket(fd, FOLLOW, &fm, sizeof fm);
            }
            for (;;) {                                        // hits on other players
                HitMsg hm;
                nn::os::LockMutex(&g_lock);
                bool have = g_hitCount > 0;
                if (have) { hm = g_hitOut[(g_hitHead - g_hitCount + HIT_LINES) % HIT_LINES]; g_hitCount--; }
                nn::os::UnlockMutex(&g_lock);
                if (!have || !ok) break;
                ok = SendPacket(fd, HIT, &hm, sizeof hm);
            }
            for (;;) {                                        // items given to other players
                GiveMsg gm;
                nn::os::LockMutex(&g_lock);
                bool have = g_giveCount > 0;
                if (have) { gm = g_giveOut[(g_giveHead - g_giveCount + GIVE_LINES) % GIVE_LINES]; g_giveCount--; }
                nn::os::UnlockMutex(&g_lock);
                if (!have || !ok) break;
                ok = SendPacket(fd, GIVE, &gm, sizeof gm);
            }
            for (;;) {                                        // actions, in order
                ActionMsg a;
                nn::os::LockMutex(&g_lock);
                bool have = g_actOutCount > 0;
                if (have) { a = g_actOut[(g_actOutHead - g_actOutCount + ACT_LINES) % ACT_LINES]; g_actOutCount--; }
                nn::os::UnlockMutex(&g_lock);
                if (!have || !ok) break;
                ok = SendPacket(fd, ACTION, &a, sizeof a);
            }
            for (;;) {
                nn::os::LockMutex(&g_lock);
                bool have = g_killOutCount > 0;
                u32 id = have ? g_killOut[(g_killOutHead - g_killOutCount + KILL_LINES) % KILL_LINES] : 0;
                u16 kf = have ? g_killOutFlags[(g_killOutHead - g_killOutCount + KILL_LINES) % KILL_LINES] : 0;
                if (have) g_killOutCount--;
                nn::os::UnlockMutex(&g_lock);
                if (!have || !ok) break;
                KillMsg k{0, kf, id};
                ok = SendPacket(fd, KILL, &k, sizeof k);
            }
            if (ok && g_statsDirty) {
                g_statsDirty = false;
                StatsMsg m = g_statsOut;
                ok = SendPacket(fd, STATS, &m, sizeof m);
            }
            if (ok && g_timeDirty) {
                g_timeDirty = false;
                TimeSync t{0, 0, g_timeOut};
                ok = SendPacket(fd, TIME, &t, sizeof t);
            }
            if (ok) ok = CloudTick(fd, nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds());
            if (ok && g_chatPending) {
                char text[160];
                nn::os::LockMutex(&g_lock);
                memcpy(text, g_chatOut, sizeof text);
                g_chatPending = false;
                nn::os::UnlockMutex(&g_lock);
                u8 pkt[sizeof(ChatHead) + sizeof text];
                ChatHead h{0};
                u32 n = strlen(text);
                memcpy(pkt, &h, sizeof h);
                memcpy(pkt + sizeof h, text, n);
                ok = SendPacket(fd, CHAT, pkt, sizeof h + n);
            }
            if (ok && g_equipmentVersion != sentEquipment) {
                u8 copy[sizeof g_equipment];
                nn::os::LockMutex(&g_lock);
                u32 n = g_equipmentSize;
                sentEquipment = g_equipmentVersion;
                memcpy(copy, g_equipment, n);
                nn::os::UnlockMutex(&g_lock);
                if (n) ok = SendPacket(fd, EQUIP, copy, n);
            }
            if (ok && valid && s.tick != lastTick) {
                s.id = g_myId;
                ok = SendPacket(fd, STATE, &s, sizeof s);
                lastTick = s.tick;
            }
            nn::os::SleepThread(nn::TimeSpan::FromMilliSeconds(50));   // 20 Hz
        }
        nn::socket::Close(fd);
        nn::os::LockMutex(&g_lock);
        for (auto& r : g_remotes) r.active = false;
        nn::os::UnlockMutex(&g_lock);
        g_status = NetStatus::Failed;
        nn::os::SleepThread(nn::TimeSpan::FromSeconds(1));
    }
}

}  // namespace

void StartClient() {
    EnsureLock();
    using InitFn = Result (*)(const nn::socket::Config&);
    SocketInit::InstallAtPtr(reinterpret_cast<uintptr_t>(static_cast<InitFn>(&nn::socket::Initialize)));
    nn::os::CreateThread(&g_thread, NetThread, nullptr, g_threadStack, sizeof g_threadStack, 16, -2);
    nn::os::StartThread(&g_thread);
}

void PublishLocalState(const State& s) {
    if (Status() != NetStatus::Connected) return;          // (04.10) a personal save: nothing to the campaign
    nn::os::LockMutex(&g_lock);
    g_local = s;
    g_localValid = true;
    nn::os::UnlockMutex(&g_lock);
}

void PublishAppearance(u32 flags, const void* data, u32 size) {
    if (Status() != NetStatus::Connected) return;          // (04.10) a personal save: nothing to the campaign
    if (!size || size > APPEARANCE_MAX) return;
    AppearanceHead h{0, 0, flags};
    nn::os::LockMutex(&g_lock);
    memcpy(g_appearance, &h, sizeof h);
    memcpy(g_appearance + sizeof h, data, size);
    g_appearanceSize = sizeof h + size;
    __atomic_add_fetch(&g_appearanceVersion, 1u, __ATOMIC_RELEASE);
    nn::os::UnlockMutex(&g_lock);
}

void PublishEquipment(const void* items, u32 count, u32 itemSize) {
    if (Status() != NetStatus::Connected) return;          // (04.10) a personal save: nothing to the campaign
    if (sizeof(EquipHead) + count * itemSize > sizeof g_equipment) return;
    EquipHead h{0, static_cast<u16>(count)};
    nn::os::LockMutex(&g_lock);
    memcpy(g_equipment, &h, sizeof h);
    memcpy(g_equipment + sizeof h, items, count * itemSize);
    g_equipmentSize = sizeof h + count * itemSize;
    __atomic_add_fetch(&g_equipmentVersion, 1u, __ATOMIC_RELEASE);
    nn::os::UnlockMutex(&g_lock);
}

int SnapshotRemotes(RemotePlayer* out, int max) {
    int n = 0;
    if (Status() != NetStatus::Connected) return 0;          // (04.10) a personal save: no other players in it
    nn::os::LockMutex(&g_lock);
    for (auto& r : g_remotes)
        if (r.active && n < max) out[n++] = r;
    nn::os::UnlockMutex(&g_lock);
    return n;
}

bool TakeMessage(char* out, u32 size) {
    bool have = false;
    nn::os::LockMutex(&g_lock);
    if (g_msgCount > 0) {
        snprintf(out, size, "%s", g_msgRing[(g_msgHead - g_msgCount + MSG_LINES) % MSG_LINES]);
        g_msgCount--;
        have = true;
    }
    nn::os::UnlockMutex(&g_lock);
    return have;
}

void PushEvent(const char* name) {
    if (Status() != NetStatus::Connected) return;          // (04.10) a personal save: nothing to the campaign
    if (Status() != NetStatus::Connected) return;
    nn::os::LockMutex(&g_lock);
    Ev& e = g_evOut[g_evOutHead];
    e.id = 0;
    snprintf(e.name, sizeof e.name, "%s", name);
    g_evOutHead = (g_evOutHead + 1) % EV_LINES;
    if (g_evOutCount < EV_LINES) g_evOutCount++;
    nn::os::UnlockMutex(&g_lock);
}

bool TakeEvent(u16* id, char* name, u32 size) {
    bool have = false;
    nn::os::LockMutex(&g_lock);
    if (g_evInCount > 0) {
        const Ev& e = g_evIn[(g_evInHead - g_evInCount + EV_LINES) % EV_LINES];
        *id = e.id;
        snprintf(name, size, "%s", e.name);
        g_evInCount--;
        have = true;
    }
    nn::os::UnlockMutex(&g_lock);
    return have;
}

void PushAction(const ActionMsg& m) {
    if (Status() != NetStatus::Connected) return;          // (04.10) a personal save: nothing to the campaign
    if (Status() != NetStatus::Connected) return;
    nn::os::LockMutex(&g_lock);
    g_actOut[g_actOutHead] = m;
    g_actOutHead = (g_actOutHead + 1) % ACT_LINES;
    if (g_actOutCount < ACT_LINES) g_actOutCount++;
    nn::os::UnlockMutex(&g_lock);
}

bool TakeAction(u16* id, ActionMsg* m) {
    bool have = false;
    nn::os::LockMutex(&g_lock);
    if (g_actInCount > 0) {
        *m = g_actIn[(g_actInHead - g_actInCount + ACT_LINES) % ACT_LINES];
        *id = m->id;
        g_actInCount--;
        have = true;
    }
    nn::os::UnlockMutex(&g_lock);
    return have;
}

void PublishPlugins(const char* list) {
    if (g_pluginsReady) return;
    snprintf(g_plugins, sizeof g_plugins, "%s", list);
    g_pluginsReady = true;
}

void PushDamage(u32 actor, float amount) {
    if (Status() != NetStatus::Connected) return;          // (04.10) a personal save: nothing to the campaign
    nn::os::LockMutex(&g_lock);
    g_dOut[g_dHead] = DamageMsg{actor, amount};
    g_dHead = (g_dHead + 1) % DMG_LINES;
    if (g_dCount < DMG_LINES) g_dCount++;
    nn::os::UnlockMutex(&g_lock);
}

void PushQuest(u32 quest, u16 stage) {
    if (Status() != NetStatus::Connected) return;          // (04.10) a personal save: nothing to the campaign
    if (!quests::IntroDone()) return;                      // (07.10) the intro is each player's own: none of its stages go out
    nn::os::LockMutex(&g_lock);
    g_qOut[g_qHead] = QuestMsg{quest, stage, 0};
    g_qHead = (g_qHead + 1) % QUEST_LINES;
    if (g_qCount < QUEST_LINES) g_qCount++;
    nn::os::UnlockMutex(&g_lock);
}

void PushFollow(u32 formId, u16 leader) {
    if (Status() != NetStatus::Connected) return;          // (04.10) a personal save: nothing to the campaign
    nn::os::LockMutex(&g_lock);
    g_folOut[g_folHead] = FollowMsg{formId, leader, 0};
    g_folHead = (g_folHead + 1) % FOLLOW_LINES;
    if (g_folCount < FOLLOW_LINES) g_folCount++;
    nn::os::UnlockMutex(&g_lock);
}

void PushHit(u16 victim, float amount) {
    if (Status() != NetStatus::Connected) return;          // (04.10) a personal save: nothing to the campaign
    nn::os::LockMutex(&g_lock);
    g_hitOut[g_hitHead] = HitMsg{victim, 0, amount};
    g_hitHead = (g_hitHead + 1) % HIT_LINES;
    if (g_hitCount < HIT_LINES) g_hitCount++;
    nn::os::UnlockMutex(&g_lock);
}

void PushGive(u16 toPlayer, u32 formId, s32 count) {
    if (Status() != NetStatus::Connected) return;          // (04.10) a personal save: nothing to the campaign
    nn::os::LockMutex(&g_lock);
    g_giveOut[g_giveHead] = GiveMsg{toPlayer, 0, formId, count};
    g_giveHead = (g_giveHead + 1) % GIVE_LINES;
    if (g_giveCount < GIVE_LINES) g_giveCount++;
    nn::os::UnlockMutex(&g_lock);
}

void PushKill(u32 formId, u16 flags) {
    if (Status() != NetStatus::Connected) return;          // (04.10) a personal save: nothing to the campaign
    nn::os::LockMutex(&g_lock);
    g_killOut[g_killOutHead] = formId;
    g_killOutFlags[g_killOutHead] = flags;
    g_killOutHead = (g_killOutHead + 1) % KILL_LINES;
    if (g_killOutCount < KILL_LINES) g_killOutCount++;
    nn::os::UnlockMutex(&g_lock);
}

bool TakeKill(u32* formId, u16* flags) {
    bool have = false;
    nn::os::LockMutex(&g_lock);
    if (g_killInCount > 0) {
        *formId = g_killIn[(g_killInHead - g_killInCount + KILL_LINES) % KILL_LINES];
        *flags = g_killInFlags[(g_killInHead - g_killInCount + KILL_LINES) % KILL_LINES];
        g_killInCount--;
        have = true;
    }
    nn::os::UnlockMutex(&g_lock);
    return have;
}

void PublishTime(float hour) { if (Status() != NetStatus::Connected) return; g_timeOut = hour; g_timeDirty = true; }

void PublishStats(u16 level, u8 hp, u8 mp, u8 sp) {
    if (Status() != NetStatus::Connected) return;          // (04.10) a personal save: nothing to the campaign
    g_statsOut = StatsMsg{0, level, hp, mp, sp, 0};
    g_statsDirty = true;
}

bool StatsOf(u16 playerId, StatsMsg* out) {
    bool found = false;
    nn::os::LockMutex(&g_lock);
    for (auto& s : g_statsIn)
        if (s.id == playerId) { *out = s.m; found = true; }
    nn::os::UnlockMutex(&g_lock);
    return found;
}
float TakeTime() { float h = g_timeIn; g_timeIn = -1; return h; }

bool CloudList(char* out, u32 size, int timeoutMs) {
    if (Status() != NetStatus::Connected) return false;
    g_listReady = false;
    g_listWanted = true;
    for (int t = 0; t < timeoutMs && !g_listReady; t += 50) nn::os::SleepThread(nn::TimeSpan::FromMilliSeconds(50));
    if (!g_listReady) return false;
    snprintf(out, size, "%s", g_listText);
    return true;
}

void SocialSend(const char* command, bool goHouse) {
    Log("social send: %s", command);
    snprintf(g_socialOut, sizeof g_socialOut, "%s", command);
    g_socialGoHouse = goHouse;
    g_socialAsync = true;
    g_socialWanted = true;
}

bool Social(const char* command, char* out, u32 size, int timeoutMs) {
    if (Status() != NetStatus::Connected) return false;
    snprintf(g_socialOut, sizeof g_socialOut, "%s", command);
    g_socialReady = false;
    g_socialWanted = true;
    for (int t = 0; t < timeoutMs && !g_socialReady; t += 50) nn::os::SleepThread(nn::TimeSpan::FromMilliSeconds(50));
    if (!g_socialReady) return false;
    snprintf(out, size, "%s", g_socialIn);
    return true;
}

bool AvatarPut(const void* data, u32 size) {
    if (g_avPutActive || !size || size > AVATAR_MAX || Status() != NetStatus::Connected) return false;
    memcpy(g_avOut, data, size);
    g_avOutSize = size;
    g_avSent = 0;
    __atomic_store_n(&g_avPutActive, true, __ATOMIC_RELEASE);
    return true;
}

void AvatarRequest() {
    g_avState = 1;
    g_avGetWanted = true;
}

int AvatarState() { return g_avState; }

const u8* AvatarData(u32* size) {
    if (g_avState != 2) return nullptr;
    *size = g_avInSize;
    return g_avIn;
}

bool UpdateList(char* out, u32 size, int timeoutMs) {
    if (Status() != NetStatus::Connected) return false;
    g_updListReady = false;
    g_updListWanted = true;
    for (int t = 0; t < timeoutMs && !g_updListReady; t += 50) nn::os::SleepThread(nn::TimeSpan::FromMilliSeconds(50));
    if (!g_updListReady) return false;
    snprintf(out, size, "%s", g_updText);
    return true;
}

bool UpdateDownload(const char* path, const char* localPath, int timeoutMs) {
    if (Status() != NetStatus::Connected) return false;
    snprintf(g_updPath, sizeof g_updPath, "%s", path);
    snprintf(g_updLocal, sizeof g_updLocal, "%s", localPath);
    g_updDone = false;
    g_updFailed = false;
    g_updGetWanted = true;
    for (int t = 0; t < timeoutMs && !g_updDone; t += 100) nn::os::SleepThread(nn::TimeSpan::FromMilliSeconds(100));
    return g_updDone && !g_updFailed;
}

bool CloudDownload(const char* file, int timeoutMs) {
    if (Status() != NetStatus::Connected) return false;
    snprintf(g_getName, sizeof g_getName, "%s", file);
    g_getDone = false;
    g_getFailed = false;
    g_getWanted = true;
    for (int t = 0; t < timeoutMs && !g_getDone; t += 100) nn::os::SleepThread(nn::TimeSpan::FromMilliSeconds(100));
    return g_getDone && !g_getFailed;
}

const char* CloudStatus() { return g_cloudStatus[0] ? g_cloudStatus : "ещё не было"; }

void SendChat(const char* text) {
    if (!text || !text[0]) return;
    nn::os::LockMutex(&g_lock);
    snprintf(g_chatOut, sizeof g_chatOut, "%s", text);
    g_chatPending = true;
    nn::os::UnlockMutex(&g_lock);
}

NetStatus Status() {
    NetStatus s = g_status;
    if (s != NetStatus::Connected) return s;
    int c = g_campaignState;
    if (c == 0) return NetStatus::Personal;                // a personal save: the campaign is not touched
    if (c == 2) return NetStatus::Connecting;              // a game loading: nothing goes either way yet
    return s;
}
NetStatus LinkStatus() { return g_status; }
void SetCampaign(u32 id) { g_campaignId = id; Logging.Log("[coop] campaign %u", id); }
u32 CampaignId() { return g_campaignId; }
void SetCampaignState(int state) {
    if (state != g_campaignState) Logging.Log("[coop] campaign state %d -> %d", g_campaignState, state);
    g_campaignState = state;
}
void SetOnline(bool on) { g_online = on; }
bool IsOnline() { return g_online; }
int FindRooms(Room* out, int max, int seconds) { return DiscoverAll(out, max, seconds); }
bool PlayerName(u16 id, char* out, u32 size) {
    bool ok = false;
    nn::os::LockMutex(&g_lock);
    for (auto& e : g_names) if (e.id == id && e.name[0]) { snprintf(out, size, "%s", e.name); ok = true; }
    nn::os::UnlockMutex(&g_lock);
    return ok;
}

u32 g_logSeq;                      // lines logged so far (the ring's k-th line sits at k % LOG_LINES)
/* (06.10) the Eden test channel: the lines logged since *seq, newline-joined (updates.cpp writes them to the SD) */
u32 LogSince(u32* seq, char* out, u32 size) {
    EnsureLock();
    nn::os::LockMutex(&g_lock);
    u32 from = *seq, to = g_logSeq, len = 0;
    if (to - from > (u32)g_logCount) from = to - g_logCount;
    for (u32 k = from; k < to; k++) {
        const char* l = g_logRing[k % LOG_LINES];
        u32 n = strlen(l);
        if (len + n + 2 > size) { to = k; break; }
        memcpy(out + len, l, n); len += n; out[len++] = '\n';
    }
    *seq = to;
    nn::os::UnlockMutex(&g_lock);
    return len;
}
void Log(const char* fmt, ...) {
    char line[200];
    va_list va;
    va_start(va, fmt);
    vsnprintf(line, sizeof line, fmt, va);
    va_end(va);
    Logging.Log("[coop] %s", line);
    EnsureLock();
    nn::os::LockMutex(&g_lock);
    memcpy(g_logRing[g_logHead], line, sizeof line);
    g_logHead = (g_logHead + 1) % LOG_LINES;
    g_logSeq++;
    if (g_logCount < LOG_LINES) g_logCount++;
    nn::os::UnlockMutex(&g_lock);
}
volatile bool g_debugToServer;
void SetDebug(bool on) { g_debugToServer = on; }
void ToggleDebug() { g_debugToServer = !g_debugToServer; Log("debug lines to the server: %s", g_debugToServer ? "on" : "off"); }

void Debug(const char* fmt, ...) {
    char line[200];
    va_list va;
    va_start(va, fmt);
    vsnprintf(line, sizeof line, fmt, va);
    va_end(va);
    if (g_debugToServer) { Log("%s", line); return; }
    Logging.Log("[coop] %s", line);
}

void InjectLocal(u64 buttons, s32 lx, s32 ly, s32 rx, s32 ry, u32 ms) {   // the Eden test channel (updates.cpp)
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    nn::os::LockMutex(&g_lock);
    g_input = InjectedInput{buttons, lx, ly, rx, ry, now + ms};
    nn::os::UnlockMutex(&g_lock);
}
InjectedInput Injected() {
    nn::os::LockMutex(&g_lock);
    InjectedInput i = g_input;
    nn::os::UnlockMutex(&g_lock);
    return i;
}
const char* ServerName() { return g_roomName[0] ? g_roomName : g_server; }   // no room name: its address
u16 MyId() { return g_myId; }
u16 ZoneOwner() { return g_zoneOwner; }

void PushTelemetry(const char* text, u32 size) {
    if (LinkStatus() != NetStatus::Connected || size > sizeof g_tele) return;
    nn::os::LockMutex(&g_lock);
    memcpy(g_tele, text, size); g_teleLen = size; g_teleDirty = true;
    nn::os::UnlockMutex(&g_lock);
}

void PushWorld(const void* data, u32 size) {
    if (Status() != NetStatus::Connected) return;          // (04.10) a personal save: nothing to the campaign
    u32 h = g_woHead;
    if (size > WO_SIZE || h - __atomic_load_n(&g_woTail, __ATOMIC_ACQUIRE) >= WO_MAX) return;   // full: dropped
    memcpy(g_woOut[h % WO_MAX], data, size);
    g_woLen[h % WO_MAX] = (u16)size;
    __atomic_store_n(&g_woHead, h + 1, __ATOMIC_RELEASE);
}

void PushNpcStates(const proto::NpcState* s, int n) {
    if (Status() != NetStatus::Connected) return;          // (04.10) a personal save: nothing to the campaign
    if (n < 0 || n > NPC_MAX || __atomic_load_n(&g_npcOutN, __ATOMIC_ACQUIRE) >= 0) return;   // the last not sent yet
    memcpy(g_npcOut, s, n * sizeof(NpcState));
    __atomic_store_n(&g_npcOutN, n, __ATOMIC_RELEASE);
}

void PushNpcAction(const proto::NpcActionMsg& m) {
    if (Status() != NetStatus::Connected) return;          // (04.10) a personal save: nothing to the campaign
    u32 h = g_naOutHead;
    if (h - g_naOutTail >= 32) return;
    g_naOut[h % 32] = m;
    __atomic_store_n(&g_naOutHead, h + 1, __ATOMIC_RELEASE);
}

bool TakeNpcAction(proto::NpcActionMsg* m) {
    if (g_naInTail == __atomic_load_n(&g_naInHead, __ATOMIC_ACQUIRE)) return false;
    *m = g_naIn[g_naInTail % 32];
    __atomic_store_n(&g_naInTail, g_naInTail + 1, __ATOMIC_RELEASE);
    return true;
}

int TakeNpcStates(proto::NpcState* out, int max) {
    int n = __atomic_load_n(&g_npcInN, __ATOMIC_ACQUIRE);
    if (n < 0) return -1;
    if (n > max) n = max;
    memcpy(out, g_npcIn, n * sizeof(NpcState));
    __atomic_store_n(&g_npcInN, -1, __ATOMIC_RELEASE);
    return n;
}

/* (08.10) the actor core: game thread -> network thread, and back */
bool PushActorPacket(u16 type, const void* data, u16 size) {
    u32 h = g_aqOutHead;
    if (h - __atomic_load_n(&g_aqOutTail, __ATOMIC_ACQUIRE) >= AQ_OUT || size > AQ_SIZE) return false;
    auto& q = g_aqOut[h % AQ_OUT];
    q.type = type; q.size = size;
    if (size) memcpy(q.data, data, size);
    __atomic_store_n(&g_aqOutHead, h + 1, __ATOMIC_RELEASE);
    return true;
}

bool TakeActorPacket(u16* type, u8* out, u16* size) {
    u32 t = g_aqInTail;
    if (t == __atomic_load_n(&g_aqInHead, __ATOMIC_ACQUIRE)) return false;
    auto& q = g_aqIn[t % AQ_IN];
    *type = q.type; *size = q.size;
    if (q.size) memcpy(out, q.data, q.size);
    __atomic_store_n(&g_aqInTail, t + 1, __ATOMIC_RELEASE);
    return true;
}

/* (08.10) the server's game clock: the last one and when it came (our ms); false = none on this connection yet */
bool ServerClock(proto::ClockMsg* out, u64* atMs, u32* seq) {
    u32 q = __atomic_load_n(&g_clockSeq, __ATOMIC_ACQUIRE);
    if (!q) return false;
    memcpy(out, (const void*)&g_clockIn, sizeof *out);
    *atMs = g_clockAt;
    *seq = q;
    return true;
}

}  // namespace coop
