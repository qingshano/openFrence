// TEMP diagnostic: probe the shell NewMenuHandler's InvokeCommand contract.
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <atlbase.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>

static void Log(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    FILE* f = _wfopen(L"D:\\person\\openFences\\test_newmenu.log", L"a, ccs=UTF-8");
    if (f) { fwprintf(f, L"%s\r\n", buf); fclose(f); }
    wprintf(L"%s\n", buf);
}

static const CLSID kCLSID_NewMenu = { 0xD969A300, 0xE7FF, 0x11D0,
                                      { 0xA9, 0x3B, 0x00, 0xA0, 0xC9, 0x0F, 0x27, 0x19 } };

struct ItemInfo { UINT id; std::wstring text, verbW; std::string verbA; };

static void DumpSub(HMENU m, IContextMenu* cm, UINT idBase, std::vector<ItemInfo>& items) {
    int n = GetMenuItemCount(m);
    for (int i = 0; i < n; i++) {
        MENUITEMINFOW mii = { sizeof(mii) };
        mii.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STRING;
        wchar_t text[256] = {};
        mii.dwTypeData = text; mii.cch = 256;
        if (!GetMenuItemInfoW(m, i, TRUE, &mii)) continue;
        if (mii.fType & MFT_SEPARATOR) { Log(L"  ---"); continue; }
        wchar_t vw[128] = {}; char va[128] = {};
        if (cm && mii.wID != 0xFFFFFFFF && mii.wID >= idBase) {
            cm->GetCommandString(mii.wID - idBase, GCS_VERBW, nullptr, (LPSTR)vw, sizeof(vw));
            cm->GetCommandString(mii.wID - idBase, GCS_VERBA, nullptr, va, sizeof(va));
        }
        Log(L"  id=%u text=\"%s\" verbW=\"%s\" verbA=\"%S\"", mii.wID, text, vw, va);
        items.push_back({ mii.wID, text, vw, va });
    }
}

static std::vector<std::wstring> Snap(const std::wstring& dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { out.push_back(fd.cFileName); } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return out;
}

