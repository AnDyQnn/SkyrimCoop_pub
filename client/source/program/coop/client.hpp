#pragma once
#include "common.hpp"
#include "protocol.hpp"

namespace coop {

constexpr int MAX_REMOTE = 8;

struct RemotePlayer {
    bool active;
    proto::State state;
    u64 receivedTick;        // os tick when the last state arrived
};

enum class NetStatus : u32 { Idle, NoNetwork, Searching, Connecting, Connected, Failed, Personal };   // Personal: connected, a personal save loaded (no sync)

/* Called once from exl_main. Starts the network thread; the server is the room found on the
   network, or the address the player typed in (settings). The player name comes from settings. */
void StartClient();

/* Online switch from the menu: false = disconnect and stay offline. */
void SetOnline(bool on);
bool IsOnline();

/* Rooms answering on the network within `seconds` (menu "rooms"). */
struct Room { char name[48]; char host[20]; u16 port; };
int FindRooms(Room* out, int max, int seconds);

/* Name of a player in the room (from the server's JOIN), false if unknown. */
bool PlayerName(u16 id, char* out, u32 size);

/* Game thread -> network thread: latest local player state. */
void PublishLocalState(const proto::State& s);

/* Game thread -> network thread: the local player's appearance (sent now and after each reconnect). */
void PublishAppearance(u32 flags, const void* data, u32 size);
/* Game thread -> network thread: what the local player wears (EquipHead payload without the head). */
void PublishEquipment(const void* items, u32 count, u32 itemSize);

/* Network thread -> game thread: copy of remote players. Returns count copied. */
int SnapshotRemotes(RemotePlayer* out, int max);

/* Test input sent by the server, valid until untilMs (0 = none). */
struct InjectedInput { u64 buttons; s32 lx, ly, rx, ry; u64 untilMs; };
InjectedInput Injected();
u32 LogSince(u32* seq, char* out, u32 size);   // (Eden test channel) lines logged since *seq
void InjectLocal(u64 buttons, s32 lx, s32 ly, s32 rx, s32 ry, u32 ms);

/* Module log line: goes to the svc log and, when connected, to the server console. */
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
/* Diagnostics: svc log only; also to the server after the console command "coopdebug". */
void Debug(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void ToggleDebug();
void SetDebug(bool on);

/* Chat: messages for the screen ("Имя: текст"), and sending one to the room. */
bool TakeMessage(char* out, u32 size);

/* Animation events: the local player's (game thread -> network) and the other players' (network -> game). */
void PushEvent(const char* name);
/* nameplates: ours out, theirs in (plates.cpp) */
void PublishStats(u16 level, u8 hp, u8 mp, u8 sp);
bool StatsOf(u16 playerId, proto::StatsMsg* out);

/* Game clock: ours out (game thread), the host's in (-1 = nothing new). */
void PublishTime(float hour);

/* Shared world: actors killed here (out) and by the others (in). */
void PushKill(u32 formId, u16 flags);       // flags: proto::KillFlags
void PushGive(u16 toPlayer, u32 formId, s32 count);
void PushHit(u16 victim, float amount);
void PushFollow(u32 formId, u16 leader);
void PushQuest(u32 quest, u16 stage);
void PushDamage(u32 actor, float amount);
/* Game thread: the load order (sent once per connection). */
void PublishPlugins(const char* list);
void PushAction(const proto::ActionMsg& m);
bool TakeAction(u16* id, proto::ActionMsg* m);
bool TakeKill(u32* formId, u16* flags);
float TakeTime();
bool TakeEvent(u16* id, char* name, u32 size);
void SendChat(const char* text);

/* Online saves (menu): the list of this player's saves on the server ("file	size	date" lines), and a
   download into the game's save folder. Uploading the newest save runs by itself while connected. */
bool CloudList(char* out, u32 size, int timeoutMs);
/* Clans / marriage: a text command to the server (server/social.py) and its answer. */
bool Social(const char* command, char* out, u32 size, int timeoutMs);
/* Same without waiting (console commands run on the network thread); goHouse: teleport when the answer
   carries the clan house. The answer is logged. */
void SocialSend(const char* command, bool goHouse);
bool CloudDownload(const char* file, int timeoutMs);
/* Updates (updates.cpp): the server's file list, and one file downloaded to a local path (e.g. sd:/...). */
bool UpdateList(char* out, u32 size, int timeoutMs);
/* Avatar (avatar.cpp), game thread: send the profile (copied; false while the previous one is still going),
   ask for ours; state 0 idle, 1 waiting, 2 ready (AvatarData), 3 the server has none. */
bool AvatarPut(const void* data, u32 size);
void AvatarRequest();
int AvatarState();
const u8* AvatarData(u32* size);
bool UpdateDownload(const char* path, const char* localPath, int timeoutMs);
const char* CloudStatus();

NetStatus Status();                 // Connected only for a campaign game (or the title screen); see LinkStatus
NetStatus LinkStatus();
void PushTelemetry(const char* text, u32 size);   // game thread: the test tools' picture of the game (TELE)             // the connection itself
void SetCampaign(u32 id);           // network thread: the server's campaign number (WK_CAMPAIGN)
u32 CampaignId();
void SetCampaignState(int state);   // game thread: -1 title, 0 personal save, 1 campaign save, 2 loading
const char* ServerName();     // room name, or host:port when typed in
u16 MyId();
/* Zones (npcpos.cpp): the owner of the zone we are in (0 = none yet); NPC states in and out. */
u16 ZoneOwner();
void PushWorld(const void* data, u32 size);                 // game thread: a WORLD packet to the server
void PushNpcStates(const proto::NpcState* s, int n);          // game thread (owner)
int TakeNpcStates(proto::NpcState* out, int max);             // game thread: latest from the owner
void PushNpcAction(const proto::NpcActionMsg& m);              // any thread (owner)
bool TakeNpcAction(proto::NpcActionMsg* m);                    // game thread
/* (08.10) the actor core (actors.cpp): its packets out (false = queue full) and in (out holds 2048 bytes) */
extern volatile const char* g_gameStep;      // (08.10) diagnostics: the game thread's current step
extern volatile u32 g_hookIn[4], g_hookOut[4]; // 0 Actor::Update, 1 PerformAction
bool ServerClock(proto::ClockMsg* out, u64* atMs, u32* seq);   // (08.10) the campaign's clock from the server
bool PushActorPacket(u16 type, const void* data, u16 size);
bool TakeActorPacket(u16* type, u8* out, u16* size);

}  // namespace coop
