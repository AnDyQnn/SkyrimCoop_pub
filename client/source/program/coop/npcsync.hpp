#pragma once
#include "common.hpp"

/* Shared NPC / enemy health (see npcsync.cpp). */
namespace coop::npcsync {

bool Resolve();
void Noticed(u32 formId);                 // any thread: this actor did something (action hook)
void Received(u32 formId, float amount);  // network thread: another player damaged it
void Pump();
void PumpTest();                   // (08.10) only the console's damage test aid                              // game thread
void TestDamage(u32 formId, float amount);   // console: damage here (as if you hit it)

}  // namespace coop::npcsync
