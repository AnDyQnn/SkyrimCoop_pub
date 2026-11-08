#pragma once
#include "common.hpp"
#include "protocol.hpp"

/* Player actions (attack, block, draw, sneak, jump, shout, idles...) the Skyrim Together way; see actions.cpp. */
namespace coop::actions {

bool Resolve();
bool Available();
void Pump();          // game thread: other players' actions on their puppets
u8 Replay(void* actor, const proto::ActionMsg& m);   // game thread: an action forced on an actor (owner's / player's)

}  // namespace coop::actions
