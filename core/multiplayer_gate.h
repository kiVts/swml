#pragma once
bool core_multiplayer_install();
void core_multiplayer_tick();
// Static text; safe to read from the UI thread without touching Steam.
const char* core_multiplayer_notice();
