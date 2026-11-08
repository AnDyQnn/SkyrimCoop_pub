#include "titlemenu.hpp"
#include "client.hpp"
#include "game.hpp"
#include "settings.hpp"
#include "lib.hpp"
#include "updates.hpp"
#include <nn/os.hpp>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#pragma GCC diagnostic ignored "-Wformat-truncation"

/* The network screen of the title menu (our startmenu.gfx draws it, see notes/StartMenu_online.as).
   ActionScript calls GameDelegate "HELP" with ["poll"] or ["do", action]; the game's empty HELP handler slot
   holds OnHelp. The answer is a text the panel shows, put into _root.coopState with GFxMovie::SetVariable
   (vtable slot 0x11 on the Switch build: CommonLib's 0x10 + the second Itanium destructor).
   Text: first line = title, then info lines; lines "@action|label" are the buttons.
   Anything slow (room search, keyboard, downloads) runs on our worker thread. */
namespace coop::titlemenu {
namespace {

struct GFxValue { void* obj; u32 type; u32 pad; const void* value; };      // Scaleform 3.x
constexpr u32 GFX_STRING = 4, GFX_MANAGED = 0x40;
constexpr u32 VT_SET_VARIABLE = 0x11;

enum View { ROOT, ROOMS, SAVES, UPDATES };
volatile int g_view = ROOT;
char g_note[400];                         // one-line message under the title ("ищу…", "скачано")

Room g_rooms[8];
volatile int g_roomN = -1;                // -1 = never searched
char g_saveFiles[6][96], g_saveLabels[6][40];
volatile int g_saveN = -1;

/* worker */
enum Job { NONE, SEARCH, ASK_IP, ASK_NAME, LIST_SAVES, GET_SAVE, UPD_CHECK, UPD_INSTALL };
volatile int g_updState;                  // 0 idle, 1 working, 2 checked (updates found), 3 done / nothing to do
volatile int g_job = NONE;
volatile int g_jobArg;
nn::os::ThreadType g_thread;
alignas(0x1000) u8 g_stack[0x8000];
bool g_started;

void Note(const char* t) {
    snprintf(g_note, sizeof g_note, "%s", t);
    /* (07.10, his console: "мои персонажи на сервере все в кракозябрах") a note cut inside a Cyrillic letter made the whole
       screen's text invalid UTF-8: never end on a broken sequence */
    size_t n = strlen(g_note);
    while (n && (static_cast<u8>(g_note[n - 1]) & 0xC0) == 0x80) n--;
    if (n && (static_cast<u8>(g_note[n - 1]) & 0xC0) == 0xC0) n--;
    if (n < strlen(g_note) && strlen(t) >= sizeof g_note - 1) g_note[n] = 0;
}

void Worker(void*) {
    for (;;) {
        int job = __atomic_exchange_n(&g_job, (int)NONE, __ATOMIC_ACQ_REL);
        char buf[128], host[64];
        u16 port;
        switch (job) {
            case SEARCH: {
                Note("Ищу серверы в сети…");
                Room r[8];
                int n = FindRooms(r, 8, 3);
                memcpy(g_rooms, r, sizeof r);
                g_roomN = n;
                Note(n ? "" : "Серверов не найдено. Сервер запущен? Консоль и ПК в одной Wi-Fi сети?");
                break;
            }
            case ASK_IP:
                if (!settings::ManualAddress(host, sizeof host, &port)) host[0] = 0;
                if (settings::AskText(u"Подключиться по IP", u"Адрес сервера, например 192.168.1.14 (порт по умолчанию 27015)",
                                      host, buf, sizeof buf)) {
                    settings::SetAddress(buf);
                    SetOnline(true);
                    Note(buf[0] ? "Подключаюсь…" : "Адрес очищен — ищу сервер в сети");
                }
                break;
            case ASK_NAME: {
                char name[32];
                settings::GetName(name, sizeof name);
                if (settings::AskText(u"Имя игрока", u"Так тебя увидят другие игроки", name, buf, sizeof buf)) settings::SetName(buf);
                break;
            }
            case LIST_SAVES: {
                char list[1024];
                g_saveN = -1;
                if (!CloudList(list, sizeof list, 5000)) { Note("Нет связи с сервером"); g_saveN = 0; break; }
                int n = 0;
                for (char* line = list; line && *line && n < 6;) {
                    char* next = strchr(line, '\n');
                    if (next) *next++ = 0;
                    char* t1 = strchr(line, '\t');
                    char* t2 = t1 ? strchr(t1 + 1, '\t') : nullptr;
                    if (t1 && t2) {
                        *t1 = 0;
                        snprintf(g_saveFiles[n], sizeof g_saveFiles[n], "%s", line);
                        snprintf(g_saveLabels[n], sizeof g_saveLabels[n], "%s", t2 + 1);
                        n++;
                    }
                    line = next;
                }
                g_saveN = n;
                Note(n ? "" : "На сервере пока нет твоих сохранений — они появятся после первого сохранения в сетевой игре");
                break;
            }
            case GET_SAVE:
                Note("Скачиваю сохранение…");
                Note(CloudDownload(g_saveFiles[g_jobArg], 120000)
                         ? "Сохранение скачано — выбери «Загрузить игру»"
                         : "Не удалось скачать сохранение");
                break;
            case UPD_CHECK:
            case UPD_INSTALL:
                g_updState = 1;
                if (updates::Check(job == UPD_INSTALL)) g_updState = 3;
                else g_updState = job == UPD_CHECK && Status() == NetStatus::Connected ? 2 : 3;
                break;
            default:
                nn::os::SleepThread(nn::TimeSpan::FromMilliSeconds(50));
        }
    }
}

void Start(int job, int arg = 0) {
    if (!g_started) {
        g_started = true;
        nn::os::CreateThread(&g_thread, Worker, nullptr, g_stack, sizeof g_stack, 16, -2);
        nn::os::StartThread(&g_thread);
    }
    g_jobArg = arg;
    g_job = job;
}

const char* StatusText() {
    switch (Status()) {
        case NetStatus::Connected: return "подключено";
        case NetStatus::Personal: return "подключено, но загружен личный сейв — он с сервером не синхронизируется";
        case NetStatus::Searching: return "ищу сервер в сети…";
        case NetStatus::Connecting: return "подключаюсь…";
        case NetStatus::NoNetwork: return "нет сети";
        case NetStatus::Failed: return "сервер недоступен, пробую снова";
        default: return IsOnline() ? "…" : "выключено (игра одному)";
    }
}

char g_text[3000];

void Build() {
    int len = 0;
    auto add = [&](const char* fmt, auto... a) {
        if (len < (int)sizeof g_text - 1) len += snprintf(g_text + len, sizeof g_text - len, fmt, a...);
    };
    char name[32], host[64];
    u16 port = 0;
    settings::GetName(name, sizeof name);
    bool manual = settings::ManualAddress(host, sizeof host, &port);
    if (g_view == ROOMS) {
        add("ПОИСК СЕРВЕРА\n");
        if (g_note[0]) add("%s\n", g_note);
        if (g_roomN > 0) {
            add("Найдено: %d. Выбери сервер:\n", (int)g_roomN);
            for (int i = 0; i < g_roomN; i++) {
                if (g_rooms[i].name[0]) add("@room:%d|%s   %s:%u\n", i, g_rooms[i].name, g_rooms[i].host, g_rooms[i].port);
                else add("@room:%d|%s:%u\n", i, g_rooms[i].host, g_rooms[i].port);
            }
        }
        add("@search|Искать снова\n@back|Назад\n");
        return;
    }
    if (g_view == SAVES) {
        add("МОИ ПЕРСОНАЖИ НА СЕРВЕРЕ\n");
        if (g_note[0]) add("%s\n", g_note);
        add("Каждое сохранение в сетевой игре уходит на сервер само. Выбери — оно скачается на консоль.\n");
        if (g_saveN < 0) add("Загружаю список…\n");
        for (int i = 0; i < g_saveN; i++) add("@save:%d|Сохранение от %s\n", i, g_saveLabels[i]);
        add("@back|Назад\n");
        return;
    }
    if (g_view == UPDATES) {
        add("ОБНОВЛЕНИЯ С СЕРВЕРА\n");
        if (Status() != NetStatus::Connected) add("Сначала подключись к серверу (сейчас: %s).\n", StatusText());
        add("%s\n", g_updState == 1 ? "Проверяю…" : updates::Status());
        add("Файлы из папки updates сервера сравниваются с картой памяти, отличающиеся скачиваются.\n");
        if (g_updState == 2) add("@updinstall|Установить обновления\n");
        if (g_updState != 1) add("@updcheck|Проверить ещё раз\n");
        add("@back|Назад\n");
        return;
    }
    add("СЕТЕВАЯ ИГРА\n");
    if (g_note[0]) add("%s\n", g_note);
    add("Сервер: %s", StatusText());
    if (Status() == NetStatus::Connected) add(" — %s", ServerName());
    add("\nИгрок: %s\nАдрес: %s\n", name, manual ? host : "поиск в сети");
    RemotePlayer rs[MAX_REMOTE];
    int n = SnapshotRemotes(rs, MAX_REMOTE);
    if (Status() == NetStatus::Connected) {
        add("Уже в игре: ");
        if (!n) add("никого");
        for (int i = 0; i < n; i++) {
            char pn[32];
            if (!PlayerName(rs[i].state.id, pn, sizeof pn)) snprintf(pn, sizeof pn, "Игрок %u", rs[i].state.id);
            add("%s%s", i ? ", " : "", pn);
        }
        add("\nЗагрузи или начни игру — в мире ты будешь вместе с остальными.\n");
    }
    add("@rooms|Найти сервер\n@ip|Подключиться по IP\n@name|Имя игрока\n@saves|Мои персонажи на сервере\n@updates|Проверить обновления\n");
    add("@online|%s\n@back|Назад\n", IsOnline() ? "Играть одному (отключиться)" : "Играть по сети (подключиться)");
}

void Act(const char* a) {
    g_note[0] = 0;
    if (!strcmp(a, "rooms")) { g_view = ROOMS; if (g_roomN < 0) Start(SEARCH); }
    else if (!strcmp(a, "search")) Start(SEARCH);
    else if (!strncmp(a, "room:", 5)) {
        int i = atoi(a + 5);
        if (i >= 0 && i < g_roomN) {
            char addr[64];
            snprintf(addr, sizeof addr, "%s:%u", g_rooms[i].host, g_rooms[i].port);
            settings::SetAddress(addr);
            SetOnline(true);
            g_view = ROOT;
            Note("Подключаюсь к выбранному серверу…");
        }
    }
    else if (!strcmp(a, "ip")) Start(ASK_IP);
    else if (!strcmp(a, "name")) Start(ASK_NAME);
    else if (!strcmp(a, "saves")) { g_view = SAVES; g_saveN = -1; Start(LIST_SAVES); }
    else if (!strncmp(a, "save:", 5)) { int i = atoi(a + 5); if (i >= 0 && i < g_saveN) Start(GET_SAVE, i); }
    else if (!strcmp(a, "updates") || !strcmp(a, "updcheck")) { g_view = UPDATES; if (g_updState != 1) { g_updState = 1; Start(UPD_CHECK); } }
    else if (!strcmp(a, "updinstall")) { if (g_updState == 2) { g_updState = 1; Start(UPD_INSTALL); } }
    else if (!strcmp(a, "online")) SetOnline(!IsOnline());
    else if (!strcmp(a, "back")) g_view = ROOT;
}

const char* ArgString(const GFxValue* v) {
    if ((v->type & 0x0F) != GFX_STRING) return nullptr;
    return (v->type & GFX_MANAGED) ? *static_cast<const char* const*>(v->value) : static_cast<const char*>(v->value);
}

}  // namespace

/* FxDelegate callback: const FxDelegateArgs& (movie +0x20, args +0x28, count +0x30). */
void OnHelp(void* argsObj) {
    auto base = static_cast<u8*>(argsObj);
    void* movie = *reinterpret_cast<void**>(base + 0x20);
    auto args = *reinterpret_cast<const GFxValue**>(base + 0x28);
    u32 n = *reinterpret_cast<u32*>(base + 0x30);
    const char* cmd = n > 0 ? ArgString(&args[0]) : nullptr;
    if (!cmd || !movie) return;
    if (!strcmp(cmd, "do") && n > 1) {
        if (const char* a = ArgString(&args[1])) Act(a);
    } else if (!strcmp(cmd, "open")) {
        g_view = ROOT;
        g_note[0] = 0;
    }
    Build();
    GFxValue v{nullptr, GFX_STRING, 0, g_text};
    auto vt = *reinterpret_cast<uintptr_t**>(movie);
    reinterpret_cast<bool (*)(void*, const char*, const GFxValue*, u32)>(vt[VT_SET_VARIABLE])(movie, "_root.coopState", &v, 1);
}

}  // namespace coop::titlemenu
