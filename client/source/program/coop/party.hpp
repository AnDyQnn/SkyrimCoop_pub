#pragma once
#include "common.hpp"

/* The other players on the world map and the compass: quest markers of SwitchPatch SSW_Party (party.cpp). */
namespace coop::party {

void Pump();                      // game thread, online
void MarkReceived(const u8* p, u32 size);   // network thread: a friend's map marker (WORLD / WK_MARK)

}  // namespace coop::party
