#pragma once
#include <windows.h>
#include <string>

// Reads Explorer's persisted sort for a folder from the Shell Bags registry.
// column: 0=Name, 1=DateModified, 2=Size, 3=Type.
// Returns false when no usable entry exists (caller falls back to Name ascending).
bool QueryExplorerSortForFolder(const std::wstring& folder, int& column, bool& ascending);
