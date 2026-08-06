// icon_diag — what does the shell image list actually contain for a file?
//
// For each path on the command line this prints, per image list
// (JUMBO / EXTRALARGE / LARGE):
//   canvas   — the HICON size GetIcon returns,
//   art      — the tight opaque bounding box inside it,
//   verdict  — what the fence renderer would end up drawing.
// A canvas much larger than its art box is exactly the "tiny icon in the
// fence" case: the glyph gets padded into a big bitmap.
//
// Usage: icon_diag.exe <file> [more files...]

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commoncontrols.h>
#include <commctrl.h>
#include <stdio.h>
#include <vector>
#include <algorithm>
#include "src/icon_extract.h"

static void PutW(const wchar_t* s) {
    char buf[1024];
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
    if (n > 0) { buf[n - 1] = 0; fputs(buf, stdout); }
}

static const char* ListName(int L) {
    switch (L) {
    case SHIL_JUMBO:      return "JUMBO     ";
    case SHIL_EXTRALARGE: return "EXTRALARGE";
    case SHIL_LARGE:      return "LARGE     ";
    }
    return "?";
}

static void Inspect(const wchar_t* path) {
    printf("== "); PutW(path); printf("\n");

    SHFILEINFOW shfi = {};
    if (!SHGetFileInfoW(path, 0, &shfi, sizeof(shfi), SHGFI_SYSICONINDEX)) {
        printf("   SHGetFileInfoW failed\n\n");
        return;
    }
    printf("   iIcon=%d  type=\"", shfi.iIcon);
    PutW(shfi.szTypeName);
    printf("\"\n");

    static const int lists[3] = { SHIL_JUMBO, SHIL_EXTRALARGE, SHIL_LARGE };
    int bestPx = 0;
    const char* bestList = "(none)";

    for (int L : lists) {
        IImageList* il = nullptr;
        if (FAILED(SHGetImageList(L, IID_PPV_ARGS(&il))) || !il) {
            printf("   %s: SHGetImageList failed\n", ListName(L));
            continue;
        }
        HICON icon = nullptr;
        HRESULT hr = il->GetIcon(shfi.iIcon, ILD_TRANSPARENT, &icon);
        il->Release();
        if (FAILED(hr) || !icon) {
            printf("   %s: GetIcon failed (0x%08X)\n", ListName(L), (unsigned)hr);
            continue;
        }
        std::vector<BYTE> px;
        int w = 0, h = 0;
        bool ok = ExtractIconPixels(icon, px, w, h);
        DestroyIcon(icon);
        if (!ok) {
            printf("   %s: no color plane\n", ListName(L));
            continue;
        }
        int x0, y0, x1, y1;
        if (!TightBounds(px.data(), w, h, x0, y0, x1, y1)) {
            printf("   %s: canvas %dx%d fully transparent!\n", ListName(L), w, h);
            continue;
        }
        int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
        bool padded = (bw * 10 < w * 9 || bh * 10 < h * 9);
        int eff = (std::min)(bw, bh);
        printf("   %s: canvas %3dx%-3d art %3dx%-3d%s\n",
               ListName(L), w, h, bw, bh,
               padded ? "   <-- PADDED, renderer crops it" : "");
        if (!padded) eff = (std::min)(w, h);
        if (eff > bestPx) { bestPx = eff; bestList = ListName(L); }
    }
    printf("   -> drawn from %s at %d real px (scaled to the fence glyph box)\n\n",
           bestList, bestPx);
}

// ── desk mode: what does the real desktop actually draw? ──
//
// Reports the desktop's own icon size (its LVSIL_NORMAL list, if that handle
// survives the trip into this process; plus the shell's persisted IconSize),
// then walks the real desktop items and compares the PIDL-based icon index
// with the path-based one our app uses, running the per-list analysis on a
// few of them.

static HWND FindLV() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return nullptr;
    HWND dv = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    if (!dv) return nullptr;
    return FindWindowExW(dv, nullptr, L"SysListView32", nullptr);
}

