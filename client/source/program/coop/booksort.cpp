#include "booksort.hpp"
#include "client.hpp"
#include "equip.hpp"
#include "game.hpp"
#include <cstdio>
#include <cstring>

namespace coop::booksort {
namespace {

struct BookRow { u8 file; u32 id; u8 mask; };
#include "booksort_data.inc"

enum : u8 { NOTE = 1, JOURNAL = 2, SKILL = 4, TOME = 8, RECIPE = 16 };
/* the keywords, as KID's ini names them: LotD's note / journal / skill book sorting, vanilla VendorItemSpellTome and
   VendorItemRecipe */
struct Kw { u8 bit; const char* file; u32 id; };
const Kw KWS[] = {{NOTE, "LegacyoftheDragonborn.esp", 0x166440}, {JOURNAL, "LegacyoftheDragonborn.esp", 0x166441},
                  {SKILL, "LegacyoftheDragonborn.esp", 0x3B0564}, {TOME, "Skyrim.esm", 0x0937A5},
                  {RECIPE, "Skyrim.esm", 0x0F5CB0}};
constexpr u32 FORM_ID = 0x14;

template <class T> T& At(void* p, u32 off) { return *reinterpret_cast<T*>(static_cast<u8*>(p) + off); }

/* BGSKeywordForm inside TESObjectBOOK: {vtable, BGSKeyword** keywords, u32 count}; SE has it at +0xF8. Found on a
   vanilla spell tome by its VendorItemSpellTome keyword instead of trusting the PC offset. Returns the offset of
   the keywords pointer. */
u32 FindKeywords(void* tome, u32 tomeKw) {
    for (u32 o = 0x30; o < 0x200; o += 8) {
        void** arr = At<void**>(tome, o);
        u32 n = At<u32>(tome, o + 8);
        if (!n || n > 64 || (reinterpret_cast<uintptr_t>(arr) & 7) || !game::Readable(arr, n * 8)) continue;
        for (u32 i = 0; i < n; i++)
            if (arr[i] && game::Readable(arr[i], 0x20) && At<u32>(arr[i], FORM_ID) == tomeKw) return o;
    }
    return 0;
}

/* when the game's allocator says no: the arrays from the module's own memory (books are never unloaded) */
alignas(8) u8 g_pool[96 * 1024];
u32 g_poolUsed;
void* Alloc(u32 size, int* fromPool) {
    if (void* p = game::GameAlloc(size)) return p;
    if (g_poolUsed + size > sizeof g_pool) return nullptr;
    (*fromPool)++;
    void* p = g_pool + g_poolUsed;
    g_poolUsed += (size + 7) & ~7u;
    return p;
}

bool Has(void** arr, u32 n, void* kw) {
    for (u32 i = 0; i < n; i++)
        if (arr[i] == kw) return true;
    return false;
}

}  // namespace

void Pump() {
    static bool done;
    static int waits;
    if (done) return;
    if (game::PluginIndex("Skyrim.esm") != 0) return;         // plugins not loaded yet
    if (game::PluginIndex("LegacyoftheDragonborn.esp") < 0) { done = true; Log("LotD: не загружен — сортировка книг не нужна"); return; }
    if (!equip::Lookup(0x00000007)) {                         // the player's base: lookup not resolved yet
        if (++waits == 2000) { done = true; Log("LotD: нет поиска форм — сортировка книг пропущена"); }
        return;
    }
    done = true;

    void* kw[sizeof KWS / sizeof KWS[0]] = {};
    for (u32 k = 0; k < sizeof KWS / sizeof KWS[0]; k++) {
        int fi = game::PluginIndex(KWS[k].file);
        kw[k] = fi < 0 ? nullptr : equip::Lookup((u32(fi) << 24) | KWS[k].id);
        if (!kw[k]) { Log("LotD: нет ключевого слова %s %06X", KWS[k].file, KWS[k].id); return; }
    }
    /* any vanilla tome of the table finds the offset */
    u32 off = 0;
    u8 bookType = 0;
    int tries = 0;
    for (const auto& r : BOOKS) {
        if (!(r.mask & TOME) || strcmp(BOOK_FILES[r.file], "Skyrim.esm")) continue;
        void* b = equip::Lookup(r.id);
        if (b && (off = FindKeywords(b, 0x0937A5))) { bookType = At<u8>(b, 0x1A); break; }
        if (++tries > 20) break;
    }
    if (!off) { Log("LotD: не нашёл ключевые слова в книге — сортировка книг пропущена"); return; }

    int added = 0, books = 0, missing = 0, had = 0, pool = 0, failed = 0;
    for (const auto& r : BOOKS) {
        int fi = game::PluginIndex(BOOK_FILES[r.file]);
        void* b = fi < 0 ? nullptr : equip::Lookup((u32(fi) << 24) | r.id);
        if (!b || At<u8>(b, 0x1A) != bookType) { missing++; continue; }
        void** arr = At<void**>(b, off);
        u32 n = At<u32>(b, off + 8);
        void* want[8];
        u32 add = 0;
        for (u32 k = 0; k < sizeof KWS / sizeof KWS[0]; k++)
            if ((r.mask & KWS[k].bit) && !Has(arr, n, kw[k])) want[add++] = kw[k];
        if (!add) { had++; continue; }
        auto fresh = static_cast<void**>(Alloc((n + add) * sizeof(void*), &pool));
        if (!fresh) { failed++; continue; }
        if (n) memcpy(fresh, arr, n * sizeof(void*));
        memcpy(fresh + n, want, add * sizeof(void*));
        At<void**>(b, off) = fresh;                              // the old array stays (a few bytes, once)
        At<u32>(b, off + 8) = n + add;
        added += add;
        books++;
    }
    Log("LotD: сортировка книг — слов добавлено %d в %d книг, уже были у %d (смещение +%x, не найдено %d, из запаса %d, "
        "без памяти %d)", added, books, had, off, missing, pool, failed);
}

}  // namespace coop::booksort
