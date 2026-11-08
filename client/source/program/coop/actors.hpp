#pragma once
#include "common.hpp"
#include "protocol.hpp"

/* The actor core, the Skyrim Together way (notes/ST_РАЗБОР.md): every loaded actor has one owner console; on the
   others it is remote — held back and moved from the owner's snapshots. See actors.cpp. */
namespace coop::actors {

bool Resolve();                                   // finds the functions, installs the Actor::Update hook
void Pump();                                      // game thread (VM pump), every update
bool IsRemote(const void* actor);                 // any thread: its own actions must not run here
bool OwnedSid(const void* actor, u32* sid);       // any thread: an actor we own (its actions go out)
void SendAction(u32 sid, const proto::ActionMsg& m);
bool DrivesPuppet(const void* ref);               // the puppet of a player is moved by the core (not game.cpp)
void EquipOverride(bool on);                      // game thread: our own equip calls on remote actors (puppets, NPC copies) pass

}  // namespace coop::actors
