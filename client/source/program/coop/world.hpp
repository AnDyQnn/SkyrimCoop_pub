#pragma once
#include "common.hpp"

/* The server owns the world (03.10, notes/АРХИТЕКТУРА_МИРА.md): what was taken out of / put into containers and
   corpses lives on the server; every console brings its copy of a container to the server's state (world.cpp). */
namespace coop::world {

void Install();                   // online: the Game.TeachWord hook (words for the party)
void DragonDied(void* actor);     // game thread: an actor died here — a dragon gives the zone a soul
void* RaceOf(void* actor);       // the actor's RACE form (from its NPC base), or null
void Touched(void* ref);
void Activated(void* ref);        // game thread: the player used this reference (a lever, a chain, a button: the others too)          // game thread: the player activated this reference (a container, a corpse...)
void Pump();                      // game thread
void Received(const u8* p, u32 size);   // network thread: a WORLD packet from the server

}  // namespace coop::world
