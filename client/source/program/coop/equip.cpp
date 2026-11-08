#include "equip.hpp"
#include "actors.hpp"
#include "client.hpp"
#include "game.hpp"
#include <nn/os.hpp>
#include <cstring>
#include <cstdio>

namespace coop::equip {
namespace {

struct Fn { u32 off; u32 sig[4]; };
/* 1.1.177 (tools/xrefs.py):
     GetInventoryChanges 0x3521c(ExtraDataList* at ref+0x70) -> InventoryChanges* (+0: list of entries)
     list node = {item, next}; InventoryEntryData = {TESBoundObject* object, list<ExtraDataList*>* extraLists}
     ExtraDataList: presence bits at +0x10 (type t = byte t/8, bit t%8): Worn 0x16, WornLeft 0x17
     LookupFormByID 0xcb70c(u32)
     Actor.EquipItem native 0x9fd850(vm, stack, actor, form, preventRemoval, silent): VM only for errors
     Actor.UnequipAll native 0xa04c50(vm, stack, actor) -> ActorEquipManager::UnequipAll */
constexpr Fn GET_CHANGES{0x3521c, {0xA9BD7BFD, 0xA90157F6, 0x910003FD, 0xA9024FF4}};
constexpr Fn LOOKUP_FORM{0xcb70c, {0xA9BD7BFD, 0xF9000BF5, 0x910003FD, 0xA9024FF4}};
constexpr Fn EQUIP_ITEM{0x9fd850, {0xD101C3FF, 0xA9027BFD, 0x910083FD, 0xA90367FA}};
constexpr Fn UNEQUIP_ALL{0xa04c50, {0xD000BA08, 0xF941A508, 0xF9400100, 0xAA0203E1}};
/* ObjectReference.RemoveAllItems native (vm, stack, ref, transferTo, keepOwnership, removeQuest): no VM use.
   The puppet's inventory holds only what the player wears, so its AI cannot take out a torch at night. */
constexpr Fn REMOVE_ALL{0xa547e0, {0xA9BD7BFD, 0xA90157F6, 0x910003FD, 0xA9024FF4}};
/* ObjectReference.RemoveItem native (vm, stack, ref, form, count, silent, otherContainer): queues a task
   that keeps a reference on the VM (our no-op one) */
constexpr Fn REMOVE_ITEM{0xa54bb0, {0xD101C3FF, 0xA9017BFD, 0x910043FD, 0xF90013FB}};

/* ActorEquipManager::EquipObject (mgr, actor, form, extra, count, slot, queue, force, sounds, applyNow): the
   EquipItem native passes slot 0; a left-hand weapon needs the LeftHand equip slot (0x13F43).
   Actor.EquipSpell native (vm, stack, actor, spell, source 0 left / 1 right / 2 voice): VM only for errors.
   Selected spells: actor + 0x1F8 + source * 8 (GetEquippedSpell native). */
constexpr Fn EQUIP_OBJECT{0x67a9f8, {0xD10243FF, 0xA9037BFD, 0x9100C3FD, 0xA9046FFC}};
constexpr Fn EQUIP_SPELL{0x9fddc0, {0x2A0103E8, 0xB4000143, 0x7100089F, 0x54000168}};
constexpr u32 EQUIP_MANAGER_GOT = 0x2146348, LEFT_HAND_SLOT = 0x13F43, SELECTED_SPELLS = 0x1F8;
uintptr_t g_getChanges, g_lookup, g_equipItem, g_unequipAll, g_removeAll, g_removeItem, g_equipObject, g_equipSpell;
bool g_ok;

/* Stand-in VM for the natives: only their error paths touch it. */
u64 Nop() { return 0; }
const void* g_vmTable[64];
struct { const void* const* vtable; u32 refcount; u32 pad; } g_vm{g_vmTable, 1u << 28, 0};

template <class T> T At(const void* p, u32 off) { return *reinterpret_cast<const T*>(static_cast<const u8*>(p) + off); }

/* ---- sending */
Item g_out[MAX_ITEMS];
u32 g_lastHash;
u64 g_lastCollect;

/* ---- receiving */
struct Entry { u16 id; bool dirty; int count; Item items[MAX_ITEMS]; void* applied; u64 appliedMs; bool again; u64 equipAt, checkAt; int pendingCount; Item pending[MAX_ITEMS]; u64 lightCheckMs; void* cleaned; };
Entry g_entries[MAX_REMOTE];
nn::os::MutexType g_lock;
bool g_lockReady;
void Lock() { if (!g_lockReady) { nn::os::InitializeMutex(&g_lock, false, 0); g_lockReady = true; } nn::os::LockMutex(&g_lock); }
void Unlock() { nn::os::UnlockMutex(&g_lock); }

}  // namespace

bool Resolve() {
    g_getChanges = game::Checked177(GET_CHANGES.off, GET_CHANGES.sig);
    g_lookup = game::Checked177(LOOKUP_FORM.off, LOOKUP_FORM.sig);
    g_equipItem = game::Checked177(EQUIP_ITEM.off, EQUIP_ITEM.sig);
    g_unequipAll = game::Checked177(UNEQUIP_ALL.off, UNEQUIP_ALL.sig);
    g_removeAll = game::Checked177(REMOVE_ALL.off, REMOVE_ALL.sig);
    g_removeItem = game::Checked177(REMOVE_ITEM.off, REMOVE_ITEM.sig);
    g_equipObject = game::Checked177(EQUIP_OBJECT.off, EQUIP_OBJECT.sig);
    g_equipSpell = game::Checked177(EQUIP_SPELL.off, EQUIP_SPELL.sig);
    g_ok = g_getChanges && g_lookup && g_equipItem && g_unequipAll;
    for (auto& s : g_vmTable) s = reinterpret_cast<const void*>(&Nop);
    return g_ok;
}

bool Available() { return g_ok; }

int WornList(void* actor, Item* out);

bool Collect(const Item** items, int* count) {
    if (!g_ok) return false;
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    if (now - g_lastCollect < 1000) return false;
    g_lastCollect = now;
    void* player = game::Player();
    if (!player) return false;
    int n = WornList(player, g_out);
    if (n < 0) return false;
    for (u32 source = 0; source < 3 && n < MAX_ITEMS; source++) {   // spells in hand, shout / power
        auto spell = *reinterpret_cast<u8**>(static_cast<u8*>(player) + SELECTED_SPELLS + source * 8);
        if (spell && spell[0x1A] == 0x16) g_out[n++] = Item{*reinterpret_cast<u32*>(spell + 0x14), static_cast<u8>(2 + source), {}};
    }
    u32 h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ g_out[i].formId ^ (g_out[i].left << 31)) * 16777619u;
    if (h == g_lastHash) return false;
    g_lastHash = h;
    *items = g_out;
    *count = n;
    Debug("equipment: %d items to send", n);
    return true;
}

