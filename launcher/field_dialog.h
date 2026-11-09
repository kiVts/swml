#pragma once
#include <windows.h>
#include <optional>
#include <string>

std::optional<std::wstring> prompt_value(HWND owner, const wchar_t* title, const std::wstring& initial);
