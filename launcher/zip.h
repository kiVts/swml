#pragma once
#include <string>

// Extracts every entry of a .zip into dest (which must exist). false + error text on the first problem.
bool zip_extract(const std::wstring& zip, const std::wstring& dest, std::wstring& error);
