#pragma once
#include "common.hpp"

/* Damage between players (PvP rooms) and friendly puppets (see pvp.cpp). */
namespace coop::pvp {

bool Resolve();
void Pump();                                  // game thread
void Received(u16 fromId, float amount);      // network thread: another player hit you
void HealPlayer();                            // game thread: health back to full (console "heal")

}  // namespace coop::pvp
