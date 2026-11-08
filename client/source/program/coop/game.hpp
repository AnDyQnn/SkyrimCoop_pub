#pragma once
#include "common.hpp"
#include "protocol.hpp"

/* Game side of the co-op module. TESV.nss is stripped, so everything is located at runtime
   from Papyrus native registrations: the native's name string is found in .rodata, the code
   that passes it to the NativeFunction constructor is found in .text (ADRP+ADD), and the
   function pointer loaded right after it is the native. Small natives are then decoded
   (e.g. GetPositionY = "ldr s0, [x2, #0x58]") to learn field offsets and engine functions.
   Nothing is hard-coded per game version. */
namespace coop::game {

struct Resolved {
    uintptr_t playerSlot;       // GOT slot -> &g_thePlayer
    uintptr_t notify;           // DebugNotification(const char* msg, const char* sound, bool cancelIfQueued)
    uintptr_t getWorldspace;    // TESWorldSpace* TESObjectREFR::GetWorldspace(TESObjectREFR*)
    u32 offPos;                 // NiPoint3 position in TESObjectREFR
    u32 offRotZ;                // heading (radians) in TESObjectREFR
    u32 offCell;                // TESObjectCELL* parentCell
    u32 offFormId;              // TESForm::formID
    u32 offActorState;          // u16 with the weapon state bits (IsWeaponDrawn)
    u32 vtIsDead;               // Actor vtable offset of IsDead(bool)
};

/* Locates everything; false = some piece is missing (the module then stays network-only). */
bool Resolve();
const Resolved& Get();
bool Ready();

void* Player();
void Notify(const char* msg);

/* Installs the once-per-frame hook (controller polling). */
void InstallFrameHook();

/* Helpers for the other modules (menu, console). */
bool Readable(const void* p, u32 len);
void* WorldOf(void* ref);                           // its worldspace (null in an interior)           // mapped and readable for len bytes (svcQueryMemory; 02.10)
uintptr_t ModuleAddr(u32 offset);                  // TESV.nss base + offset
uintptr_t Checked177(u32 offset, const u32* sig);  // address if the first 4 instructions match, else 0
void* Singleton(uintptr_t gotSlot);
void* RefFromHandle(u32 handle);                     // a reference handle -> TESObjectREFR*, null if gone                 // GOT slot -> global -> object
void* GameAlloc(u64 size);                          // the game's own allocator
const char* DistanceText(const proto::State& s);    // "12 м" / "в другом месте" (static buffer)
u32 PuppetFormId(u16 playerId);                     // FormID of that player's puppet reference, 0 if none
void* PuppetRef(u16 playerId);                      // that player's puppet reference, nullptr if none
u16 PuppetOwner(const void* ref);                   // player id of a puppet reference, 0 if not one
void PuppetAi(void* ref, bool on);                  // game thread: AI on/off for a puppet
const float* RefPos(const void* ref);                // x, y, z of a reference
void FaceTowards(void* ref);
/* zones (npcpos.cpp), game thread: high-process actors; one described as the owner sees it; our copy following
   the owner's state; released to its own AI */
int HighActors(void** out, int max);
bool NpcDescribe(void* ref, proto::NpcState* s);
void NpcFollow(void* ref, const proto::NpcState& s, bool first, bool movingChanged, float direction);
void NpcRelease(void* ref);
void* PlaceObject(void* base, float x, float y, float z, bool persist = false);   // game thread: a new reference (held)
void DeleteRef(void* ref);                                 // game thread                        // game thread, test aid: turn the player to it
void* DataHandler();
/* game thread: the player is in a place and has been there a while (3D loaded, same cell for 5 s) — no load
   going on. Natives that lock the actor's data must only run then: during a load the loader holds those locks
   and waits for the thread we would block (seen: the game froze on coc / save load). */
bool Settled();
bool SendAnimEvent(void* ref, const char* event);  // true = the graph took it
void Press(u64 buttons, u32 ms);                    // the pad: these buttons held for ms (bit 1 = B)  // game thread: an animation graph event (idles by their event name)
int PluginIndex(const char* file);                  // load-order index of a plugin (FormID high byte), -1 if not loaded                                // TESDataHandler (nullptr until found)
u16 RealLevel(uintptr_t getLevel, void* player);   // game thread: our own level, not the raised world level
void* FakeVm();                                     // no-op VM for natives that queue a task
bool KillActor(u32 formId);                         // game thread: kill a loaded actor (goes out like any death)
/* Teleport the player to a cell / world position (next game-thread pump). */
void TeleportTo(u32 cell, u32 world, float x, float y, float z, const char* arrived = "Кооп: ты в доме клана");
/* Where the player stands: cell, world FormIDs and position. */
bool WhereAmI(u32* cell, u32* world, float* pos);
/* Loaded plugins in load order, one per line; false until the game has loaded them. */
bool PluginList(char* out, u32 size);
/* (08.10) the actor core (actors.cpp), game thread */
bool ForcePosition(void* ref, const float* pos);       // Actor::SetPosition: the reference, its 3D and its controller
void SetHeading(void* ref, float rot);
float Heading(const void* ref);
void DontMove(void* ref, bool on);                     // Actor.SetDontMove: its AI does not walk it
bool IsDeadActor(void* ref);
void PlaceOf(void* ref, u32* cell, u32* world);        // its cell and worldspace FormIDs
u32 RefFormId(const void* ref);

}  // namespace coop::game
