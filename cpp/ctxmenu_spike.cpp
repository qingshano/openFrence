// Spike: enumerate the Windows 11 "modern" context menu (IExplorerCommand)
// for a single file, so we can re-render it ourselves in the fence.
// Usage: ctxmenu_spike.exe <path>
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <servprov.h>   // IServiceProvider
#include <stdio.h>

static const char* Ok(HRESULT hr) { return SUCCEEDED(hr) ? "OK" : "FAIL"; }

// The console is on the C locale, so printf("%ls") aborts at the first CJK
// character and silently swallows the rest of the line. Emit UTF-8 instead.
static void PutW(const wchar_t* s) {
    char buf[512];
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
    if (n > 0) { buf[n - 1] = 0; fputs(buf, stdout); }
}

static void PrintCommand(IExplorerCommand* cmd, IShellItemArray* sia, int depth) {
    EXPCMDFLAGS flags = ECF_DEFAULT;
    cmd->GetFlags(&flags);

    if (flags & ECF_ISSEPARATOR) {
        printf("%*s--- separator ---\n", depth * 2, "");
        return;
    }

    LPWSTR title = nullptr;
    cmd->GetTitle(sia, &title);
    LPWSTR icon = nullptr;
    cmd->GetIcon(sia, &icon);
    EXPCMDSTATE state = ECS_ENABLED;
    cmd->GetState(sia, FALSE, &state);

    printf("%*s[item] flags=0x%x state=0x%x %s\n", depth * 2, "",
           (unsigned)flags, (unsigned)state, icon ? "icon" : "no-icon");
    if (title) { printf("%*s   title=%ls\n", depth * 2, "", title); CoTaskMemFree(title); }
    if (icon)  { printf("%*s   icon =%ls\n", depth * 2, "", icon);  CoTaskMemFree(icon); }

    if (flags & ECF_HASSUBCOMMANDS) {
        IEnumExplorerCommand* subEnum = nullptr;
        if (SUCCEEDED(cmd->EnumSubCommands(&subEnum)) && subEnum) {
            IExplorerCommand* sub = nullptr;
            while (subEnum->Next(1, &sub, nullptr) == S_OK && sub) {
                PrintCommand(sub, sia, depth + 1);
                sub->Release(); sub = nullptr;
            }
            subEnum->Release();
        }
    }
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { printf("usage: ctxmenu_spike.exe <path>\n"); return 1; }
    const wchar_t* path = argv[1];
    printf("Target: %ls\n\n", path);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    PIDLIST_ABSOLUTE pidlFull = nullptr;
    HRESULT hr = SHParseDisplayName(path, nullptr, &pidlFull, 0, nullptr);
    printf("SHParseDisplayName: %s (0x%08x)\n", Ok(hr), (unsigned)hr);
    if (FAILED(hr) || !pidlFull) return 2;

    PCUITEMID_CHILD pidlChild = (PCUITEMID_CHILD)ILFindLastID(pidlFull);
    PIDLIST_ABSOLUTE pidlParent = ILCloneFull(pidlFull);
    ILRemoveLastID(pidlParent);

    IShellFolder* psfDesktop = nullptr;
    SHGetDesktopFolder(&psfDesktop);
    IShellFolder* psfParent = nullptr;
    if (ILIsEmpty(pidlParent)) { psfParent = psfDesktop; psfParent->AddRef(); }
    else psfDesktop->BindToObject(pidlParent, nullptr, IID_PPV_ARGS(&psfParent));
    printf("parent folder bound: %s\n", psfParent ? "OK" : "FAIL");

    IShellItemArray* sia = nullptr;
    hr = SHCreateShellItemArray(pidlParent, psfParent, 1, &pidlChild, &sia);
    printf("SHCreateShellItemArray: %s (0x%08x)\n\n", Ok(hr), (unsigned)hr);

    int shown = 0;

    // ── Approach A: provider via folder CreateViewObject ──
    {
        IExplorerCommandProvider* prov = nullptr;
        HRESULT h2 = psfParent->CreateViewObject(nullptr, IID_PPV_ARGS(&prov));
        printf("[A] CreateViewObject(IExplorerCommandProvider): %s (0x%08x)\n", Ok(h2), (unsigned)h2);
        if (SUCCEEDED(h2) && prov) {
            IEnumExplorerCommand* en = nullptr;
            HRESULT h3 = prov->GetCommands(sia, IID_PPV_ARGS(&en));
            printf("[A] GetCommands: %s (0x%08x)\n", Ok(h3), (unsigned)h3);
            if (SUCCEEDED(h3) && en) {
                IExplorerCommand* c = nullptr;
                while (en->Next(1, &c, nullptr) == S_OK && c) {
                    PrintCommand(c, sia, 0); shown++;
                    c->Release(); c = nullptr;
                }
                en->Release();
            }
            prov->Release();
        }
    }

    // ── Approach B: QI the shell item array for the provider ──
    if (sia && shown == 0) {
        IExplorerCommandProvider* prov = nullptr;
        HRESULT h2 = sia->QueryInterface(IID_PPV_ARGS(&prov));
        printf("\n[B] QI IShellItemArray -> provider: %s (0x%08x)\n", Ok(h2), (unsigned)h2);
        if (SUCCEEDED(h2) && prov) { prov->Release(); }
    }

    // ── Approach C: single IShellItem -> QI provider ──
    if (shown == 0) {
        IShellItem* si = nullptr;
        HRESULT h2 = SHCreateItemFromIDList(pidlFull, IID_PPV_ARGS(&si));
        printf("\n[C] SHCreateItemFromIDList: %s (0x%08x)\n", Ok(h2), (unsigned)h2);
        if (SUCCEEDED(h2) && si) {
            IExplorerCommandProvider* prov = nullptr;
            HRESULT h3 = si->QueryInterface(IID_PPV_ARGS(&prov));
            printf("[C] QI IShellItem -> provider: %s (0x%08x)\n", Ok(h3), (unsigned)h3);
            if (prov) prov->Release();
            si->Release();
        }
    }

    // ── Approach D: IServiceProvider on the array -> QueryService ──
    if (sia && shown == 0) {
        IServiceProvider* sp = nullptr;
        HRESULT h2 = sia->QueryInterface(IID_PPV_ARGS(&sp));
        printf("\n[D] QI IShellItemArray -> IServiceProvider: %s (0x%08x)\n", Ok(h2), (unsigned)h2);
        if (SUCCEEDED(h2) && sp) {
            IExplorerCommandProvider* prov = nullptr;
            HRESULT h3 = sp->QueryService(IID_IExplorerCommandProvider, IID_PPV_ARGS(&prov));
            printf("[D] QueryService(provider): %s (0x%08x)\n", Ok(h3), (unsigned)h3);
            if (prov) prov->Release();
            sp->Release();
        }
    }

    // ── Approach F: shell data object (drag/drop source) -> QI provider ──
    if (shown == 0) {
        IDataObject* pdo = nullptr;
        HRESULT h2 = SHCreateDataObject(pidlParent, 1, &pidlChild, nullptr,
                                        IID_PPV_ARGS(&pdo));
        printf("\n[F] SHCreateDataObject: %s (0x%08x)\n", Ok(h2), (unsigned)h2);
        if (SUCCEEDED(h2) && pdo) {
            IExplorerCommandProvider* prov = nullptr;
            HRESULT h3 = pdo->QueryInterface(IID_PPV_ARGS(&prov));
            printf("[F] QI IDataObject -> provider: %s (0x%08x)\n", Ok(h3), (unsigned)h3);
            if (SUCCEEDED(h3) && prov) {
                IEnumExplorerCommand* en = nullptr;
                HRESULT h4 = prov->GetCommands(sia, IID_PPV_ARGS(&en));
                printf("[F] GetCommands: %s (0x%08x)\n", Ok(h4), (unsigned)h4);
                if (SUCCEEDED(h4) && en) {
                    IExplorerCommand* c = nullptr;
                    while (en->Next(1, &c, nullptr) == S_OK && c) {
                        PrintCommand(c, sia, 0); shown++;
                        c->Release(); c = nullptr;
                    }
                    en->Release();
                }
                prov->Release();
            }
            pdo->Release();
        }
    }

    // ── Approach E: QI the IContextMenu (GetUIObjectOf) for the provider ──
    if (shown == 0) {
        IContextMenu* cm = nullptr;
        HRESULT h2 = psfParent->GetUIObjectOf(nullptr, 1,
            reinterpret_cast<PCUITEMID_CHILD_ARRAY>(&pidlChild),
            IID_IContextMenu, nullptr, (void**)&cm);
        printf("\n[E] GetUIObjectOf(IContextMenu): %s (0x%08x)\n", Ok(h2), (unsigned)h2);
        if (SUCCEEDED(h2) && cm) {
            IExplorerCommandProvider* prov = nullptr;
            HRESULT h3 = cm->QueryInterface(IID_PPV_ARGS(&prov));
            printf("[E] QI IContextMenu -> provider: %s (0x%08x)\n", Ok(h3), (unsigned)h3);
            // G: same QI, but after QueryContextMenu has populated a menu
            HMENU hmenu = CreatePopupMenu();
            HRESULT hq = cm->QueryContextMenu(hmenu, 0, 1, 0x7FFF, CMF_EXPLORE);
            printf("[G] QueryContextMenu: %s (0x%08x)\n", Ok(hq), (unsigned)hq);
            if (FAILED(h3)) {
                h3 = cm->QueryInterface(IID_PPV_ARGS(&prov));
                printf("[G] QI after QueryContextMenu -> provider: %s (0x%08x)\n", Ok(h3), (unsigned)h3);
            }
            DestroyMenu(hmenu);
            if (SUCCEEDED(h3) && prov) {
                IEnumExplorerCommand* en = nullptr;
                HRESULT h4 = prov->GetCommands(sia, IID_PPV_ARGS(&en));
                printf("[E] GetCommands: %s (0x%08x)\n", Ok(h4), (unsigned)h4);
                if (SUCCEEDED(h4) && en) {
                    IExplorerCommand* c = nullptr;
                    while (en->Next(1, &c, nullptr) == S_OK && c) {
                        PrintCommand(c, sia, 0); shown++;
                        c->Release(); c = nullptr;
                    }
                    en->Release();
                }
                prov->Release();
            }
            cm->Release();
        }
    }

    // ── Approach H: host a real DefView via the ExplorerBrowser control.
    //    Explorer's own menu host pulls commands off the view (CDefView),
    //    not the folder object. ──
    if (shown == 0) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"SpikeHost";
        RegisterClassExW(&wc);
        HWND host = CreateWindowExW(0, L"SpikeHost", L"", WS_POPUP,
                                    0, 0, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);

        IExplorerBrowser* peb = nullptr;
        HRESULT h2 = CoCreateInstance(CLSID_ExplorerBrowser, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&peb));
        printf("\n[H] CoCreateInstance(ExplorerBrowser): %s (0x%08x)\n", Ok(h2), (unsigned)h2);
        if (SUCCEEDED(h2) && peb) {
            FOLDERSETTINGS fs = { FVM_ICON, 0 };
            RECT rc = { 0, 0, 640, 480 };
            HRESULT h3 = peb->Initialize(host, &rc, &fs);
            printf("[H] Initialize: %s (0x%08x)\n", Ok(h3), (unsigned)h3);
            if (SUCCEEDED(h3)) {
                HRESULT h4 = peb->BrowseToIDList(pidlParent, SBSP_ABSOLUTE);
                printf("[H] BrowseToIDList: %s (0x%08x)\n", Ok(h4), (unsigned)h4);
                IShellView* sv = nullptr;
                HRESULT h5 = peb->GetCurrentView(IID_PPV_ARGS(&sv));
                printf("[H] GetCurrentView(IShellView): %s (0x%08x)\n", Ok(h5), (unsigned)h5);
                if (SUCCEEDED(h5) && sv) {
                    IExplorerCommandProvider* prov = nullptr;
                    HRESULT h6 = sv->QueryInterface(IID_PPV_ARGS(&prov));
                    printf("[H] QI IShellView -> provider: %s (0x%08x)\n", Ok(h6), (unsigned)h6);
                    if (SUCCEEDED(h6) && prov) {
                        IEnumExplorerCommand* en = nullptr;
                        HRESULT h7 = prov->GetCommands(sia, IID_PPV_ARGS(&en));
                        printf("[H] GetCommands: %s (0x%08x)\n", Ok(h7), (unsigned)h7);
                        if (SUCCEEDED(h7) && en) {
                            IExplorerCommand* c = nullptr;
                            while (en->Next(1, &c, nullptr) == S_OK && c) {
                                PrintCommand(c, sia, 0); shown++;
                                c->Release(); c = nullptr;
                            }
                            en->Release();
                        }
                        prov->Release();
                    }
                    sv->Release();
                }
            }
            peb->Destroy();
            peb->Release();
        }
        DestroyWindow(host);
    }

    // ── Owner-draw capture test: can we make the shell paint its built-in
    //    (MFT_OWNERDRAW) items onto our own HDC? Make-or-break for the
    //    Fluent re-skin. ──
    {
        printf("\n=== Owner-draw capture test ===\n");
        // Opt into dark mode so the shell paints light-on-dark glyphs
        // (uxtheme!SetPreferredAppMode, ordinal 135, Win10 1809+).
        HMODULE ux = LoadLibraryW(L"uxtheme.dll");
        typedef int (WINAPI* SetPrefModeT)(int);
        auto setPrefMode = (SetPrefModeT)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(135));
        printf("SetPreferredAppMode: %s\n", setPrefMode ? "found" : "missing");
        if (setPrefMode) setPrefMode(2 /* ForceDark */);

        IContextMenu* cm = nullptr;
        HRESULT h2 = psfParent->GetUIObjectOf(nullptr, 1,
            reinterpret_cast<PCUITEMID_CHILD_ARRAY>(&pidlChild),
            IID_IContextMenu, nullptr, (void**)&cm);
        if (SUCCEEDED(h2) && cm) {
            HMENU hmenu = CreatePopupMenu();
            cm->QueryContextMenu(hmenu, 0, 1, 0x7FFF, CMF_EXPLORE | CMF_CANRENAME);
            IContextMenu3* cm3 = nullptr;
            IContextMenu2* cm2 = nullptr;
            cm->QueryInterface(IID_PPV_ARGS(&cm3));
            cm->QueryInterface(IID_PPV_ARGS(&cm2));
            LRESULT lr = 0;
            if (cm3)      cm3->HandleMenuMsg2(WM_INITMENUPOPUP, (WPARAM)hmenu, 0, &lr);
            else if (cm2) cm2->HandleMenuMsg(WM_INITMENUPOPUP, (WPARAM)hmenu, 0);

            int n = GetMenuItemCount(hmenu);
            for (int i = 0; i < n && i < 4; i++) {
                MENUITEMINFOW mii = { sizeof(mii) };
                mii.fMask = MIIM_ID | MIIM_FTYPE | MIIM_DATA | MIIM_STRING;
                wchar_t text[128] = {};
                mii.dwTypeData = text; mii.cch = 128;
                GetMenuItemInfoW(hmenu, i, TRUE, &mii);
                printf("[%d] id=%u fType=0x%x data=%p text='", i, mii.wID,
                       mii.fType, (void*)mii.dwItemData);
                PutW(text);
                printf("' ownerdraw=%s\n",
                       (mii.fType & MFT_OWNERDRAW) ? "YES" : "no");
                if (!(mii.fType & MFT_OWNERDRAW)) continue;

                // 1) ask the shell how big the item is
                MEASUREITEMSTRUCT mis = {};
                mis.CtlType = ODT_MENU;
                mis.itemID = mii.wID;
                mis.itemData = mii.dwItemData;
                mis.itemWidth = 300; mis.itemHeight = 32;
                if (cm3)      cm3->HandleMenuMsg2(WM_MEASUREITEM, 0, (LPARAM)&mis, &lr);
                else if (cm2) cm2->HandleMenuMsg(WM_MEASUREITEM, 0, (LPARAM)&mis);
                printf("    measured: %ux%u\n", mis.itemWidth, mis.itemHeight);
                if (!mis.itemWidth || !mis.itemHeight) continue;

                // 2) paint it onto a 32bpp DIB over a dark plate
                HDC hdc = GetDC(nullptr);
                BITMAPINFO bi = {};
                bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                bi.bmiHeader.biWidth = (int)mis.itemWidth;
                bi.bmiHeader.biHeight = -(int)mis.itemHeight;   // top-down
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                void* bits = nullptr;
                HBITMAP dib = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
                HDC mem = CreateCompatibleDC(hdc);
                HBITMAP old = (HBITMAP)SelectObject(mem, dib);
                RECT rcFill = { 0, 0, (LONG)mis.itemWidth, (LONG)mis.itemHeight };
                HBRUSH br = CreateSolidBrush(RGB(32, 32, 32));
                FillRect(mem, &rcFill, br);
                DeleteObject(br);

                DRAWITEMSTRUCT dis = {};
                dis.CtlType = ODT_MENU;
                dis.itemID = mii.wID;
                dis.itemData = mii.dwItemData;
                dis.hwndItem = (HWND)hmenu;
                dis.hDC = mem;
                dis.rcItem = rcFill;
                dis.itemState = 0;
                if (cm3)      cm3->HandleMenuMsg2(WM_DRAWITEM, 0, (LPARAM)&dis, &lr);
                else if (cm2) cm2->HandleMenuMsg(WM_DRAWITEM, 0, (LPARAM)&dis);

                // 3) save it for inspection
                wchar_t fn[64];
                swprintf_s(fn, L"cmitem_%u.bmp", mii.wID);
                // minimal BMP file: header + bottom-up pixels
                BITMAPINFO biUp = bi;
                biUp.bmiHeader.biHeight = (int)mis.itemHeight;   // bottom-up for file
                FILE* f = _wfopen(fn, L"wb");
                if (f) {
                    DWORD imgSz = mis.itemWidth * mis.itemHeight * 4;
                    BITMAPFILEHEADER fh = {};
                    fh.bfType = 0x4D42;
                    fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
                    fh.bfSize = fh.bfOffBits + imgSz;
                    fwrite(&fh, sizeof(fh), 1, f);
                    fwrite(&biUp.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
                    // rows are top-down in 'bits'; write reversed
                    for (LONG row = (LONG)mis.itemHeight - 1; row >= 0; row--)
                        fwrite((BYTE*)bits + row * mis.itemWidth * 4, mis.itemWidth * 4, 1, f);
                    fclose(f);
                    printf("    saved %ls\n", fn);
                }
                SelectObject(mem, old);
                DeleteDC(mem);
                DeleteObject(dib);
                ReleaseDC(nullptr, hdc);
            }
            if (cm3) cm3->Release();
            if (cm2) cm2->Release();
            DestroyMenu(hmenu);
            cm->Release();
        }
        if (ux) FreeLibrary(ux);
    }

    // ── Dump the classic IContextMenu HMENU: this is the data source the
    //    Fluent re-skin will feed on. Do items carry bitmaps? What verbs? ──
    {
        printf("\n=== Classic IContextMenu dump ===\n");
        IContextMenu* cm = nullptr;
        HRESULT h2 = psfParent->GetUIObjectOf(nullptr, 1,
            reinterpret_cast<PCUITEMID_CHILD_ARRAY>(&pidlChild),
            IID_IContextMenu, nullptr, (void**)&cm);
        if (SUCCEEDED(h2) && cm) {
            HMENU hmenu = CreatePopupMenu();
            HRESULT hq = cm->QueryContextMenu(hmenu, 0, 1, 0x7FFF,
                                              CMF_EXPLORE | CMF_CANRENAME);
            printf("QueryContextMenu: %s (0x%08x)\n", Ok(hq), (unsigned)hq);

            // Handlers populate text/icons lazily on WM_INITMENUPOPUP.
            IContextMenu3* cm3 = nullptr;
            IContextMenu2* cm2 = nullptr;
            cm->QueryInterface(IID_PPV_ARGS(&cm3));
            cm->QueryInterface(IID_PPV_ARGS(&cm2));
            printf("QI IContextMenu2=%s IContextMenu3=%s\n",
                   cm2 ? "OK" : "no", cm3 ? "OK" : "no");
            struct Initer {
                static void Init(HMENU m, IContextMenu3* cm3, IContextMenu2* cm2) {
                    LRESULT lr = 0;
                    if (cm3)      cm3->HandleMenuMsg2(WM_INITMENUPOPUP, (WPARAM)m, 0, &lr);
                    else if (cm2) cm2->HandleMenuMsg(WM_INITMENUPOPUP, (WPARAM)m, 0);
                    int n = GetMenuItemCount(m);
                    for (int i = 0; i < n; i++) {
                        MENUITEMINFOW mii = { sizeof(mii) };
                        mii.fMask = MIIM_SUBMENU;
                        GetMenuItemInfoW(m, i, TRUE, &mii);
                        if (mii.hSubMenu) Init(mii.hSubMenu, cm3, cm2);
                    }
                }
            };
            Initer::Init(hmenu, cm3, cm2);
            if (cm3) cm3->Release();
            if (cm2) cm2->Release();
            struct Dumper {
                static void Dump(HMENU m, IContextMenu* cm, int depth) {
                    int n = GetMenuItemCount(m);
                    for (int i = 0; i < n; i++) {
                        MENUITEMINFOW mii = { sizeof(mii) };
                        mii.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STATE |
                                    MIIM_BITMAP | MIIM_SUBMENU | MIIM_STRING;
                        wchar_t text[256] = {};
                        mii.dwTypeData = text; mii.cch = 256;
                        GetMenuItemInfoW(m, i, TRUE, &mii);
                        if (mii.fType & MFT_SEPARATOR) {
                            printf("%*s--- separator ---\n", depth * 2, "");
                            continue;
                        }
                        char verb[64] = {};
                        cm->GetCommandString(mii.wID - 1, GCS_VERBA, nullptr,
                                             verb, sizeof(verb));
                        printf("%*sid=%-4u bmp=%p state=0x%x verb=%-14s '",
                               depth * 2, "", mii.wID, mii.hbmpItem,
                               mii.fState, verb);
                        PutW(text);
                        printf("'\n");
                        if (mii.hbmpItem && mii.hbmpItem != HBMMENU_CALLBACK) {
                            BITMAP bm = {};
                            int ok = GetObjectW(mii.hbmpItem, sizeof(bm), &bm);
                            printf("%*s    bmp: valid=%d %ldx%ld bpp=%d\n",
                                   depth * 2, "", ok,
                                   ok ? bm.bmWidth : 0, ok ? bm.bmHeight : 0,
                                   ok ? bm.bmBitsPixel : 0);
                        }
                        if (mii.hSubMenu) Dump(mii.hSubMenu, cm, depth + 1);
                    }
                }
            };
            Dumper::Dump(hmenu, cm, 1);
            DestroyMenu(hmenu);
            cm->Release();
        }
    }

    printf("\nTotal printed: %d\n", shown);

    if (sia) sia->Release();
    if (psfParent) psfParent->Release();
    if (psfDesktop) psfDesktop->Release();
    CoTaskMemFree(pidlParent);
    CoTaskMemFree(pidlFull);
    CoUninitialize();
    return 0;
}
