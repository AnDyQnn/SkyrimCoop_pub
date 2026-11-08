#pragma once
#include "common.hpp"

/* Things on the ground shared by the party (items.cpp). */
namespace coop::items {

void Pump();                                  // game thread, online
void Received(const u8* p, u32 size);         // network thread: WORLD / WK_ITEM
bool PickedUp(void* ref);                     // game thread: the player activated this reference

}  // namespace coop::items
