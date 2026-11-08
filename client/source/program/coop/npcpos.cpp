#include "npcpos.hpp"
#include "quests.hpp"
#include "client.hpp"
#include "equip.hpp"
#include "game.hpp"
#include "lib.hpp"
#include <nn/os.hpp>
#include <cmath>

/* One world for the room, MMO style: the server splits it into zones and names the owner of each (ZONE). The
   owner's console is the one whose NPCs are real — it sends where they are, how they face, walk, hold weapons
   (NPC_STATE, five times a second, those within 6000 units). The others in the zone put their copies of the same
   NPCs there (game::NpcFollow: Actor::SetPosition, walking animation, weapon; SetDontMove keeps their own AI from
   walking them away). When the owner leaves, the server names another one and our copies go back to our AI. */
namespace coop::npcpos {
namespace {

constexpr int MAX = proto::NPC_MAX;
constexpr float RADIUS = 6000.0f;

/* owner side: last positions to tell walking and its speed */
struct Seen { u32 id; float x, y; u64 ms; };
Seen g_seen[MAX];

/* the others' side: the NPCs we move for the owner */
struct Ctl { u32 id; u64 lastMs; bool moving; float dir; };
Ctl g_ctl[MAX];

Seen* SeenOf(u32 id) {
    Seen* free = nullptr;
    for (auto& s : g_seen) { if (s.id == id) return &s; if (!s.id && !free) free = &s; }
    return free;
}

void ReleaseAll() {
    for (auto& c : g_ctl)
        if (c.id) { game::NpcRelease(equip::Lookup(c.id)); c = Ctl{}; }
}

}  // namespace

void Pump() {
    void* player = game::Player();
    if (!player || Status() != NetStatus::Connected) { ReleaseAll(); return; }
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    u16 owner = ZoneOwner(), me = MyId();
    /* (03.10) in our intro the scene (cart, Helgen) is ours alone: nobody else drives its NPCs — a zone owner who
       sent nothing for them stopped the cart's prisoners in a test */
    if (!quests::IntroDone() && owner != me) { ReleaseAll(); TakeNpcStates(nullptr, 0); return; }
    if (owner && owner == me) {
        ReleaseAll();                                          // ours are the real ones now
        static u64 next;
        if (now < next) return;
        next = now + 200;
        u32 cell, world; float pos[3];
        if (!game::WhereAmI(&cell, &world, pos)) return;
        void* actors[256];
        int n = game::HighActors(actors, 256);
        proto::NpcState out[MAX];
        int k = 0;
        for (int i = 0; i < n && k < MAX; i++) {
            proto::NpcState s;
            if (!game::NpcDescribe(actors[i], &s)) continue;
            float dx = s.x - pos[0], dy = s.y - pos[1];
            if (dx * dx + dy * dy > RADIUS * RADIUS) continue;
            if (Seen* w = SeenOf(s.formId)) {
                if (w->id == s.formId && now > w->ms) {
                    float mx = s.x - w->x, my = s.y - w->y;
                    s.speed = sqrtf(mx * mx + my * my) * 1000.0f / (float)(now - w->ms);
                    if (s.speed > 15.0f && s.speed < 2000.0f) s.flags |= proto::NPC_MOVING;
                    else s.speed = 0.0f;
                }
                *w = Seen{s.formId, s.x, s.y, now};
            }
            out[k++] = s;
        }
        if (k) PushNpcStates(out, k);
        static u64 nextLog;
        if (now >= nextLog) { nextLog = now + 10000; Debug("zone: we own it, %d NPCs of %d actors sent", k, n); }
        return;
    }
    /* someone else owns the zone: follow its NPCs */
    proto::NpcState in[MAX];
    int n = TakeNpcStates(in, MAX);
    for (int i = 0; i < n; i++) {
        const auto& s = in[i];
        if (s.flags & proto::NPC_DEAD) continue;               // deaths come with the kills
        auto ref = static_cast<u8*>(equip::Lookup(s.formId));
        if (!ref || ref[0x1A] != 0x3E || !*reinterpret_cast<void**>(ref + 0x68) || game::PuppetOwner(ref)) continue;
        Ctl* c = nullptr;
        for (auto& x : g_ctl) if (x.id == s.formId) c = &x;
        bool first = !c;
        if (!c) for (auto& x : g_ctl) if (!x.id) { c = &x; break; }
        if (!c) continue;
        if (first) *c = Ctl{s.formId, now, false, 0.0f};
        const float* cur = game::RefPos(ref);
        float mx = s.x - cur[0], my = s.y - cur[1];
        if (mx * mx + my * my > 4.0f) {
            float d = (atan2f(mx, my) - s.rot) / 6.2831853f;
            c->dir = d - floorf(d);
        }
        bool moving = s.flags & proto::NPC_MOVING;
        game::NpcFollow(ref, s, first, moving != c->moving, c->dir);
        c->moving = moving;
        c->lastMs = now;
    }
    if (n > 0) {
        static u64 nextLog;
        if (now >= nextLog) { nextLog = now + 10000; Debug("zone: owner is %u, following %d NPCs", owner, n); }
    }
    for (auto& c : g_ctl)                                      // no news for 3 s: ours again
        if (c.id && now - c.lastMs > 3000) { game::NpcRelease(equip::Lookup(c.id)); c = Ctl{}; }
}

}  // namespace coop::npcpos
