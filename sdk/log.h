#pragma once
#include <cstdarg>

// Mod-side logging (swml_sdk.cpp): one line in logs\mods\<mod id>.log, tagged with this mod's id.
void swlog(const char* fmt, ...);
const wchar_t* log_dir();      // swml logs folder: put output files (traces, csv) here
const wchar_t* settings_ini(); // this mod's settings.ini
const wchar_t* mod_dir();      // this mod's folder
