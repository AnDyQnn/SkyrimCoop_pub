#include "settings.hpp"
#include "lib.hpp"
#include <nn/fs.hpp>
#include <nn/os.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* nn::swkbd, layout as Skyrim itself uses it (SDK 14.3, see notes/plan.md):
   ShowKeyboardArg = KeyboardConfig (MakePreset) + work buffer pointer/size at +0x4D0. */
namespace nn::swkbd {
    struct KeyboardConfig;
    struct ShowKeyboardArg;
    struct String { char* buffer; u64 size; };
    enum Preset : int { Preset_Default = 0 };
    u64 GetRequiredWorkBufferSize(bool);
    u64 GetRequiredStringBufferSize();
    void MakePreset(KeyboardConfig* config, Preset preset);
    void SetInitialTextUtf8(ShowKeyboardArg* arg, const char* text);
    u32 ShowKeyboard(String* out, const ShowKeyboardArg& arg);
}
namespace nn::account { struct Uid { u64 value[2]; }; }
namespace nn::fs {
    Result MountSaveData(const char* mount, const nn::account::Uid& user);
    Result CommitSaveData(const char* mount);
    Result DeleteFile(const char* path);
    Result GetEntryType(DirectoryEntryType* type, const char* path);
}

namespace coop::settings {
namespace {

constexpr const char* PATH = "save:/skyrimcoop.txt";
constexpr u32 ARG_SIZE = 0x500;
constexpr u32 OFF_HEADER = 0x24, OFF_GUIDE = 0x1A8, OFF_MAX_LEN = 0x3AC, OFF_FORM = 0x3B8, OFF_UTF8 = 0x3BD,
              OFF_WORK_BUF = 0x4D0, OFF_WORK_SIZE = 0x4D8;
constexpr u64 WORK_MAX = 0x20000, STRING_MAX = 0x2000;

nn::os::MutexType g_lock;
bool g_lockInit;
char g_address[64];          // "" = auto search
char g_name[32] = "Игрок";
bool g_loaded;
volatile bool g_saveMounted;
volatile bool g_changed;

nn::os::MutexType g_kbLock;  // one keyboard at a time
bool g_kbLockInit;
alignas(0x1000) u8 g_work[WORK_MAX];
alignas(0x1000) char g_out[STRING_MAX];
alignas(8) u8 g_arg[ARG_SIZE];

void Lock()   { if (!g_lockInit) { nn::os::InitializeMutex(&g_lock, false, 0); g_lockInit = true; } nn::os::LockMutex(&g_lock); }
void Unlock() { nn::os::UnlockMutex(&g_lock); }

HOOK_DEFINE_TRAMPOLINE(MountSave) {
    static Result Callback(const char* mount, const nn::account::Uid& user) {
        Result r = Orig(mount, user);
        if (R_SUCCEEDED(r) && strcmp(mount, "save") == 0) g_saveMounted = true;
        return r;
    }
};

void PutUtf16(u32 off, const char16_t* s, u32 maxChars) {
    auto dst = reinterpret_cast<char16_t*>(g_arg + off);
    u32 i = 0;
    for (; s && s[i] && i + 1 < maxChars; i++) dst[i] = s[i];
    dst[i] = 0;
}

bool Save() {
    if (!g_saveMounted) return false;
    char text[160];
    Lock();
    snprintf(text, sizeof text, "addr=%s\nname=%s\n", g_address, g_name);
    Unlock();
    u64 len = strlen(text);
    nn::fs::DirectoryEntryType t;
    if (R_SUCCEEDED(nn::fs::GetEntryType(&t, PATH))) nn::fs::DeleteFile(PATH);
    if (R_FAILED(nn::fs::CreateFile(PATH, len))) return false;
    nn::fs::FileHandle h;
    if (R_FAILED(nn::fs::OpenFile(&h, PATH, nn::fs::OpenMode_Write))) return false;
    Result r = nn::fs::WriteFile(h, 0, text, len, nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush));
    nn::fs::CloseFile(h);
    return R_SUCCEEDED(r) && R_SUCCEEDED(nn::fs::CommitSaveData("save"));
}

void ParseLine(const char* line) {
    if (!strncmp(line, "addr=", 5)) snprintf(g_address, sizeof g_address, "%s", line + 5);
    else if (!strncmp(line, "name=", 5)) { if (line[5]) snprintf(g_name, sizeof g_name, "%s", line + 5); }
    else if (!strchr(line, '=')) snprintf(g_address, sizeof g_address, "%s", line);   // old format: address only
}

}  // namespace

