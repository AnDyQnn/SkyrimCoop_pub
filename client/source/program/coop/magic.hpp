#pragma once
#include "common.hpp"

/* Spells another player casts, seen on their puppet: the projectile / beam / impact art, not the effect. */
namespace coop::magic {

bool Resolve();
/* game thread: the local player released a spell from this hand (0 left, 1 right) -> the event to send */
void Released(u32 hand);
/* game thread: another player's release, on its puppet */
void CastOn(void* puppet, u32 hand, u32 spellId);
/* bows: the local player's arrow left the bow (game thread) / another player's arrow from its puppet */
void Shot();
/* shouts: the local player's shout left (game thread) -> the variation's spell goes out like a cast */
void Shouted();
void FireOn(void* puppet, u32 bowId, u32 arrowId);
void TestFire(void* source, u32 bowId, u32 arrowId, bool copy);   // console test aid
void Pump();                         // game thread: the player's magic casters (any camera)

}  // namespace coop::magic
