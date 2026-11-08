#include "updates.hpp"
#include "client.hpp"
#include "console.hpp"
#include "game.hpp"
#include "lib.hpp"
#include <nn/fs.hpp>
#include <nn/os.hpp>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#pragma GCC diagnostic ignored "-Wformat-truncation"

namespace nn::fs {
    Result MountSdCard(const char* mount);
    Result CreateDirectory(const char* path);
    Result DeleteFile(const char* path);
    Result RenameFile(const char* from, const char* to);
    Result GetEntryType(DirectoryEntryType* type, const char* path);
}

/* Updates from the server: the server lists the files of its "updates" folder (paths from the SD card root,
   size, CRC32); the console compares them with its SD card ("sd:" mount — main.npdm grants the FS rights)
   and downloads the ones that differ (written as .new, then swapped in). The game has to be restarted
   for a new module / new files to be used. */
namespace coop::updates {
namespace {

bool g_mounted;
char g_status[200] = "не проверялось";

u32 Crc32(const u8* p, u64 n, u32 c = 0) {
    c = ~c;
    for (u64 i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

alignas(0x1000) u8 g_buf[0x10000];

bool FileCrc(const char* path, u64* size, u32* crc) {
    nn::fs::FileHandle h;
    if (R_FAILED(nn::fs::OpenFile(&h, path, nn::fs::OpenMode_Read))) return false;
    long sz = 0;
    nn::fs::GetFileSize(&sz, h);
    u32 c = 0;
    for (long off = 0; off < sz;) {
        u64 n = (u64)(sz - off) < sizeof g_buf ? (u64)(sz - off) : sizeof g_buf;
        if (R_FAILED(nn::fs::ReadFile(h, off, g_buf, n))) { nn::fs::CloseFile(h); return false; }
        c = Crc32(g_buf, n, c);
        off += n;
    }
    nn::fs::CloseFile(h);
    *size = sz;
    *crc = c;
    return true;
}

void MakeDirs(const char* path) {                  // "sd:/a/b/c.bin" -> sd:/a, sd:/a/b
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char* p = strchr(tmp + 4, '/'); p; p = strchr(p + 1, '/')) {
        *p = 0;
        nn::fs::CreateDirectory(tmp);
        *p = '/';
    }
}

}  // namespace

const char* Status() { return g_status; }

/* test machines only (Eden): the file sd:/skycoop_test/autoconnect makes the module connect at boot, so the test tools
   can drive the game; without it (every real console) the network starts only from the «Сетевая игра» menu (04.10) */
/* (06.10) the Eden test channel, no server: sd:/skycoop_test/cmd.txt — one command a line, "pad <a+b|-> lx ly rx ry ms"
   (sticks -1..1) or a console command; read every 0.25 s, then deleted. Only where that folder exists (the emulator). */
void PumpTestChannel() {
    static int state;                                   // 0 unknown, 1 on, 2 off
    static u64 next;
    u64 now = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
    if (state == 2 || now < next) return;
    next = now + 250;
    if (!g_mounted) g_mounted = R_SUCCEEDED(nn::fs::MountSdCard("sd"));
    nn::fs::DirectoryEntryType t;
    if (state == 0) state = g_mounted && R_SUCCEEDED(nn::fs::GetEntryType(&t, "sd:/skycoop_test")) ? 1 : 2;
    if (state != 1) return;
    {   // the module's log, at once (Eden's own log reaches the disk in batches, late)
        static u32 seq;
        static char lb[8192];
        u32 n = LogSince(&seq, lb, sizeof lb);
        constexpr const char* LOGF = "sd:/skycoop_test/log.txt";
        if (n) {
            nn::fs::DirectoryEntryType lt;
            if (R_FAILED(nn::fs::GetEntryType(&lt, LOGF))) nn::fs::CreateFile(LOGF, 0);
            nn::fs::FileHandle lh;
            if (R_SUCCEEDED(nn::fs::OpenFile(&lh, LOGF, nn::fs::OpenMode_Write | nn::fs::OpenMode_Append))) {
                s64 at = 0;
                nn::fs::GetFileSize(&at, lh);
                nn::fs::WriteFile(lh, at, lb, n, nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush));
                nn::fs::CloseFile(lh);
            }
        }
    }
    {   // (06.10) memory: what the process has / uses, every 5 s — to weigh what each mod costs (Morthal ran out with LotD)
        static u64 memAt;
        if (now - memAt > 5000) {
            memAt = now;
            u64 total = 0, used = 0;
            svcGetInfo(&total, 6, CUR_PROCESS_HANDLE, 0);
            svcGetInfo(&used, 7, CUR_PROCESS_HANDLE, 0);
            Log("тест: память занято %lu из %lu МБ", (unsigned long)(used >> 20), (unsigned long)(total >> 20));
        }
    }
    static bool inWorld;
    if (!inWorld && game::Player() && game::Settled()) {
        inWorld = true;
        Log("тест: плагины LotD %d, _ResourcePack %d, SSWTestStub %d, Kanjs %d", game::PluginIndex("LegacyoftheDragonborn.esp"),
            game::PluginIndex("_ResourcePack.esp"), game::PluginIndex("SSWTestStub.esp"), game::PluginIndex("Kanjs - Harkon Katana.esp"));
        Log("тест: игрок в мире");
    }
    constexpr const char* CMD = "sd:/skycoop_test/cmd.txt";
    if (R_FAILED(nn::fs::GetEntryType(&t, CMD))) return;
    nn::fs::FileHandle h;
    if (R_FAILED(nn::fs::OpenFile(&h, CMD, nn::fs::OpenMode_Read))) return;
    static char buf[2048];
    s64 size = 0;
    nn::fs::GetFileSize(&size, h);
    if (size > (s64)sizeof buf - 1) size = sizeof buf - 1;
    nn::fs::ReadFile(h, 0, buf, size);
    nn::fs::CloseFile(h);
    nn::fs::DeleteFile(CMD);
    buf[size] = 0;
    static const char* BTN[16] = {"a", "b", "x", "y", "l3", "r3", "l", "r", "zl", "zr", "plus", "minus", "left", "up", "right", "down"};
    int n = 0;
    for (char* line = buf; line && *line; ) {
        char* e = strchr(line, '\n');
        if (e) *e = 0;
        size_t L = strlen(line);
        if (L && line[L - 1] == '\r') line[--L] = 0;
        if (!strncmp(line, "pad ", 4)) {
            char btn[64] = ""; float lx = 0, ly = 0, rx = 0, ry = 0; unsigned ms = 150;
            sscanf(line + 4, "%63s %f %f %f %f %u", btn, &lx, &ly, &rx, &ry, &ms);
            u64 mask = 0;
            for (char* b = strtok(btn, "+"); b; b = strtok(nullptr, "+"))
                for (int i = 0; i < 16; i++) if (!strcmp(b, BTN[i])) mask |= 1ull << i;
            InjectLocal(mask, (s32)(lx * 32767), (s32)(ly * 32767), (s32)(rx * 32767), (s32)(ry * 32767), ms);
        } else if (L) {
            console::Execute(line);
        }
        n++;
        line = e ? e + 1 : nullptr;
    }
    Log("тест: выполнено %d", n);
}

bool TestAutoConnect() {
    if (!g_mounted) g_mounted = R_SUCCEEDED(nn::fs::MountSdCard("sd"));
    nn::fs::DirectoryEntryType t;
    return g_mounted && R_SUCCEEDED(nn::fs::GetEntryType(&t, "sd:/skycoop_test/autoconnect"));
}

bool Check(bool install) {
    if (!g_mounted) g_mounted = R_SUCCEEDED(nn::fs::MountSdCard("sd"));
    if (!g_mounted) { snprintf(g_status, sizeof g_status, "нет доступа к карте памяти"); return false; }
    static char list[8192];
    snprintf(g_status, sizeof g_status, "спрашиваю сервер…");
    if (!UpdateList(list, sizeof list, 6000)) { snprintf(g_status, sizeof g_status, "нет связи с сервером"); return false; }
    int total = 0, differ = 0, done = 0;
    char failed[96] = "";
    for (char* line = list; line && *line;) {
        char* next = strchr(line, '\n');
        if (next) *next++ = 0;
        char* t1 = strchr(line, '\t');
        char* t2 = t1 ? strchr(t1 + 1, '\t') : nullptr;
        if (t1 && t2) {
            *t1 = 0;
            u64 size = strtoull(t1 + 1, nullptr, 10);
            u32 crc = strtoul(t2 + 1, nullptr, 16);
            char path[256];
            snprintf(path, sizeof path, "sd:/%s", line);
            u64 hs = 0; u32 hc = 0;
            total++;
            bool same = FileCrc(path, &hs, &hc) && hs == size && hc == crc;
            if (!same) {
                differ++;
                if (install) {
                    char tmp[264];
                    snprintf(tmp, sizeof tmp, "%s.new", path);
                    MakeDirs(path);
                    snprintf(g_status, sizeof g_status, "скачиваю %s…", line);
                    bool ok = UpdateDownload(line, tmp, 180000);
                    u64 ns = 0; u32 nc = 0;
                    ok = ok && FileCrc(tmp, &ns, &nc) && ns == size && nc == crc;
                    if (ok) {
                        nn::fs::DeleteFile(path);
                        ok = R_SUCCEEDED(nn::fs::RenameFile(tmp, path));
                    }
                    if (ok) done++;
                    else snprintf(failed, sizeof failed, "%s", line);
                    Log("update %s: %s", line, ok ? "installed" : "failed");
                }
            }
        }
        line = next;
    }
    if (!total) snprintf(g_status, sizeof g_status, "на сервере нет обновлений");
    else if (!differ) snprintf(g_status, sizeof g_status, "всё актуально (файлов: %d)", total);
    else if (!install) snprintf(g_status, sizeof g_status, "есть обновления — файлов: %d из %d", differ, total);
    else if (done == differ) snprintf(g_status, sizeof g_status, "обновлено файлов: %d — перезапусти игру", done);
    else snprintf(g_status, sizeof g_status, "обновлено %d из %d, ошибка: %s", done, differ, failed);
    return differ == 0 || (install && done == differ);
}

}  // namespace coop::updates
