#pragma once
#include "common.hpp"

/* Spell scrolls that stay after reading (see scrolls.cpp). */
namespace coop::scrolls {

bool Resolve();
void Pump();       // game thread
void AddDays(float n);
void WorldBossTest(int k, u32 marker);   // test aid: boss k's place at this reference   // test aid: the smuggler's calendar moved on

}  // namespace coop::scrolls