/* Items worn (and held in the left hand) by an actor, -1 without inventory data. */
int WornList(void* actor, Item* out) {
    void* changes = reinterpret_cast<void* (*)(void*)>(g_getChanges)(static_cast<u8*>(actor) + 0x70);
    if (!changes) return -1;
    int n = 0;
    for (auto node = At<const u8*>(changes, 0); node && n < MAX_ITEMS; node = At<const u8*>(node, 8)) {
        const u8* entry = At<const u8*>(node, 0);
        if (!entry) continue;
        const void* object = At<const void*>(entry, 0);
        if (!object) continue;
        bool worn = false, left = false;
        for (auto x = At<const u8*>(entry, 8); x; x = At<const u8*>(x, 8)) {
            const u8* xl = At<const u8*>(x, 0);
            const u8* bits = xl ? At<const u8*>(xl, 0x10) : nullptr;
            if (!bits) continue;
            worn |= (bits[2] & 0x40) != 0;
            left |= (bits[2] & 0x80) != 0;
        }
        if (!worn && !left) continue;
        u32 id = At<u32>(object, 0x14);
        if (worn) out[n++] = Item{id, 0, {}};
        if (left && n < MAX_ITEMS) out[n++] = Item{id, 1, {}};
    }
    return n;
}

void Received(u16 id, const Item* items, int count) {
    if (count < 0 || count > MAX_ITEMS) return;
    Lock();
    Entry* e = nullptr;
    for (auto& x : g_entries) if (x.id == id) e = &x;
    for (auto& x : g_entries) if (!e && !x.id) e = &x;
    if (e) { e->id = id; e->count = count; memcpy(e->items, items, count * sizeof(Item)); e->dirty = true; }
    Unlock();
}

