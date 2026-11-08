#pragma once
#include "common.hpp"

/* Horse riding for the puppets (see mount.cpp). */
namespace coop::mount {

bool Resolve();
void Received(u16 playerId, u32 horseBase);   // game thread: "coophorse <base>" from that player (0 = on foot)
void Pump();                                  // game thread
void* Carrier(void* puppet);                  // game thread: what moves for this puppet (its horse while riding)
void ActorAction(void* actor, const char* name);   // actions.cpp: HorseEnter / HorseExit of anyone (only flags set)
bool Command(const char* command);            // game thread, test aids: horse / ride / dismount / mounted

}  // namespace coop::mount
