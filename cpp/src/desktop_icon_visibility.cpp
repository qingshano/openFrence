#include "desktop_icon_visibility.h"
#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <algorithm>
#include <cstddef>
#include <utility>

namespace {

std::vector<HiddenDesktopIconState> g_states;

HWND FindDesktopListView() {
    auto listViewOf = [](HWND shell) -> HWND {
        if (!shell) return nullptr;
        for (HWND dv = FindWindowExW(shell, nullptr, L"SHELLDLL_DefView", nullptr); dv;
             dv = FindWindowExW(shell, dv, L"SHELLDLL_DefView", nullptr)) {
            HWND lv = FindWindowExW(dv, nullptr, L"SysListView32", nullptr);
            if (lv) return lv;
        }
        return nullptr;
    };
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (HWND lv = listViewOf(progman)) return lv;
    for (HWND ww = FindWindowExW(progman, nullptr, L"WorkerW", nullptr); ww;
         ww = FindWindowExW(progman, ww, L"WorkerW", nullptr))
        if (HWND lv = listViewOf(ww)) return lv;
    for (HWND ww = FindWindowExW(nullptr, nullptr, L"WorkerW", nullptr); ww;
         ww = FindWindowExW(nullptr, ww, L"WorkerW", nullptr))
        if (HWND lv = listViewOf(ww)) return lv;
    return nullptr;
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return lstrcmpiW(a.c_str(), b.c_str()) == 0;
}

std::wstring FileDisplayName(const std::wstring& path, bool stripExtension) {
    const wchar_t* leaf = PathFindFileNameW(path.c_str());
    std::wstring value = leaf ? leaf : path;
    if (stripExtension) {
        size_t dot = value.find_last_of(L'.');
        if (dot != std::wstring::npos) value.resize(dot);
    }
    return value;
}

bool MatchesDesktopText(const HiddenDesktopIconState& state, const std::wstring& text) {
    if (lstrcmpiW(state.name.c_str(), text.c_str()) == 0) return true;
    std::wstring leaf = FileDisplayName(state.path, false);
    if (lstrcmpiW(leaf.c_str(), text.c_str()) == 0) return true;
    std::wstring stem = FileDisplayName(state.path, true);
    return lstrcmpiW(stem.c_str(), text.c_str()) == 0;
}

bool IsParked(const POINT& pt) {
    return pt.x > 10000 || pt.y > 10000 || pt.x < -10000 || pt.y < -10000;
}

class RemoteListView {
    struct Block {
        LVITEMW item;
        wchar_t text[512];
        POINT point;
    };

    HWND m_hwnd = nullptr;
    HANDLE m_process = nullptr;
    void* m_remote = nullptr;

    bool Send(UINT msg, WPARAM wp, LPARAM lp, DWORD_PTR* result = nullptr) const {
        DWORD_PTR localResult = 0;
        LRESULT ok = SendMessageTimeoutW(m_hwnd, msg, wp, lp,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, &localResult);
        if (result) *result = localResult;
        return ok != 0;
    }

public:
    explicit RemoteListView(HWND hwnd) : m_hwnd(hwnd) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid) return;
        m_process = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ |
                                PROCESS_VM_WRITE | PROCESS_QUERY_LIMITED_INFORMATION,
                                FALSE, pid);
        if (!m_process) return;
        m_remote = VirtualAllocEx(m_process, nullptr, sizeof(Block),
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }

    ~RemoteListView() {
        if (m_remote) VirtualFreeEx(m_process, m_remote, 0, MEM_RELEASE);
        if (m_process) CloseHandle(m_process);
    }

    bool Valid() const { return m_hwnd && m_process && m_remote; }

    int Count() const {
        DWORD_PTR result = 0;
        return Send(LVM_GETITEMCOUNT, 0, 0, &result) ? (int)result : 0;
    }

    bool Text(int index, std::wstring& out) const {
        Block local = {};
        local.item.mask = LVIF_TEXT;
        local.item.iItem = index;
        local.item.iSubItem = 0;
        local.item.pszText = (LPWSTR)((BYTE*)m_remote + offsetof(Block, text));
        local.item.cchTextMax = (int)(sizeof(local.text) / sizeof(local.text[0]));
        SIZE_T wrote = 0;
        if (!WriteProcessMemory(m_process, m_remote, &local, sizeof(local), &wrote) ||
            wrote != sizeof(local)) return false;
        DWORD_PTR result = 0;
        if (!Send(LVM_GETITEMTEXTW, (WPARAM)index, (LPARAM)m_remote, &result)) return false;
        wchar_t text[512] = {};
        SIZE_T read = 0;
        const void* remoteText = (BYTE*)m_remote + offsetof(Block, text);
        if (!ReadProcessMemory(m_process, remoteText, text, sizeof(text), &read)) return false;
        text[(sizeof(text) / sizeof(text[0])) - 1] = L'\0';
        out = text;
        return true;
    }

    bool Position(int index, POINT& point) const {
        void* remotePoint = (BYTE*)m_remote + offsetof(Block, point);
        DWORD_PTR result = 0;
        if (!Send(LVM_GETITEMPOSITION, (WPARAM)index, (LPARAM)remotePoint, &result) || !result)
            return false;
        SIZE_T read = 0;
        return ReadProcessMemory(m_process, remotePoint, &point, sizeof(point), &read) &&
               read == sizeof(point);
    }

    bool SetPosition(int index, POINT point) const {
        void* remotePoint = (BYTE*)m_remote + offsetof(Block, point);
        SIZE_T wrote = 0;
        if (!WriteProcessMemory(m_process, remotePoint, &point, sizeof(point), &wrote) ||
            wrote != sizeof(point)) return false;
        DWORD_PTR result = 0;
        return Send(LVM_SETITEMPOSITION32, (WPARAM)index, (LPARAM)remotePoint, &result) &&
               result != 0;
    }
};

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
        it->hidden = false; // Apply restores its saved position, then erases it.
    }
}

void ShowAll() {
    for (auto& state : g_states) state.hidden = false;
    Apply();
}

void Apply() {
    HWND list = FindDesktopListView();
    RemoteListView view(list);
    if (!view.Valid()) return;
    int count = view.Count();
    if (count <= 0 || count > 4096) return;

    for (int i = 0; i < count; i++) {
        std::wstring text;
        if (!view.Text(i, text) || text.empty()) continue;
        for (auto& state : g_states) {
            if (!MatchesDesktopText(state, text)) continue;
            POINT current = {};
            if (!view.Position(i, current)) break;
            if (state.hidden) {
                if (!state.hasPosition && !IsParked(current)) {
                    state.x = current.x;
                    state.y = current.y;
                    state.hasPosition = true;
                }
                POINT parked{ 30000 + (i % 32) * 4, 30000 + (i / 32) * 4 };
                if (!IsParked(current)) view.SetPosition(i, parked);
            } else if (state.hasPosition) {
                view.SetPosition(i, POINT{ state.x, state.y });
            }
            break;
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