void InstallHooks() {
    using MountFn = Result (*)(const char*, const nn::account::Uid&);
    MountSave::InstallAtPtr(reinterpret_cast<uintptr_t>(static_cast<MountFn>(&nn::fs::MountSaveData)));
}

void LoadIfNeeded() {
    if (g_loaded || !g_saveMounted) return;
    g_loaded = true;
    nn::fs::DirectoryEntryType t;
    if (R_FAILED(nn::fs::GetEntryType(&t, PATH))) return;          // no file yet: defaults
    nn::fs::FileHandle h;
    if (R_FAILED(nn::fs::OpenFile(&h, PATH, nn::fs::OpenMode_Read))) return;
    char buf[160] = {};
    s64 size = 0;
    nn::fs::GetFileSize(&size, h);
    if (size > 0 && size < (s64)sizeof buf) nn::fs::ReadFile(h, 0, buf, size);
    nn::fs::CloseFile(h);
    Lock();
    char oldName[32], oldAddr[64];
    memcpy(oldName, g_name, sizeof oldName);
    memcpy(oldAddr, g_address, sizeof oldAddr);
    for (char* line = buf; line && *line;) {
        char* next = strchr(line, '\n');
        if (next) *next++ = 0;
        ParseLine(line);
        line = next;
    }
    /* the save data is mounted a while after boot: if we already connected with the defaults, reconnect */
    if (strcmp(oldName, g_name) || strcmp(oldAddr, g_address)) g_changed = true;
    Unlock();
}

bool Loaded() { return g_loaded; }
bool SaveMounted() { return g_saveMounted; }

bool ManualAddress(char* host, u32 hostSize, u16* port) {
    char a[64];
    Lock(); strcpy(a, g_address); Unlock();
    if (!a[0]) return false;
    char* colon = strchr(a, ':');
    if (colon) {
        *colon = 0;
        int p = atoi(colon + 1);
        if (p > 0 && p < 65536) *port = (u16)p;
    }
    snprintf(host, hostSize, "%s", a);
    return true;
}

void SetAddress(const char* value) {
    Lock(); snprintf(g_address, sizeof g_address, "%s", value); Unlock();
    Save();
    g_changed = true;
}

void GetName(char* out, u32 size) { Lock(); snprintf(out, size, "%s", g_name); Unlock(); }

void SetName(const char* value) {
    if (!value[0]) return;
    Lock(); snprintf(g_name, sizeof g_name, "%s", value); Unlock();
    Save();
    g_changed = true;
}

bool TakeChanged() { return __atomic_exchange_n(&g_changed, false, __ATOMIC_ACQ_REL); }

bool AskText(const char16_t* header, const char16_t* guide, const char* initial, char* out, u32 outSize) {
    if (!g_kbLockInit) { nn::os::InitializeMutex(&g_kbLock, false, 0); g_kbLockInit = true; }
    nn::os::LockMutex(&g_kbLock);
    bool ok = false;
    u64 work = nn::swkbd::GetRequiredWorkBufferSize(false);
    u64 outBuf = nn::swkbd::GetRequiredStringBufferSize();
    if (work <= WORK_MAX && outBuf <= STRING_MAX) {
        memset(g_arg, 0, sizeof g_arg);
        nn::swkbd::MakePreset(reinterpret_cast<nn::swkbd::KeyboardConfig*>(g_arg), nn::swkbd::Preset_Default);
        PutUtf16(OFF_HEADER, header, 0x41);
        PutUtf16(OFF_GUIDE, guide, 0x101);
        *reinterpret_cast<s32*>(g_arg + OFF_MAX_LEN) = (s32)(outSize - 1 < 500 ? outSize - 1 : 500);
        *reinterpret_cast<s32*>(g_arg + OFF_FORM) = 0;        // one line
        g_arg[OFF_UTF8] = 1;                                   // result as UTF-8
        memset(g_work, 0, work);
        *reinterpret_cast<void**>(g_arg + OFF_WORK_BUF) = g_work;
        *reinterpret_cast<u64*>(g_arg + OFF_WORK_SIZE) = work;
        if (initial && initial[0]) nn::swkbd::SetInitialTextUtf8(reinterpret_cast<nn::swkbd::ShowKeyboardArg*>(g_arg), initial);
        memset(g_out, 0, outBuf);
        nn::swkbd::String res{g_out, outBuf};
        u32 rc = nn::swkbd::ShowKeyboard(&res, *reinterpret_cast<nn::swkbd::ShowKeyboardArg*>(g_arg));
        if (rc == 0) { snprintf(out, outSize, "%s", g_out); ok = true; }
    }
    nn::os::UnlockMutex(&g_kbLock);
    return ok;
}

}  // namespace coop::settings
