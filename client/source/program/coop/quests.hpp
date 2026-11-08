#pragma once
#include "common.hpp"

/* Shared quest progress (see quests.cpp). */
namespace coop::quests {

bool Resolve();
void Received(u32 questId, u16 stage);   // network thread
void Pump();
bool SetStage(void* quest, u16 stage);   // game thread: through the hook (the stage goes out like any other)
bool IntroDone();                        // game thread: the intro (cart, Helgen, MQ101) is over                             // game thread
void SetLocal(u32 questId, u16 stage);
void Scan();                             // test aid (game thread)   // console: as if the game set it here (goes out)

}  // namespace coop::quests
