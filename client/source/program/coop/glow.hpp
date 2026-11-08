#pragma once
#include "common.hpp"

/* Armour the player enchanted himself glows like the pre-enchanted pieces (see glow.cpp). */
namespace coop::glow {

void Pump();       // game thread
int TestPaint(u32 layer, int colour);   // test aid: paint a worn layer (console glowtest)

}  // namespace coop::glow
