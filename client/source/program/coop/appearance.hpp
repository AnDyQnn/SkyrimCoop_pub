#pragma once
#include "common.hpp"

/* The other player's real character (race, sex, face, name) the way Skyrim Together does it:
   the sender serialises its player NPC base with TESForm::SaveGame, the receiver duplicates its own
   player base (CreateDuplicateForm) and loads those bytes into the copy with TESForm::LoadGame; the
   puppet is then spawned from that copy. FormIDs inside are written raw (both consoles run the same
   build and plugins), so the save-game refID table is bypassed on our thread (1.1.177, checked). */
namespace coop::appearance {

bool Resolve();
bool Available();

/* game thread (VM pump): serialise the local player base if it changed; true + bytes when there is
   something new to send. */
bool Collect(const void** data, u32* size, u32* flags);

/* network thread: bytes received for player `id` (stored, applied later on the game thread). */
void Received(u16 id, u32 flags, const void* data, u32 size);

/* game thread (VM pump): builds NPC bases for players whose appearance arrived. */
void Pump();

/* game thread: the player's own NPC base saved with these change flags (0 = failed) / loaded back from such bytes
   (the avatar, avatar.cpp). */
u32 SavePlayer(u8* out, u32 max, u32 flags);
bool LoadPlayer(const u8* data, u32 size, u32 flags);

/* NPC base to spawn player `id`'s puppet from; nullptr = not built yet. */
void* BaseFor(u16 id);
/* ActorBase.SetEssential on a puppet base (a puppet that has to die with its player) */
void SetEssential(void* base, bool on);
void Forget(u16 id);
/* game thread: the name shown over that player's puppet ("[34] AnDy"); false until their look is built */
bool SetTitle(u16 id, const char* title);

}  // namespace coop::appearance
