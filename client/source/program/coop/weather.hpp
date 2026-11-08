#pragma once
#include "common.hpp"

/* The room host's weather on every console (see weather.cpp). */
namespace coop::weather {

bool Resolve();
void Received(u32 weatherId);   // network / event thread: the host's weather
void Pump();                    // game thread

}  // namespace coop::weather
