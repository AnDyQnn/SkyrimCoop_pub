#pragma GCC diagnostic ignored "-Wformat-truncation"   // snprintf truncation is intended (fixed-size UI labels)
#include "menu.hpp"
#include "death.hpp"
#include "client.hpp"
#include "console.hpp"
#include "game.hpp"
#include "settings.hpp"
#include "avatar.hpp"
#include "chest.hpp"
#include "trade.hpp"
#include "followers.hpp"
#include "titlemenu.hpp"
#include <nn/os.hpp>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace coop::menu {
namespace {

/* CreateMessage(text, callback, 0, 4, 10, button0, button1, ..., nullptr) — the arguments
   Debug.MessageBox passes; the button list is variadic and ends with a null pointer. */
using CreateMessageFn = void (*)(const char* text, void* callback, u32, u32, u32, const char* button, ...);
uintptr_t g_createMessage;

/* The callback argument is a plain function: CreateMessage wraps it in its own
   IMessageBoxCallback (vtable 0x1a54360) whose Run calls fn(index of the pressed button). */
void OnButton(u32 button);

constexpr int MAX_BUTTONS = 9;
char g_text[1024];
char g_buttons[MAX_BUTTONS][80];
int g_buttonCount;
volatile bool g_requestShow;     // menu thread -> PumpUI
volatile int g_choice = -1;      // CbRun -> menu thread

volatile bool g_open;
alignas(0x1000) u8 g_stack[0x8000];
nn::os::ThreadType g_thread;
bool g_threadMade;

/* a question that does not need the menu open (03.10: "go along?"): its answer runs a function on the game thread */
volatile bool g_offering;
void (*g_offerYes)();

void OnButton(u32 button) {
    if (__atomic_exchange_n(&g_offering, false, __ATOMIC_ACQ_REL)) {
        if (button == 0 && g_offerYes) g_offerYes();
        return;
    }
    g_choice = (int)button;
}

/* Shows a box and waits for a button; returns its index. */
int Ask(const char* text, const char* const* buttons, int count) {
    snprintf(g_text, sizeof g_text, "%s", text);
    g_buttonCount = count < MAX_BUTTONS ? count : MAX_BUTTONS;
    for (int i = 0; i < g_buttonCount; i++) {
        strncpy(g_buttons[i], buttons[i], sizeof g_buttons[i] - 1);
        g_buttons[i][sizeof g_buttons[i] - 1] = 0;
    }
    g_choice = -1;
    __atomic_store_n(&g_requestShow, true, __ATOMIC_RELEASE);
    while (g_choice < 0) nn::os::SleepThread(nn::TimeSpan::FromMilliSeconds(50));
    return g_choice;
}

const char* StatusText(char* buf, u32 size) {
    switch (Status()) {
        case NetStatus::Connected:  snprintf(buf, size, "подключено — %s", ServerName()); break;
        case NetStatus::Searching:  snprintf(buf, size, "ищу комнату в сети…"); break;
        case NetStatus::Connecting: snprintf(buf, size, "подключаюсь…"); break;
        case NetStatus::NoNetwork:  snprintf(buf, size, "нет сети"); break;
        case NetStatus::Failed:     snprintf(buf, size, "сервер недоступен, пробую снова"); break;
        default:                    snprintf(buf, size, IsOnline() ? "запуск…" : "отключено"); break;
    }
    return buf;
}

void RoomsMenu() {
    Room rooms[6];
    int n = FindRooms(rooms, 6, 3);
    if (n == 0) {
        const char* b[] = {"Назад"};
        Ask("Комнаты в сети\n\nНичего не найдено. Сервер запущен? Консоль и ПК в одной Wi-Fi сети?", b, 1);
        return;
    }
    char text[512] = "Комнаты в сети — выбери, к какой подключиться:";
    char labels[7][80];
    const char* b[7];
    for (int i = 0; i < n; i++) {
        if (rooms[i].name[0]) snprintf(labels[i], sizeof labels[i], "%s (%s)", rooms[i].name, rooms[i].host);
        else snprintf(labels[i], sizeof labels[i], "%s:%u", rooms[i].host, rooms[i].port);
        b[i] = labels[i];
    }
    b[n] = "Назад";
    int c = Ask(text, b, n + 1);
    if (c < n) {
        char addr[64];
        snprintf(addr, sizeof addr, "%s:%u", rooms[c].host, rooms[c].port);
        settings::SetAddress(addr);
        SetOnline(true);
        game::Notify("Кооп: подключаюсь к выбранной комнате");
    }
}

bool PlayersMenu() {             // true = teleported: the whole menu closes
    char text[1024];
    int len = snprintf(text, sizeof text, "Игроки в комнате\n");
    RemotePlayer rs[MAX_REMOTE];
    int n = SnapshotRemotes(rs, MAX_REMOTE);
    char me[32];
    settings::GetName(me, sizeof me);
    len += snprintf(text + len, sizeof text - len, "\n%s (ты)", me);
    for (int i = 0; i < n && len < (int)sizeof text - 80; i++) {
        char name[32];
        if (!PlayerName(rs[i].state.id, name, sizeof name)) snprintf(name, sizeof name, "Игрок %u", rs[i].state.id);
        proto::StatsMsg st{};                                  /* level and health, as the nameplates show them */
        if (StatsOf(rs[i].state.id, &st) && st.level && st.level < 1000)
            len += snprintf(text + len, sizeof text - len, "\n%s [%u ур., здоровье %u%%] — %s", name, st.level, st.hp,
                            game::DistanceText(rs[i].state));
        else
            len += snprintf(text + len, sizeof text - len, "\n%s — %s", name, game::DistanceText(rs[i].state));
    }
    if (n == 0) snprintf(text + len, sizeof text - len, "\n\nБольше никого нет.");
    else snprintf(text + len, sizeof text - len, "\n\nВыбери игрока: переместиться к нему или обменяться вещами.");
    /* one button per player: teleport next to them (same world or interior) */
    static char labels[MAX_REMOTE][48];
    const char* b[MAX_REMOTE + 1];
    int k = 0;
    for (int i = 0; i < n && k < 6; i++, k++) {
        char name[32];
        if (!PlayerName(rs[i].state.id, name, sizeof name)) snprintf(name, sizeof name, "Игрок %u", rs[i].state.id);
        snprintf(labels[k], sizeof labels[k], "%s", name);
        b[k] = labels[k];
    }
    b[k] = "Назад";
    int c = Ask(text, b, k + 1);
    if (c >= 0 && c < k) {
        char pname[32], t[128];
        if (!PlayerName(rs[c].state.id, pname, sizeof pname)) snprintf(pname, sizeof pname, "Игрок %u", rs[c].state.id);
        snprintf(t, sizeof t, "%s\n\n%s", pname, game::DistanceText(rs[c].state));
        const char* bb[] = {"Переместиться к нему", "Обмен вещами", "Назад"};
        int d = Ask(t, bb, 3);
        if (d == 0) {
            char cmd[16];
            snprintf(cmd, sizeof cmd, "tp %u", rs[c].state.id);
            console::Execute(cmd);
            return true;
        }
        if (d == 1) {
            trade::Start(rs[c].state.id);
            return true;
        }
    }
    return false;
}

/* The campaign's house and chest (03.10: no guilds, everything is the campaign's), marriage (server/social.py). true = teleported (menu closes). */
bool ClanMenu() {
    for (;;) {
        char info[1600];
        if (!Social("info", info, sizeof info, 4000)) {
            const char* b[] = {"Назад"};
            Ask("Кампания\n\nНет связи с сервером.", b, 1);
            return false;
        }
        /* "#house cell world x y z" first line = the clan has a house */
        bool house = false;
        u32 hc = 0, hw = 0; float hx = 0, hy = 0, hz = 0;
        char* body = info;
        if (!strncmp(info, "#house ", 7)) {
            house = true;
            char* nl = strchr(info, '\n');
            if (nl) *nl = 0;
            char* p = info + 7;
            hc = strtoul(p, &p, 16); hw = strtoul(p, &p, 16);
            hx = strtof(p, &p); hy = strtof(p, &p); hz = strtof(p, &p);
            body = nl ? nl + 1 : info + strlen(info);
        }
        char text[1700];
        snprintf(text, sizeof text, "Кампания и семья\n\n%s", body);
        const char* b[8]; int k = 0; int act[8];
        if (house) { b[k] = "В дом кампании"; act[k++] = 1; }
        b[k] = "Общий сундук"; act[k++] = 9;
        b[k] = "Сделать здесь дом кампании"; act[k++] = 2;
        b[k] = "Предложить брак"; act[k++] = 6;
        if (strstr(body, "предлагает тебе брак")) { b[k] = "Принять предложение"; act[k++] = 7; }
        if (strstr(body, "Супруг")) { b[k] = "Развод"; act[k++] = 8; }
        b[k] = "Назад";
        int c = Ask(text, b, k + 1);
        if (c < 0 || c >= k) return false;
        char cmd[160], out[1600];
        switch (act[c]) {
            case 1: game::TeleportTo(hc, hw, hx, hy, hz); return true;
            case 2: {
                u32 cell, world; float pos[3];
                if (!game::WhereAmI(&cell, &world, pos)) break;
                snprintf(cmd, sizeof cmd, "clan house set %x %x %.0f %.0f %.0f", cell, world, pos[0], pos[1], pos[2]);
                Social(cmd, out, sizeof out, 4000);
                break;
            }
            case 6: {
                RemotePlayer rs[MAX_REMOTE];
                int m = SnapshotRemotes(rs, MAX_REMOTE);
                static char names[6][32];
                const char* bb[7]; int j = 0;
                for (int i = 0; i < m && j < 6; i++)
                    if (PlayerName(rs[i].state.id, names[j], sizeof names[j])) { bb[j] = names[j]; j++; }
                bb[j] = "Назад";
                int d = Ask("Кому сделать предложение?", bb, j + 1);
                if (d >= 0 && d < j) { snprintf(cmd, sizeof cmd, "marry %s", names[d]); Social(cmd, out, sizeof out, 4000); }
                break;
            }
            case 7: Social("accept", out, sizeof out, 4000); break;
            case 8: Social("divorce", out, sizeof out, 4000); break;
            case 9:
                if (Social("chest get", out, sizeof out, 4000) && !strncmp(out, "#chest ", 7)) {
                    chest::Open(out);                              // opens on the game thread
                    return true;                                   // the menu closes for the container
                }
                break;
        }
    }
}

/* Shared followers: pick one, then whom it follows (you, another player, or nobody). */
void FollowersMenu() {
    u32 ids[8]; char names[8][40]; u16 leaders[8];
    int n = followers::List(ids, names, leaders, 6);
    char text[900];
    int len = snprintf(text, sizeof text, "Спутники\n\nЛюбой игрок может сказать спутнику, за кем идти.\n");
    char who[32];
    for (int i = 0; i < n && len < (int)sizeof text - 80; i++) {
        if (!leaders[i]) snprintf(who, sizeof who, "ни за кем");
        else if (leaders[i] == MyId()) snprintf(who, sizeof who, "за тобой");
        else if (!PlayerName(leaders[i], who, sizeof who)) snprintf(who, sizeof who, "за игроком %u", leaders[i]);
        len += snprintf(text + len, sizeof text - len, "\n%s — %s", names[i], who);
    }
    if (!n) snprintf(text + len, sizeof text - len, "\nПока нет: возьми спутника в игре (например Лидию) — он появится здесь.");
    const char* b[7];
    for (int i = 0; i < n; i++) b[i] = names[i];
    b[n] = "Назад";
    int c = Ask(text, b, n + 1);
    if (c < 0 || c >= n) return;
    RemotePlayer rs[MAX_REMOTE];
    int m = SnapshotRemotes(rs, MAX_REMOTE);
    static char labels[8][40];
    const char* bb[9];
    u16 target[9];
    int k = 0;
    bb[k] = "За мной"; target[k++] = MyId();
    for (int i = 0; i < m && k < 6; i++) {
        char pn[32];
        if (!PlayerName(rs[i].state.id, pn, sizeof pn)) snprintf(pn, sizeof pn, "Игрок %u", rs[i].state.id);
        snprintf(labels[k], sizeof labels[k], "За: %s", pn);
        bb[k] = labels[k]; target[k++] = rs[i].state.id;
    }
    bb[k] = "Отпустить"; target[k++] = 0;
    bb[k] = "Назад";
    char t[96];
    snprintf(t, sizeof t, "%s\n\nЗа кем идти?", names[c]);
    int d = Ask(t, bb, k + 1);
    if (d < 0 || d >= k) return;
    followers::Command(ids[c], target[d]);
    game::Notify("Кооп: приказ спутнику отдан");
}

void SavesMenu() {
    char list[1024], text[1200];
    if (!CloudList(list, sizeof list, 4000)) {
        const char* b[] = {"Назад"};
        Ask("Онлайн-сохранения\n\nНет связи с сервером.", b, 1);
        return;
    }
    /* "file\tsize\tdate" lines -> buttons with the date, newest first */
    static char files[6][96], labels[6][32];
    int n = 0;
    for (char* line = list; line && *line && n < 6;) {
        char* next = strchr(line, '\n');
        if (next) *next++ = 0;
        char* t1 = strchr(line, '\t');
        char* t2 = t1 ? strchr(t1 + 1, '\t') : nullptr;
        if (t1 && t2) {
            *t1 = 0;
            snprintf(files[n], sizeof files[n], "%s", line);
            snprintf(labels[n], sizeof labels[n], "%s", t2 + 1);
            n++;
        }
        line = next;
    }
    snprintf(text, sizeof text, "Онлайн-сохранения\n\nТвой персонаж хранится на сервере.\nКаждое новое сохранение уходит туда само.\n"
             "Сейчас: %s\n%s\n\n%s", CloudStatus(),
             avatar::Status()[0] ? avatar::Status() : "Персонаж (аватар): «Сохранить персонажа» — и он переносится в любую игру.",
             n ? "Выбери сохранение — оно скачается на консоль.\nПотом загрузи его в меню игры «Загрузить»."
               : "На сервере пока нет твоих сохранений.");
    const char* b[9];
    for (int i = 0; i < n; i++) b[i] = labels[i];
    b[n] = "Сохранить персонажа";
    b[n + 1] = "Перенести персонажа сюда";
    b[n + 2] = "Назад";
    int c = Ask(text, b, n + 3);
    if (c == n) { avatar::RequestSave(); return; }
    if (c == n + 1) {
        const char* yn[] = {"Перенести", "Отмена"};
        if (Ask("Перенести моего персонажа сюда\n\nВнешность, уровень, навыки, перки, заклинания, крики и вещи\n"
                "персонажа, сохранённого на сервере, появятся у текущего героя.\nНичего не отнимается.", yn, 2) == 0)
            avatar::RequestLoad();
        return;
    }
    if (c < 0 || c >= n) return;
    game::Notify("Кооп: скачиваю сохранение…");
    bool ok = CloudDownload(files[c], 120000);
    game::Notify(ok ? "Кооп: сохранение скачано — загрузи его в меню «Загрузить»" : "Кооп: не удалось скачать сохранение");
}

void SettingsMenu() {
    for (;;) {
        char name[32], host[64], text[400], buf[128];
        u16 port = 0;
        settings::GetName(name, sizeof name);
        bool manual = settings::ManualAddress(host, sizeof host, &port);
        snprintf(text, sizeof text, "Настройки кооператива\n\nИмя: %s\nАдрес: %s", name, manual ? host : "поиск в сети");
        const char* b[] = {"Комнаты в сети", "Ввести адрес", "Имя игрока", "Назад"};
        switch (Ask(text, b, 4)) {
            case 0: RoomsMenu(); break;
            case 1:
                if (settings::AskText(u"Адрес сервера", u"Пусто = искать комнату в сети. Пример: 192.168.1.14",
                                      manual ? host : "", buf, sizeof buf)) {
                    settings::SetAddress(buf);
                    SetOnline(true);
                }
                break;
            case 2:
                if (settings::AskText(u"Имя игрока", u"Так тебя увидят другие игроки", name, buf, sizeof buf))
                    settings::SetName(buf);
                break;
            default: return;
        }
    }
}

void MainMenu() {
    for (;;) {
        char status[96], name[32], text[512];
        settings::GetName(name, sizeof name);
        RemotePlayer rs[MAX_REMOTE];
        int n = SnapshotRemotes(rs, MAX_REMOTE);
        snprintf(text, sizeof text, "Кооператив\n\nСтатус: %s\nИмя: %s\nИгроков рядом: %d",
                 StatusText(status, sizeof status), name, n);
        if (death::IsDown()) {                       // (07.10) knocked down: the menu is the way to give up
            const char* d[] = {"Сдаться (очнуться у входа / в городе)", "Ждать помощи"};
            if (Ask("Ты без сознания\n\nТоварищ может поднять тебя: подойти и нажать A.", d, 2) == 0) death::GiveUp();
            return;
        }
        const char* b[] = {
            "Сообщение", "Игроки", "Кампания", "Спутники", "Сохранения", "Консоль", "Настройки",
            IsOnline() ? "Отключиться" : "Подключиться", "Закрыть",
        };
        int c = Ask(text, b, 9);
        char buf[128];
        switch (c) {
            case 0:
                if (Status() != NetStatus::Connected) break;
                if (settings::AskText(u"Сообщение", u"Увидят все игроки в комнате", "", buf, sizeof buf)) SendChat(buf);
                break;
            case 1: if (PlayersMenu()) return; break;
            case 2: if (ClanMenu()) return; break;
            case 3: FollowersMenu(); break;
            case 4: SavesMenu(); break;
            case 5: console::Prompt(); break;
            case 6: SettingsMenu(); break;
            case 7: SetOnline(!IsOnline()); break;
            default: return;
        }
    }
}

volatile bool g_titleMode;

/* Network screen of the title menu (Palworld / Divinity style: find a room, connect by address, who you are,
   your characters on the server). */
void OnlineMenu() {
    for (;;) {
        char status[96], name[32], host[64], text[600], buf[128];
        u16 port = 0;
        settings::GetName(name, sizeof name);
        bool manual = settings::ManualAddress(host, sizeof host, &port);
        snprintf(text, sizeof text,
                 "СЕТЕВАЯ ИГРА\n\nСервер: %s\nИгрок: %s\nАдрес: %s\n\n"
                 "Загрузи или начни игру — в мире ты будешь вместе с остальными.",
                 StatusText(status, sizeof status), name, manual ? host : "поиск в сети");
        const char* b[] = {"Найти сервер", "Подключиться по IP", "Имя игрока", "Мои персонажи",
                           IsOnline() ? "Играть одному" : "Играть по сети", "Назад"};
        switch (Ask(text, b, 6)) {
            case 0: RoomsMenu(); break;
            case 1:
                if (settings::AskText(u"Подключиться по IP", u"Адрес сервера, например 192.168.1.14 (порт по умолчанию 27015)",
                                      manual ? host : "", buf, sizeof buf)) {
                    settings::SetAddress(buf);
                    SetOnline(true);
                }
                break;
            case 2:
                if (settings::AskText(u"Имя игрока", u"Так тебя увидят другие игроки", name, buf, sizeof buf))
                    settings::SetName(buf);
                break;
            case 3: SavesMenu(); break;
            case 4: SetOnline(!IsOnline()); break;
            default: return;
        }
    }
}

void MenuThread(void*) {
    if (g_titleMode) OnlineMenu();
    else MainMenu();
    g_open = false;
}

}  // namespace