static void DeskMode() {
    HWND lv = FindLV();
    if (!lv) { printf("desktop listview not found\n"); return; }

    HIMAGELIST himl = (HIMAGELIST)SendMessageW(lv, LVM_GETIMAGELIST, LVSIL_NORMAL, 0);
    printf("desktop LVSIL_NORMAL = %p\n", (void*)himl);
    int dcx = 0, dcy = 0;
    if (himl) {
        if (ImageList_GetIconSize(himl, &dcx, &dcy))
            printf("  GetIconSize -> %dx%d\n", dcx, dcy);
        else
            printf("  GetIconSize FAILED (cross-process handle unusable)\n");
        printf("  GetImageCount -> %d\n", ImageList_GetImageCount(himl));
    }

    DWORD regSz = 0, cb = sizeof regSz;
    LSTATUS st = RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\Shell\\Bags\\1\\Desktop", L"IconSize",
        RRF_RT_DWORD, nullptr, &regSz, &cb);
    printf("registry IconSize = %lu (status %ld)\n", regSz, (long)st);

    IShellFolder* desk = nullptr;
    if (FAILED(SHGetDesktopFolder(&desk)) || !desk) {
        printf("SHGetDesktopFolder failed\n");
        return;
    }
    IEnumIDList* en = nullptr;
    if (FAILED(desk->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &en)) || !en) {
        printf("EnumObjects failed\n");
        desk->Release();
        return;
    }
    int shown = 0, probeIdx = -1;
    ITEMIDLIST* child = nullptr;
    while (shown < 12 && en->Next(1, &child, nullptr) == S_OK) {
        wchar_t name[260] = L"?";
        STRRET ret;
        if (SUCCEEDED(desk->GetDisplayNameOf(child, SHGDN_NORMAL, &ret)))
            StrRetToBufW(&ret, child, name, 260);
        // The desktop is the namespace root, so child PIDLs are absolute.
        wchar_t path[MAX_PATH] = {};
        SHGetPathFromIDListW(child, path);
        SHFILEINFOW a = {};
        SHGetFileInfoW((LPCWSTR)child, 0, &a, sizeof(a),
                       SHGFI_PIDL | SHGFI_SYSICONINDEX);
        SHFILEINFOW b = {};
        BOOL okB = path[0] && SHGetFileInfoW(path, 0, &b, sizeof(b), SHGFI_SYSICONINDEX);
        printf("item \"%ls\"\n  pidl iIcon=%d  path iIcon=%d%s\n", name, a.iIcon,
               okB ? b.iIcon : -1,
               (okB && a.iIcon == b.iIcon) ? "  (match)" : "");
        if (okB && probeIdx < 0) probeIdx = b.iIcon;
        if (path[0] && shown < 5) Inspect(path);
        CoTaskMemFree(child);
        shown++;
    }
    en->Release();
    desk->Release();

    // Can we actually pull a glyph out of the desktop's list from this process?
    if (himl && probeIdx >= 0) {
        HICON hi = ImageList_GetIcon(himl, probeIdx, ILD_TRANSPARENT);
        if (hi) {
            std::vector<BYTE> px;
            int w = 0, h = 0;
            bool ok = ExtractIconPixels(hi, px, w, h);
            DestroyIcon(hi);
            if (ok) {
                int x0, y0, x1, y1;
                if (TightBounds(px.data(), w, h, x0, y0, x1, y1))
                    printf("desktop-list glyph (iIcon=%d): canvas %dx%d art %dx%d\n",
                           probeIdx, w, h, x1 - x0 + 1, y1 - y0 + 1);
                else
                    printf("desktop-list glyph (iIcon=%d): canvas %dx%d fully transparent\n",
                           probeIdx, w, h);
            } else {
                printf("desktop-list glyph: no color plane\n");
            }
        } else {
            printf("ImageList_GetIcon(himl,%d) FAILED err=%lu\n",
                   probeIdx, GetLastError());
        }
    }
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        printf("usage: icon_diag desk | <file> [...]\n");
        return 1;
    }
    // Measure what a DPI-aware app (openfences) sees, not virtualized sizes.
    SetProcessDPIAware();
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    if (!wcscmp(argv[1], L"desk")) DeskMode();
    else for (int i = 1; i < argc; i++) Inspect(argv[i]);
    CoUninitialize();
    return 0;
}
