#include "glow.hpp"
#include "client.hpp"
#include "equip.hpp"
#include "game.hpp"
#include "trade.hpp"
#include "lib.hpp"
#include <nn/os.hpp>
#include <cstdio>
#include <cstring>

/* The pre-enchanted armour of the game glows through its own models (SwitchPatch: armorglow/build.py). Armour the
   player enchants himself is the plain piece with an enchantment in its extra data, so its model cannot change —
   instead it gets a glow layer: a non-playable armour of SwitchPatch in a free biped slot (56..60) whose model is the
   piece's glowing copy with the piece itself hidden. While an enchanted piece is worn its layer is worn too, and the
   layer's glow shapes get the colour of the enchantment (their effect shader's base colour, found in memory by the
   dim white the layers are built with).

   1.1.177 layouts (seen with the console's scene / extras dumps, 01.10):
     ExtraDataList: +0 vtable, +8 the chain of BSExtraData {+0 vtable, +8 next, +0x10 data}, +0x10 presence bits;
     ExtraEnchantment (type 0x9B: bits[0x13] & 8): its data +0x10 is the EnchantmentItem (form type 0x15);
     Get3D: vtable +0x378; NiObjectNET name +0x10; NiAVObject flags +0xF4; NiNode children: data +0x118, u16 +0x120. */