void SetCreateMessage(uintptr_t fn) { g_createMessage = fn; }
bool Available() { return g_createMessage != 0; }

void Open() {
    g_titleMode = false;
    if (!g_createMessage) { Log("меню кооператива: нет окна сообщений"); return; }
    if (__atomic_exchange_n(&g_open, true, __ATOMIC_ACQ_REL)) { Log("меню кооператива: уже открыто (ждёт ответа)"); return; }
    if (g_threadMade) { nn::os::WaitThread(&g_thread); nn::os::DestroyThread(&g_thread); }
    nn::os::CreateThread(&g_thread, MenuThread, nullptr, g_stack, sizeof g_stack, 16, -2);
    nn::os::StartThread(&g_thread);
    g_threadMade = true;
}

void PumpUI() {
    if (!__atomic_exchange_n(&g_requestShow, false, __ATOMIC_ACQ_REL)) return;
    const char* b[MAX_BUTTONS + 1] = {};
    for (int i = 0; i < g_buttonCount; i++) b[i] = g_buttons[i];
    auto create = reinterpret_cast<CreateMessageFn>(g_createMessage);
    create(g_text, reinterpret_cast<void*>(&OnButton), 0, 4, 10, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], nullptr);
}

bool Offer(const char* text, const char* yes, const char* no, void (*onYes)()) {
    if (!g_createMessage || g_open || g_offering) return false;
    snprintf(g_text, sizeof g_text, "%s", text);
    snprintf(g_buttons[0], sizeof g_buttons[0], "%s", yes);
    snprintf(g_buttons[1], sizeof g_buttons[1], "%s", no);
    g_buttonCount = 2;
    g_offerYes = onYes;
    __atomic_store_n(&g_offering, true, __ATOMIC_RELEASE);
    __atomic_store_n(&g_requestShow, true, __ATOMIC_RELEASE);
    return true;
}

}  // namespace coop::menu

