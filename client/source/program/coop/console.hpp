#pragma once
#include "common.hpp"

/* Console commands like the PC "~" console (coc, tgm, player.additem ...). Skyrim on Switch has
   no console menu, but its command path is still there: ConsoleMenu::ExecuteCommand builds a
   Script with the typed text and queues it; the main loop runs Script::CompileAndRun in the
   SysWindow (console) mode. We do the same (1.1.177, signature-checked in console.cpp). */
namespace coop::console {

bool Resolve();                    // false = this build doesn't match, the menu hides the button
bool Available();
bool Execute(const char* command); // built-in (tgm, tcl, tai, tm, coc, tp) or queued for the script compiler
void PumpBuiltins();               // game thread (VM pump hook): runs a pending built-in command
void Prompt();                     // system keyboard -> Execute (blocking; menu thread)

}  // namespace coop::console
