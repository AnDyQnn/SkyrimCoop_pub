#pragma once
#include "common.hpp"

/* Shared followers: any player can tell a follower whom to follow (see followers.cpp). */
namespace coop::followers {

bool Resolve();
bool Available();

/* Followers you have (or share): FormIDs, names, current leader player id (0 = nobody set). */
int List(u32* ids, char (*names)[40], u16* leaders, int max);
void Command(u32 formId, u16 leader);        // menu: make it follow `leader` (0 = release)
void Received(u32 formId, u16 leader);       // network thread
void Pump();                                  // game thread
/* horses (03.10): one owner too — mounted = claimed; somebody else's horse cannot be mounted */
bool OwnedByOther(u32 formId);
void Claim(u32 formId);

}  // namespace coop::followers