namespace coop::menu {

void OpenOnline() {
    if (!g_createMessage || __atomic_exchange_n(&g_open, true, __ATOMIC_ACQ_REL)) return;
    g_titleMode = true;
    if (g_threadMade) { nn::os::WaitThread(&g_thread); nn::os::DestroyThread(&g_thread); }
    nn::os::CreateThread(&g_thread, MenuThread, nullptr, g_stack, sizeof g_stack, 16, -2);
    nn::os::StartThread(&g_thread);
    g_threadMade = true;
}

namespace {
void OnHelp(void* args) { titlemenu::OnHelp(args); }   // the network screen of the title menu (titlemenu.cpp)
}

/* The title menu registers its callbacks (name -> function) from GOT slots when it opens; HELP's slot
   (1.1.177: 0x2158d10, the empty 0x92048c) gets our function, the button of our startmenu.gfx calls it. */
void InstallTitleEntry() {
    constexpr u32 HELP_SLOT = 0x2158d10, HELP_FN = 0x92048c;
    auto slot = reinterpret_cast<uintptr_t*>(game::ModuleAddr(HELP_SLOT));
    if (*slot != game::ModuleAddr(HELP_FN)) { Log("title entry: HELP slot not as expected"); return; }
    *slot = reinterpret_cast<uintptr_t>(&OnHelp);
}

}  // namespace coop::menu
