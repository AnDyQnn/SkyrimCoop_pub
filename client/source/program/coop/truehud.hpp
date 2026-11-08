#pragma once
#include "common.hpp"

/* TrueHUD (ersh1, github.com/ersh1/TrueHUD, GPL-3.0 with the modding exception) ported to the Switch build:
   its own swf (Interface/TrueHUD.swf + TrueHUD_Widgets.swf) loaded inside our HUD, its C++ logic here. */
namespace coop::truehud {

void Install();                       // once, after game::Resolve: takes the HUD's GameDelegate slot
void SetBossDir(int d);
void SetBossWidth(int w);             // test aid: setWidth again on the boss bars              // test aid: the boss bars' fill direction
void HudText(const char* text);      // (07.10) the big line in the middle of the screen ("" hides it)
void OnHudCall(void* fxDelegateArgs); // the HUD's per-frame call (our HUDMenu.as): the whole TrueHUD update

}  // namespace coop::truehud