void Forget(u16 id) {
    Lock();
    for (auto& e : g_entries) if (e.id == id) e = Entry{};
    Unlock();
}

void PumpImpl();
/* (08.10) puppets are remote actors (actors.cpp blocks their own equipping): our lists for them pass */
void Pump() {
    actors::EquipOverride(true);
    PumpImpl();
    actors::EquipOverride(false);
}

void PumpImpl() {
    if (!g_ok) return;
    for (auto& e : g_entries) {
        if (!e.id) continue;
        void* ref = game::PuppetRef(e.id);
        if (!ref) continue;
        u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
        /* 2. put the list on half a second after stripping: the removal is processed after the call and
              would take away what is equipped in the same frame */
        if (e.equipAt && now >= e.equipAt && ref == e.applied) {
            e.equipAt = 0;
            int done = 0;
            /* left-hand weapons first: taking one into the inventory equips it in the right hand, which must
               not knock out the right-hand weapon put on after it */
            for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < e.pendingCount; i++) {
                if ((pass == 0) != (e.pending[i].left == 1)) continue;
                void* form = reinterpret_cast<void* (*)(u32)>(g_lookup)(e.pending[i].formId);
                if (!form) continue;
                u8 kind = e.pending[i].left, type = static_cast<u8*>(form)[0x1A];
                if (kind >= 2) {                                   // spell: left / right hand or voice
                    if (g_equipSpell && type == 0x16)
                        reinterpret_cast<void (*)(void*, u32, void*, void*, u32)>(g_equipSpell)(&g_vm, 0, ref, form, kind - 2u);
                } else if (kind == 1 && type == 0x29 && g_equipObject) {   // weapon in the left hand
                    void* slot = Lookup(LEFT_HAND_SLOT);
                    void* mgr = game::Singleton(game::ModuleAddr(EQUIP_MANAGER_GOT));
                    /* EquipItem puts it in inventory first: take one in, then move it to the left hand */
                    reinterpret_cast<bool (*)(void*, u32, void*, void*, bool, bool)>(g_equipItem)(&g_vm, 0, ref, form, true, true);
                    if (slot && mgr)
                        reinterpret_cast<void (*)(void*, void*, void*, void*, u32, void*, bool, bool, bool, bool)>(g_equipObject)(
                            mgr, ref, form, nullptr, 1, slot, true, true, false, false);
                } else {
                    /* worn armour, right-hand weapon, shield; torches go to the left by themselves */
                    if (kind == 1 && type != 0x1F) continue;
                    reinterpret_cast<bool (*)(void*, u32, void*, void*, bool, bool)>(g_equipItem)(&g_vm, 0, ref, form, true, true);
                }
                done++;
            }
            e.checkAt = now + 2000;
            Debug("equipment of player %u: %d/%d put on", e.id, done, e.pendingCount);
        }
        if (e.checkAt && now >= e.checkAt && ref == e.applied) {       // what it really wears
            e.checkAt = 0;
            /* the copy of the player's NPC starts with its template's things (potions, gold, lockpicks): the
               puppet keeps only what its player wears, so nothing can be taken from it in the exchange screen */
            if (g_removeItem) {
                void* changes = reinterpret_cast<void* (*)(void*)>(g_getChanges)(static_cast<u8*>(ref) + 0x70);
                int removed = 0;
                for (auto node = changes ? At<const u8*>(changes, 0) : nullptr; node && removed < 32; node = At<const u8*>(node, 8)) {
                    const u8* entry = At<const u8*>(node, 0);
                    void* object = entry ? At<void*>(entry, 0) : nullptr;
                    if (!object) continue;
                    u32 id = At<u32>(object, 0x14);
                    bool keep = false;
                    for (int k = 0; k < e.pendingCount; k++) keep |= e.pending[k].formId == id;
                    if (keep) continue;
                    reinterpret_cast<void (*)(void*, u32, void*, void*, s32, bool, void*)>(g_removeItem)(&g_vm, 0, ref, object, 9999, true, nullptr);
                    removed++;
                }
                if (removed) Debug("puppet %u: removed %d template items", e.id, removed);
            }
            Item worn[MAX_ITEMS];
            int w = WornList(ref, worn);
            char line[160]; int len = 0;
            for (int i = 0; i < w && len < 140; i++) len += snprintf(line + len, sizeof line - len, " %08x%s", worn[i].formId, worn[i].left ? "L" : "");
            for (u32 k = 0; k < 3 && len < 140; k++) {
                auto spell = *reinterpret_cast<u8**>(static_cast<u8*>(ref) + SELECTED_SPELLS + k * 8);
                if (spell) len += snprintf(line + len, sizeof line - len, " spell%u=%08x", k, *reinterpret_cast<u32*>(spell + 0x14));
            }
            line[len] = 0;
            Debug("puppet %u wears %d:%s", e.id, w, line);
        }
        /* its own AI takes out a torch at night: take it away unless the player holds one too */
        if (g_removeItem && now - e.lightCheckMs > 2000 && ref == e.applied && !e.equipAt) {
            e.lightCheckMs = now;
            Item worn[MAX_ITEMS];
            int w = WornList(ref, worn);
            for (int i = 0; i < w; i++) {
                void* form = reinterpret_cast<void* (*)(u32)>(g_lookup)(worn[i].formId);
                if (!form || static_cast<u8*>(form)[0x1A] != 0x1F) continue;
                bool theirs = false;
                for (int k = 0; k < e.pendingCount; k++) theirs |= e.pending[k].formId == worn[i].formId;
                if (theirs) continue;
                reinterpret_cast<void (*)(void*, u32, void*, void*, s32, bool, void*)>(g_removeItem)(&g_vm, 0, ref, form, 99, true, nullptr);
                Debug("puppet %u: took away its torch %08x", e.id, worn[i].formId);
            }
        }
        /* 1. new list, or a new puppet (re-spawned), or once more 3 s after a new puppet appeared: its default
              outfit is put on while its 3D loads */
        bool again = ref == e.applied && e.again && now - e.appliedMs > 3000;
        if (!e.dirty && ref == e.applied && !again) continue;
        Lock();
        e.pendingCount = e.count;
        memcpy(e.pending, e.items, e.count * sizeof(Item));
        e.again = ref != e.applied;
        e.dirty = false;
        e.applied = ref;
        e.appliedMs = now;
        Unlock();
        reinterpret_cast<void (*)(void*, u32, void*)>(g_unequipAll)(&g_vm, 0, ref);
        /* the copy of the player's NPC starts with its template's things (potions, gold, lockpicks): the puppet
           holds only what its player wears, so nobody can loot a puppet in the exchange screen */
        e.equipAt = now + 500;
    }
}

void RemoveAll(void* ref) {
    if (g_removeAll && ref)
        reinterpret_cast<void (*)(void*, u32, void*, void*, bool, bool)>(g_removeAll)(&g_vm, 0, ref, nullptr, false, true);
}

bool Worn(void* actor, const Item** items, int* count) {
    static Item buf[MAX_ITEMS];
    if (!g_ok || !actor) return false;
    int n = WornList(actor, buf);
    if (n < 0) return false;
    *items = buf;
    *count = n;
    return true;
}

void* Lookup(u32 formId) { return g_ok ? reinterpret_cast<void* (*)(u32)>(g_lookup)(formId) : nullptr; }

bool EquipOn(void* actor, void* form) {
    if (!g_ok || !actor || !form) return false;
    reinterpret_cast<bool (*)(void*, u32, void*, void*, bool, bool)>(g_equipItem)(&g_vm, 0, actor, form, false, false);
    return true;
}

bool UnequipAllOn(void* actor) {
    if (!g_ok || !actor) return false;
    reinterpret_cast<void (*)(void*, u32, void*)>(g_unequipAll)(&g_vm, 0, actor);
    return true;
}

}  // namespace coop::equip
