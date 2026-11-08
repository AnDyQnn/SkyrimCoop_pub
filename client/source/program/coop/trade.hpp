#pragma once
#include "common.hpp"

/* Exchange between players: the other player's puppet opens as a container (the follower "trade" screen).
   Whatever you put into it goes to that player's inventory on their console; their worn things cannot be
   taken out (they are put back). Received items are added to your player. */
namespace coop::trade {

bool Resolve();
bool Available();

void Start(u16 playerId);                    // any thread: opens the exchange on the next game-thread pump
void Pump();                                 // game thread (VM pump)
void Received(u16 fromId, u32 formId, s32 count);   // network thread
void TestPut(u32 formId, s32 count);         // test: as if you put this into the open exchange

/* helpers for chest.cpp (game thread): add / remove (n < 0) items, a reference's inventory changes
   (FormID, count delta), activation as the player */
void AddItems(void* ref, void* form, s32 n, bool silent = false);
struct Count { u32 id; s32 n; };
int Changes(void* ref, Count* out, int max);
void ActivateAsPlayer(void* ref);
void ActivateAs(void* ref, void* activator);       // game thread: the activation by another actor (a player's puppet)

}  // namespace coop::trade
