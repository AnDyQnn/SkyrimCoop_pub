#pragma once
#include "common.hpp"

/* Player settings, stored in the game's own save mount ("save:/skyrimcoop.txt", lines "key=value"):
     addr  server address typed by the player ("" = find the room on the network automatically)
     name  player name shown to the others */
namespace coop::settings {

/* Hooks nn::fs::MountSaveData to know when "save:" exists (call from exl_main). */
void InstallHooks();

/* Reads the file once the game has mounted its save data (cheap to call repeatedly). */
void LoadIfNeeded();
bool Loaded();
bool SaveMounted();             // the game's "save:" is mounted (online saves read / write it)                  // the settings file was looked at (save data mounted)

/* Copy of the manual address ("host" or "host:port"); false = none (auto search). */
bool ManualAddress(char* host, u32 hostSize, u16* port);
void SetAddress(const char* value);         // "" = back to auto search; saves and flags a reconnect

void GetName(char* out, u32 size);
void SetName(const char* value);            // saves and flags a reconnect

/* Set when the address or name changed; the network thread reconnects and clears it. */
bool TakeChanged();

/* System keyboard (blocking; call from a worker thread). false = cancelled. */
bool AskText(const char16_t* header, const char16_t* guide, const char* initial, char* out, u32 outSize);

}  // namespace coop::settings
