#pragma once
// A mod's short text on SWML's line in the bottom-left corner, after the loaded-mod count:
//
//     v1.15.27 64Bit | SWML V4.7.11 9 mods loaded | CATCHING UP...
//
//     status_line("CATCHING UP...", SWML_STATUS_RED);  // show this mod's text (red), or replace what it showed
//     status_line("synced");                           // the label's own colour
//     status_line(nullptr);                            // remove it ("" does the same)
//
// Call it from any thread, as often as you like: the core copies the text and only touches the label when something
// changed. Each mod has one segment, shown in the order mods first set one. Text is one line of at most
// SWML_STATUS_LINE_MAX bytes (longer text is cut, control characters become spaces). Colours are native AABBGGRR
// (SWML_STATUS_RGB); 0 is the label's own colour. The line can show three different colours at once: a segment
// whose colour doesn't fit keeps the label's colour. A mod that is unloaded loses its segment. Needs SWML 4.7.11 or
// newer: on an older core status_line() returns false and nothing is shown.

#include "swml_api.h"

#define SWML_STATUS_LINE_MAX 63
#define SWML_STATUS_RGB(r, g, b) (0xff000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))
#define SWML_STATUS_RED SWML_STATUS_RGB(0xf2, 0x1f, 0x1a)
#define SWML_STATUS_YELLOW SWML_STATUS_RGB(0xf2, 0xc8, 0x1a)
#define SWML_STATUS_GREEN SWML_STATUS_RGB(0x40, 0xd0, 0x40)

#ifdef __cplusplus
// C++ mods built with the SDK runtime (swml_sdk.cpp). True when the core shows the text.
bool status_line(const char* text, uint32_t color = 0);
#endif
