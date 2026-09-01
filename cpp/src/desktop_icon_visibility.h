#pragma once
#include <string>
#include <vector>

struct HiddenDesktopIconState {
    std::wstring name;
    std::wstring path;
    int x = 0;
    int y = 0;
    bool hasPosition = false;
    unsigned long originalAttributes = 0;
    bool hasOriginalAttributes = false;
    bool hidden = true;
};

namespace DesktopIconVisibility {

bool IsHidden(const std::wstring& path);
bool HasHidden();
void SetHidden(const std::wstring& name, const std::wstring& path, bool hidden);
void ShowAll();
void Apply();

const std::vector<HiddenDesktopIconState>& States();
void LoadStates(std::vector<HiddenDesktopIconState> states);

} // namespace DesktopIconVisibility
