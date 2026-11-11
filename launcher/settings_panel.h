#pragma once
#include <windows.h>
#include <string>
HWND create_settings_panel(HWND parent,HFONT font);
void select_settings_panel(HWND panel,const std::wstring& directory);
