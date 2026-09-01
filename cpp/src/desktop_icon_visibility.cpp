#include "desktop_icon_visibility.h"
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <algorithm>
#include <utility>

namespace {

std::vector<HiddenDesktopIconState> g_states;

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return lstrcmpiW(a.c_str(), b.c_str()) == 0;
}

bool SameDirectory(const std::wstring& a, const wchar_t* b) {
    return b && *b && lstrcmpiW(a.c_str(), b) == 0;
}

// Only change attributes on direct children of the user's or public Desktop.
// A mapped fence may show an arbitrary directory; hiding those files in their
// real folder would be surprising and is outside this feature's scope.
bool IsPhysicalDesktopItem(const std::wstring& path) {
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;

    wchar_t parent[MAX_PATH] = {};
    if (path.size() >= _countof(parent)) return false;
    wcscpy_s(parent, path.c_str());
    if (!PathRemoveFileSpecW(parent)) return false;

    wchar_t userDesktop[MAX_PATH] = {};
    wchar_t publicDesktop[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, SHGFP_TYPE_CURRENT,
                     userDesktop);
    SHGetFolderPathW(nullptr, CSIDL_COMMON_DESKTOPDIRECTORY, nullptr,
                     SHGFP_TYPE_CURRENT, publicDesktop);
    return SameDirectory(parent, userDesktop) || SameDirectory(parent, publicDesktop);
}

void NotifyAttributeChange(const std::wstring& path) {
    SHChangeNotify(SHCNE_ATTRIBUTES, SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
                   path.c_str(), nullptr);
}

} // namespace

namespace DesktopIconVisibility {

bool IsHidden(const std::wstring& path) {
    return std::any_of(g_states.begin(), g_states.end(), [&](const auto& state) {
        return state.hidden && SamePath(state.path, path);
    });
}

bool HasHidden() {
    return std::any_of(g_states.begin(), g_states.end(),
                       [](const auto& state) { return state.hidden; });
}

void SetHidden(const std::wstring& name, const std::wstring& path, bool hidden) {
    auto it = std::find_if(g_states.begin(), g_states.end(), [&](const auto& state) {
        return SamePath(state.path, path);
    });
    if (hidden) {
        if (!IsPhysicalDesktopItem(path)) return;
        if (it == g_states.end()) {
            HiddenDesktopIconState state;
            state.name = name;
            state.path = path;
            g_states.push_back(std::move(state));
        } else {
            it->name = name;
            it->hidden = true;
        }
    } else if (it != g_states.end()) {
        it->hidden = false;
    }
}

void ShowAll() {
    for (auto& state : g_states) state.hidden = false;
    Apply();
}

void Apply() {
    for (auto& state : g_states) {
        DWORD current = GetFileAttributesW(state.path.c_str());
        if (current == INVALID_FILE_ATTRIBUTES) continue;

        if (state.hidden) {
            if (!IsPhysicalDesktopItem(state.path)) continue;
            if (!state.hasOriginalAttributes) {
                state.originalAttributes = current;
                state.hasOriginalAttributes = true;
            }
            DWORD hiddenAttributes =
                (current & ~FILE_ATTRIBUTE_NORMAL) |
                FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
            if (hiddenAttributes != current &&
                SetFileAttributesW(state.path.c_str(), hiddenAttributes))
                NotifyAttributeChange(state.path);
        } else if (state.hasOriginalAttributes) {
            DWORD restored = state.originalAttributes;
            if (restored == 0) restored = FILE_ATTRIBUTE_NORMAL;
            if (restored != current && SetFileAttributesW(state.path.c_str(), restored))
                NotifyAttributeChange(state.path);
        }
    }

    g_states.erase(std::remove_if(g_states.begin(), g_states.end(),
        [](const auto& state) { return !state.hidden; }), g_states.end());
}

const std::vector<HiddenDesktopIconState>& States() { return g_states; }

void LoadStates(std::vector<HiddenDesktopIconState> states) {
    g_states = std::move(states);
    for (auto& state : g_states) state.hidden = true;
}

} // namespace DesktopIconVisibility
