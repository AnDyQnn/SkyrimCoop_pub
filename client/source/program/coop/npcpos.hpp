#pragma once
#include "common.hpp"

/* Zones: the server-named owner's NPCs are the real ones, the others follow them (npcpos.cpp). */
namespace coop::npcpos {

void Pump();                  // game thread

}  // namespace coop::npcpos