// Invoke `offset` and report what appeared in `dir`; delete the new entry.
static void ProbeInvoke(IContextMenu* cm, const std::wstring& dir,
                        int offset, const wchar_t* label) {
    auto before = Snap(dir);
    CMINVOKECOMMANDINFO ici = {};
    ici.cbSize = sizeof(ici);
    ici.lpVerb = MAKEINTRESOURCEA(offset);
    ici.nShow = SW_SHOWNORMAL;
    HRESULT hr = cm->InvokeCommand(&ici);
    Log(L"Invoke %s offset=%d -> 0x%08X", label, offset, (unsigned)hr);
    auto after = Snap(dir);
    for (const auto& n : after) {
        bool isNew = true;
        for (const auto& o : before) if (o == n) { isNew = false; break; }
        if (isNew) {
            Log(L"  CREATED: \"%s\"", n.c_str());
            std::wstring p = dir + L"\\" + n;
            DWORD attr = GetFileAttributesW(p.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
                RemoveDirectoryW(p.c_str());
            else
                DeleteFileW(p.c_str());
        }
    }
}

static void RunCase(PCIDLIST_ABSOLUTE pidl, const std::wstring& dir, const wchar_t* name) {
    Log(L"===== CASE %s (dir=%s) =====", name, dir.c_str());
    CComPtr<IContextMenu> cm;
    HRESULT hr = cm.CoCreateInstance(kCLSID_NewMenu, nullptr, CLSCTX_INPROC_SERVER);
    Log(L"CoCreate=0x%08X", (unsigned)hr);
    if (FAILED(hr)) return;
    CComPtr<IShellExtInit> init;
    hr = cm.QueryInterface(&init);
    if (SUCCEEDED(hr)) hr = init->Initialize(pidl, nullptr, nullptr);
    Log(L"Initialize=0x%08X", (unsigned)hr);
    if (FAILED(hr)) return;

    HMENU menu = CreatePopupMenu();
    hr = cm->QueryContextMenu(menu, 0, 0x200, 0x7FFF, CMF_NORMAL);
    Log(L"QueryContextMenu=0x%08X", (unsigned)hr);
    HMENU sub = GetSubMenu(menu, 0);
    CComPtr<IContextMenu3> cm3;
    cm.QueryInterface(&cm3);
    if (sub && cm3) {
        LRESULT lr = 0;
        cm3->HandleMenuMsg2(WM_INITMENUPOPUP, (WPARAM)sub, 0, &lr);
        Log(L"after WM_INITMENUPOPUP:");
        std::vector<ItemInfo> items;
        DumpSub(sub, cm, 0x200, items);

        // Invoke "Folder" candidates: its menu id tells us the offset.
        for (const auto& it : items) {
            if (it.text.find(L"文件夹") != std::wstring::npos ||
                it.text.find(L"Folder") != std::wstring::npos) {
                ProbeInvoke(cm, dir, (int)(it.id - 0x200), L"FOLDER(offset=id-base)");
                ProbeInvoke(cm, dir, (int)(it.id - 0x200) - 1, L"FOLDER(offset=id-base-1)");
            }
            if (it.text.find(L"文本文档") != std::wstring::npos ||
                it.text.find(L"Text") != std::wstring::npos) {
                ProbeInvoke(cm, dir, (int)(it.id - 0x200), L"TXT(offset=id-base)");
                ProbeInvoke(cm, dir, (int)(it.id - 0x200) - 1, L"TXT(offset=id-base-1)");
            }
        }
    }
    DestroyMenu(menu);
}

int main() {
    FILE* f = _wfopen(L"D:\\person\\openFences\\test_newmenu.log", L"w, ccs=UTF-8");
    if (f) fclose(f);
    CoInitialize(nullptr);

    wchar_t desktopDir[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktopDir);
    wchar_t tempDir[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, tempDir);
    std::wstring testFolder = std::wstring(tempDir) + L"\\openfences_newmenu_test";
    CreateDirectoryW(testFolder.c_str(), nullptr);

    // Case 1: desktop PIDL (empty)
    {
        PIDLIST_ABSOLUTE pidl = nullptr;
        SHGetSpecialFolderLocation(nullptr, CSIDL_DESKTOP, &pidl);
        Log(L"desktop pidl=%p isEmpty=%d", pidl, pidl ? ILIsEmpty(pidl) : -1);
        RunCase(pidl, desktopDir, L"DESKTOP-PIDL");
        if (pidl) CoTaskMemFree(pidl);
    }
    // Case 2: physical desktop folder path as PIDL
    {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(desktopDir, nullptr, &pidl, 0, nullptr))) {
            Log(L"desktopDir pidl isEmpty=%d", ILIsEmpty(pidl));
            RunCase(pidl, desktopDir, L"DESKTOPDIR-PIDL");
            CoTaskMemFree(pidl);
        }
    }
    // Case 3: plain folder PIDL
    {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(testFolder.c_str(), nullptr, &pidl, 0, nullptr))) {
            RunCase(pidl, testFolder, L"FOLDER-PIDL");
            CoTaskMemFree(pidl);
        }
    }

    // Case 4: app-order — DestroyMenu BEFORE invoke (the app's current flow)
    {
        Log(L"===== CASE DESTROY-FIRST (folder pidl) =====");
        PIDLIST_ABSOLUTE pidl = nullptr;
        SHParseDisplayName(testFolder.c_str(), nullptr, &pidl, 0, nullptr);
        CComPtr<IContextMenu> cm;
        cm.CoCreateInstance(kCLSID_NewMenu, nullptr, CLSCTX_INPROC_SERVER);
        CComPtr<IShellExtInit> init;
        cm.QueryInterface(&init);
        init->Initialize(pidl, nullptr, nullptr);
        HMENU menu = CreatePopupMenu();
        cm->QueryContextMenu(menu, 0, 0x200, 0x7FFF, CMF_NORMAL);
        CComPtr<IContextMenu3> cm3;
        cm.QueryInterface(&cm3);
        LRESULT lr = 0;
        cm3->HandleMenuMsg2(WM_INITMENUPOPUP, (WPARAM)GetSubMenu(menu, 0), 0, &lr);
        DestroyMenu(menu);   // app destroys before invoking
        ProbeInvoke(cm, testFolder, 8, L"TXT after DestroyMenu");
        ProbeInvoke(cm, testFolder, 1, L"FOLDER after DestroyMenu");
        CoTaskMemFree(pidl);
    }
    // Case 5: hwnd set on invoke (app passes the fence hwnd)
    {
        Log(L"===== CASE HWND-SET (folder pidl) =====");
        PIDLIST_ABSOLUTE pidl = nullptr;
        SHParseDisplayName(testFolder.c_str(), nullptr, &pidl, 0, nullptr);
        CComPtr<IContextMenu> cm;
        cm.CoCreateInstance(kCLSID_NewMenu, nullptr, CLSCTX_INPROC_SERVER);
        CComPtr<IShellExtInit> init;
        cm.QueryInterface(&init);
        init->Initialize(pidl, nullptr, nullptr);
        HMENU menu = CreatePopupMenu();
        cm->QueryContextMenu(menu, 0, 0x200, 0x7FFF, CMF_NORMAL);
        CComPtr<IContextMenu3> cm3;
        cm.QueryInterface(&cm3);
        LRESULT lr = 0;
        cm3->HandleMenuMsg2(WM_INITMENUPOPUP, (WPARAM)GetSubMenu(menu, 0), 0, &lr);
        HWND probeHwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
                                         0, 0, 10, 10, nullptr, nullptr, nullptr, nullptr);
        auto before = Snap(testFolder);
        CMINVOKECOMMANDINFO ici = {};
        ici.cbSize = sizeof(ici);
        ici.hwnd = probeHwnd;
        ici.lpVerb = MAKEINTRESOURCEA(8);
        ici.nShow = SW_SHOWNORMAL;
        HRESULT hrI = cm->InvokeCommand(&ici);
        Log(L"Invoke TXT hwnd-set -> 0x%08X", (unsigned)hrI);
        DestroyMenu(menu);
        DestroyWindow(probeHwnd);
        CoTaskMemFree(pidl);
    }

    RemoveDirectoryW(testFolder.c_str());
    CoUninitialize();
    Log(L"DONE");
    return 0;
}
