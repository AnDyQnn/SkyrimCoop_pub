#pragma once
#include "common.hpp"

/* The character as a thing of its own ("avatar"): look, level, skills, perks and perk points, spells, shouts
   and words, things carried — kept on the server under the player's name, put onto any save / session. */
namespace coop::avatar {

bool Resolve();
void Pump();                  // game thread (VM pump)

/* any thread (menu, console): the work runs on the next game-thread pump */
void RequestSave();           // profile of the current character -> server
void RequestLoad();           // server profile -> the current character
const char* Status();

}  // namespace coop::avatar
