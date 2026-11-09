#pragma once
#include <string>
#include <vector>

namespace swml_manager {
bool install(const std::wstring& root, const std::wstring& source, const std::wstring& id, std::wstring& error);
bool rollback(const std::wstring& root, const std::wstring& id, std::wstring& error);
bool doctor(const std::wstring& root, const std::wstring& path, std::wstring& error);
}
