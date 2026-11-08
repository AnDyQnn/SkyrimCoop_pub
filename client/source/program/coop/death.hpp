#pragma once
#include "common.hpp"

/* Death in the online world: knock-down and get up at the door you came in by / the nearest city (death.cpp). */
namespace coop::death {
void Pump();          // game thread (VM pump), online build
/* (07.10) knock-down: 90 s on the ground, a friend's A gets you up, or give up / time out -> the usual get-up */
bool IsDown();                    // any thread
void GiveUp();                    // any thread (the co-op menu)
void RevivedBy(u16 playerId);     // game thread (a "cooprevive" event for us)
bool ReviveFriend(u16 playerId);
void Healed(u16 byPlayer, float amount);   // game thread: a friend's restore-health spell reached us  // game thread (A on a friend's puppet): true = they were down, the revive went out
}  // namespace coop::death
