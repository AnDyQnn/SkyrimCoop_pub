#pragma once
#include "common.hpp"

/* What the other player wears and holds. The sender walks the player's inventory (InventoryChanges ->
   entries -> extra lists with the Worn / WornLeft bit) on the game thread; the receiver strips the
   puppet (ActorEquipManager::UnequipAll) and equips the list with Actor.EquipItem, which also puts a
   missing item into the puppet's inventory (1.1.177, checked). */
namespace coop::equip {

/* left: 0 worn / right hand, 1 left hand item, 2 spell in the left hand, 3 spell in the right hand,
   4 voice (shout / power) */
struct Item { u32 formId; u8 left; u8 pad[3]; };
static_assert(sizeof(Item) == 8);
constexpr int MAX_ITEMS = 40;

bool Resolve();
bool Available();

/* game thread: the local player's worn list if it changed (checked once a second). */
bool Collect(const Item** items, int* count);

/* network thread: list received for player `id`. */
void Received(u16 id, const Item* items, int count);
void Forget(u16 id);

/* game thread: applies lists to puppets (again whenever a puppet is re-spawned). */
void Pump();

/* Console helpers (game thread): FormID -> form, equip on / strip an actor. */
void* Lookup(u32 formId);
/* game thread: what an actor wears now (a static buffer) */
bool Worn(void* actor, const Item** items, int* count);
/* game thread: ObjectReference.RemoveAllItems(ref) to nowhere */
void RemoveAll(void* ref);
bool EquipOn(void* actor, void* form);
bool UnequipAllOn(void* actor);

}  // namespace coop::equip
