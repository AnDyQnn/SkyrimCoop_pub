#pragma once
#include "common.hpp"

/* In-game co-op menu in Skyrim's own message box (the one Debug.MessageBox shows), opened by
   holding both sticks. It runs on its own thread; the boxes themselves are created from the
   game thread in PumpUI (called by the VM pump hook in game.cpp). */
namespace coop::menu {

/* 1.1.177 CreateMessage (Debug.MessageBox calls it), found and signature-checked in game.cpp. */
void SetCreateMessage(uintptr_t fn);
bool Available();

void Open();          // no-op if the menu is already open
/* "СЕТЕВАЯ ИГРА" on the title screen (our startmenu.gfx calls the empty HELP handler, which we replace):
   rooms, direct connection, name, online characters. */
void OpenOnline();
void InstallTitleEntry();
void PumpUI();        // game thread: shows a pending message box
/* a yes / no box without the co-op menu (03.10); false if a box is already up. onYes runs on the game thread. */
bool Offer(const char* text, const char* yes, const char* no, void (*onYes)());

}  // namespace coop::menu
