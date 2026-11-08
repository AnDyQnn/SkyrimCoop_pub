#pragma once
#include "common.hpp"
/* Legacy of the Dragonborn's book sorting (06.10): on PC its keywords come from KID (LotD_Booksort_KID.ini); the
   Switch has no KID, so the module adds them to the books in memory once the plugins are loaded — the table is
   made from the pack's plugins by SkyrimSwitch/work/port/lotd_booksort.py with KID's rules. */
namespace coop::booksort {
void Pump();                          // game thread (VM pump); does its work once
}  // namespace coop::booksort
