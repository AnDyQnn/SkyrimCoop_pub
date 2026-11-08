#pragma once
#include "common.hpp"

/* The clan's shared chest (chest.cpp). */
namespace coop::chest {

void Open(const char* list);     // any thread: the server's "#chest <hex>:<n> ..." answer -> the container opens
void Denied(const char* text);   // network thread: "#chestdeny <hex>:<n> ..."
void Pump();                     // game thread

}  // namespace coop::chest
