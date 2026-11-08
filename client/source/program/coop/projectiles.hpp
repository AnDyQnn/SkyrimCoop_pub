#pragma once
#include "common.hpp"
#include "protocol.hpp"

/* Projectiles the Skyrim Together way (Projectile.cpp / CombatService): the shooter's console launches the real one and
   sends the launch; the others launch the same projectile from the shooter's copy. See projectiles.cpp. */
namespace coop::projectiles {

bool Resolve();
void Pump();          // game thread: our launches out
void Received(const proto::ProjMsg& m, void* shooter);   // game thread: someone's launch, from its copy here

}  // namespace coop::projectiles
