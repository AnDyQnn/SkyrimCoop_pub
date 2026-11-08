#include "coop/menu.hpp"
#include "lib.hpp"
#include "coop/client.hpp"
#include "coop/config.hpp"
#include "coop/game.hpp"
#include "coop/settings.hpp"

extern "C" void exl_main(void* x0, void* x1) {
    exl::hook::Initialize();

    /* Engine pieces are found from Papyrus native registrations (no per-version offsets).
       If anything is missing the module stays network-only instead of touching the game. */
#if defined(SSW_OFFLINE) && !defined(SSW_TESTCTL)
    if (coop::game::Resolve())             // offline: the controller hook only for the module's own presses
        coop::game::InstallFrameHook();
#else
    if (coop::game::Resolve())
        coop::game::InstallFrameHook();
#endif

#if !defined(SSW_OFFLINE) || defined(SSW_TESTCTL)      // SSW_TESTCTL: the offline build driven by the test tools
    /* the multiplayer part only: settings save, "Сетевая игра" on the title screen, the network thread */
    coop::settings::InstallHooks();
    coop::menu::InstallTitleEntry();
    coop::StartClient();
#endif
}

extern "C" NORETURN void exl_exception_entry() {
    EXL_ABORT("Default exception handler called!");
}