namespace coop::glow {
namespace {

struct Layer { const char* plugin; u32 obj; u32 layer; };
const Layer LAYERS[] = {
#define GLOW_LAYERS
#include "glow_layers.inc"
#undef GLOW_LAYERS
};
struct Effect { const char* plugin; u32 obj; int colour; };
const Effect EFFECTS[] = {
#define GLOW_EFFECTS
#include "glow_layers.inc"
#undef GLOW_EFFECTS
};
constexpr int NL = sizeof LAYERS / sizeof LAYERS[0], NE = sizeof EFFECTS / sizeof EFFECTS[0];
/* armorglow/build.py COLOURS: fire, frost, shock, health, magicka, stamina, gold */
constexpr float RGB[7][3] = {{1.0f, 0.42f, 0.08f}, {0.30f, 0.80f, 1.0f}, {0.70f, 0.40f, 1.0f}, {1.0f, 0.18f, 0.22f},
                             {0.25f, 0.50f, 1.0f}, {0.30f, 1.0f, 0.40f}, {1.0f, 0.78f, 0.30f}};
constexpr float LAYER_RGB[3] = {0.6f, 0.62f, 0.66f};
constexpr u32 GET_CHANGES = 0x3521c, EQUIP_ITEM = 0x9fd850, UNEQUIP_ITEM = 0xa04c70;

u32 g_base[NL], g_layer[NL], g_mgef[NE];
bool g_ready;
int g_patch = -1;
u32 g_worn[8];          // layers we put on
int g_wornColour[8];
int g_wornN;
u64 g_next;

template <class T> T At(const void* p, u32 off) { return *reinterpret_cast<const T*>(static_cast<const u8*>(p) + off); }
bool HeapPtr(const void* p) {
    auto v = reinterpret_cast<uintptr_t>(p);
    return v >= 0x1000000000ull && v < 0x8000000000ull && !(v & 7) && game::Readable(p, 0x200);
}
u8 Type(const void* f) { return At<u8>(f, 0x1A); }

void Setup() {
    g_ready = true;
    g_patch = game::PluginIndex("SwitchPatch.esp");
    for (int i = 0; i < NL; i++) {
        int idx = game::PluginIndex(LAYERS[i].plugin);
        g_base[i] = idx >= 0 ? ((u32)idx << 24) | LAYERS[i].obj : 0;
        g_layer[i] = g_patch >= 0 ? ((u32)g_patch << 24) | LAYERS[i].layer : 0;
    }
    for (int i = 0; i < NE; i++) {
        int idx = game::PluginIndex(EFFECTS[i].plugin);
        g_mgef[i] = idx >= 0 ? ((u32)idx << 24) | EFFECTS[i].obj : 0;
    }
    Log("свечение своих зачарований: слоёв %d, эффектов %d", NL, NE);
}

/* the colour of an enchantment: its first effect we know (EnchantmentItem: an array of Effect* whose +0x10 is the
   MGEF, form type 0x12 — found by its shape, the offset is not fixed by us) */
int ColourOf(const u8* ench) {
    for (u32 o = 0x30; o <= 0x90; o += 8) {
        auto arr = At<void* const*>(ench, o);
        if (!HeapPtr(arr)) continue;
        u32 n = At<u32>(ench, o + 8);
        if (n == 0 || n > 16) n = At<u32>(ench, o + 0x10);
        if (n == 0 || n > 16) continue;
        for (u32 k = 0; k < n; k++) {
            auto eff = static_cast<const u8*>(arr[k]);
            if (!HeapPtr(eff)) break;
            auto mgef = At<const u8*>(eff, 0x10);
            if (!HeapPtr(mgef) || Type(mgef) != 0x12) break;
            u32 id = At<u32>(mgef, 0x14);
            for (int i = 0; i < NE; i++) if (g_mgef[i] == id) return EFFECTS[i].colour;
        }
    }
    return 6;                                   // gold: an effect we don't know
}

/* the player's worn pieces that carry their own enchantment -> (layer, colour) */
int Wanted(void* player, u32* layers, int* colours, int max) {
    void* changes = reinterpret_cast<void* (*)(void*)>(game::ModuleAddr(GET_CHANGES))(static_cast<u8*>(player) + 0x70);
    if (!changes) return 0;
    int n = 0;
    for (auto node = At<const u8*>(changes, 0); node && n < max; node = At<const u8*>(node, 8)) {
        const u8* entry = At<const u8*>(node, 0);
        if (!entry) continue;
        const u8* object = At<const u8*>(entry, 0);
        if (!object) continue;
        u32 id = At<u32>(object, 0x14);
        int li = -1;
        for (int i = 0; i < NL; i++) if (g_base[i] == id) { li = i; break; }
        if (li < 0) continue;
        for (auto x = At<const u8*>(entry, 8); x; x = At<const u8*>(x, 8)) {
            const u8* xl = At<const u8*>(x, 0);
            const u8* bits = xl ? At<const u8*>(xl, 0x10) : nullptr;
            if (!bits || !(bits[2] & 0x40) || !(bits[0x13] & 0x08)) continue;      // worn, enchanted
            for (auto d = At<const u8*>(xl, 8); HeapPtr(d); d = At<const u8*>(d, 8)) {
                auto ench = At<const u8*>(d, 0x10);
                if (HeapPtr(ench) && Type(ench) == 0x15) {
                    layers[n] = g_layer[li];
                    colours[n++] = ColourOf(ench);
                    break;
                }
            }
            break;
        }
    }
    return n;
}

/* colour the glow shapes of a worn layer: the node named after the layer's FormID, its shapes ":SSWGlow" */
bool FindFloats(u8* p, int span, float** at) {
    if (!game::Readable(p, span)) return false;
    for (int o = 0; o + 12 <= span; o += 4) {
        auto f = reinterpret_cast<float*>(p + o);
        if (f[0] > 0.59f && f[0] < 0.61f && f[1] > 0.61f && f[1] < 0.63f && f[2] > 0.65f && f[2] < 0.67f) { *at = f; return true; }
    }
    return false;
}

const char* NameOf(const void* obj) {
    if (!HeapPtr(obj)) return nullptr;
    auto s = At<const char*>(obj, 0x10);
    return HeapPtr(s) ? s : nullptr;
}

int Paint(const void* node, const char* tag, bool inLayer, const float* rgb, int depth) {
    if (!HeapPtr(node) || depth > 8) return 0;
    const char* nm = NameOf(node);
    if (nm && strstr(nm, tag)) inLayer = true;
    int done = 0;
    if (inLayer && nm && strstr(nm, ":SSWGlow")) {
        /* the shape's properties, then their material: where the dim white sits */
        for (u32 o = 0xF8; o < 0x180; o += 8) {
            auto prop = At<u8*>(node, o);
            if (!HeapPtr(prop)) continue;
            float* at = nullptr;
            if (!FindFloats(prop, 0x100, &at))
                for (u32 q = 0; q < 0x100 && !at; q += 8) {
                    auto mat = At<u8*>(prop, q);
                    if (HeapPtr(mat)) FindFloats(mat, 0x100, &at);
                }
            if (at) { at[0] = rgb[0]; at[1] = rgb[1]; at[2] = rgb[2]; done++; break; }
        }
    }
    auto kids = At<void* const*>(node, 0x118);
    u16 cap = At<u16>(node, 0x120);
    if (HeapPtr(kids) && cap <= 64)
        for (int i = 0; i < cap; i++) done += Paint(kids[i], tag, inLayer, rgb, depth + 1);
    return done;
}

}  // namespace

int TestPaint(u32 layer, int colour) {
    void* player = game::Player();
    if (!player || colour < 0 || colour > 6) return -1;
    void* root = reinterpret_cast<void* (*)(void*)>((*reinterpret_cast<uintptr_t**>(player))[0x378 / 8])(player);
    char tag[16];
    snprintf(tag, sizeof tag, "%08X", layer);
    int n = Paint(root, tag, false, RGB[colour], 0);
    Log("glowtest %08x цвет %d: покрашено %d", layer, colour, n);
    return n;
}

void Pump() {
    void* player = game::Player();
    if (!player || !game::Settled()) return;
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    if (now < g_next) return;
    g_next = now + 1000;
    if (!g_ready) {
        Setup();
        /* a save keeps the layers worn: take them all off, they are put on again below while wanted */
        static trade::Count inv[1024];
        int m = trade::Changes(player, inv, 1024);
        for (int j = 0; j < m; j++)
            for (int i = 0; i < NL; i++)
                if (inv[j].id == g_layer[i] && inv[j].n > 0)
                    if (void* f = equip::Lookup(g_layer[i])) trade::AddItems(player, f, -inv[j].n);
    }
    if (g_patch < 0 || !NL) return;
    u32 want[8]; int col[8];
    int n = Wanted(player, want, col, 8);
    void* vm = game::FakeVm();
    /* off what is no longer wanted */
    for (int i = 0; i < g_wornN; ) {
        bool keep = false;
        for (int k = 0; k < n; k++) if (want[k] == g_worn[i]) keep = true;
        if (keep) { i++; continue; }
        if (void* f = equip::Lookup(g_worn[i])) {
            reinterpret_cast<void (*)(void*, u32, void*, void*, bool, bool)>(game::ModuleAddr(UNEQUIP_ITEM))(vm, 0, player, f, false, true);
            trade::AddItems(player, f, -1);
        }
        Log("свечение: слой %08x снят", g_worn[i]);
        g_worn[i] = g_worn[--g_wornN];
        g_wornColour[i] = g_wornColour[g_wornN];
    }
    /* on what is wanted */
    for (int k = 0; k < n; k++) {
        int have = -1;
        for (int i = 0; i < g_wornN; i++) if (g_worn[i] == want[k]) have = i;
        if (have < 0 && g_wornN < 8) {
            if (void* f = equip::Lookup(want[k])) {
                trade::AddItems(player, f, 1);
                reinterpret_cast<void (*)(void*, u32, void*, void*, bool, bool)>(game::ModuleAddr(EQUIP_ITEM))(vm, 0, player, f, true, true);
                g_worn[g_wornN] = want[k];
                g_wornColour[g_wornN++] = -1;                  // painted once its 3D is there
                Log("свечение: слой %08x надет (цвет %d)", want[k], col[k]);
            }
        }
    }
    /* paint (every second: the 3D is rebuilt on equips, camera switches, cell changes) */
    void* root = reinterpret_cast<void* (*)(void*)>((*reinterpret_cast<uintptr_t**>(player))[0x378 / 8])(player);
    for (int i = 0; i < g_wornN; i++) {
        int c = 6;
        for (int k = 0; k < n; k++) if (want[k] == g_worn[i]) c = col[k];
        char tag[16];
        snprintf(tag, sizeof tag, "%08X", g_worn[i]);
        int painted = Paint(root, tag, false, RGB[c], 0);
        if (painted && g_wornColour[i] != c) { g_wornColour[i] = c; Debug("свечение: слой %08x покрашен (%d форм)", g_worn[i], painted); }
    }
}

}  // namespace coop::glow
