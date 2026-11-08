#pragma once
#include "common.hpp"

/* Mod / game file updates from the server onto the SD card (see updates.cpp). Blocking: worker threads only. */
namespace coop::updates {

bool Check(bool install);     // compare (and download when install); result text in Status()
const char* Status();
void PumpTestChannel();           // (06.10) Eden only: sd:/skycoop_test/cmd.txt commands, no server
bool TestAutoConnect();            // the test machines' marker file (Eden): connect at boot

}  // namespace coop::updates
