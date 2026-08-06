// vis_probe.cpp — spike: what exactly does explorer hide when "Show desktop
// icons" is turned off? Prints the desktop window chain with visibility
// flags; with the "toggle" argument it flips SSF_HIDEICONS programmatically
// (restoring the original state afterwards) so both sides can be observed.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>   // SHELLSTATE / SHGetSetSettings live here, not shellapi.h
#include <cstdio>
#include <cstring>
#include <cmath>

static void DumpOne(const char* tag, HWND h) {
    if (!h) { printf("%-12s (not found)\n", tag); return; }
    RECT r{}; GetWindowRect(h, &r);
    printf("%-12s %p  visible=%d  style=%08lX ex=%08lX  rect=%ld,%ld %ldx%ld\n",
        tag, (void*)h, IsWindowVisible(h) ? 1 : 0,
        (unsigned long)GetWindowLongPtrW(h, GWL_STYLE),
        (unsigned long)GetWindowLongPtrW(h, GWL_EXSTYLE),
        r.left, r.top, r.right - r.left, r.bottom - r.top);
}

static void DumpTree() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    DumpOne("Progman", progman);
    // DefView normally sits directly under Progman; after some shell
    // transitions it can move under a WorkerW sibling — check both.
    HWND host = progman;
    HWND defview = progman
        ? FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr)
        : nullptr;
    if (!defview) {
        HWND w = nullptr;
        while ((w = FindWindowExW(nullptr, w, L"WorkerW", nullptr)) != nullptr) {
            defview = FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr);
            if (defview) { host = w; break; }
        }
    }
    if (host && host != progman) DumpOne("WorkerW", host);
    DumpOne("DefView", defview);
    HWND lv = defview
        ? FindWindowExW(defview, nullptr, L"SysListView32", nullptr)
        : nullptr;
    DumpOne("ListView", lv);
}

// Write HKCU\...\Explorer\Advanced\HideIcons and nudge explorer the same way
// the View menu does (registry + WM_SETTINGCHANGE broadcast + assoc notify).
static void SetHideIconsReg(DWORD v) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
            0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    RegSetValueExW(k, L"HideIcons", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(k);
    DWORD_PTR res;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ShellState",
                        SMTO_NORMAL, 2000, &res);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

// Count "OpenFencesFence" children directly under `parent`.
static int CountFencesUnder(HWND parent) {
    int n = 0;
    HWND c = nullptr;
    while (parent && (c = FindWindowExW(parent, c, L"OpenFencesFence", nullptr)) != nullptr)
        n++;
    return n;
}

// Simulate what explorer does when "Show desktop icons" is unchecked: hide
// SysListView32 from an OUTSIDE process. The running openfences watcher must
// park its fences on SHELLDLL_DefView within one 300ms poll, and move them
// back when the list is shown again.
static void WatcherTest() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    HWND defview = progman ? FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr) : nullptr;
    HWND lv = defview ? FindWindowExW(defview, nullptr, L"SysListView32", nullptr) : nullptr;
    if (!lv) { printf("no desktop listview found\n"); return; }
    int before = CountFencesUnder(lv);
    printf("fences under LV before: %d\n", before);
    if (before == 0) { printf("no openfences running with fences; aborting\n"); return; }

    ShowWindow(lv, SW_HIDE);   // what explorer does
    for (int i = 0; i < 8; i++) {
        Sleep(300);
        printf("t+%4dms  LVvisible=%d  fences(LV)=%d  fences(DefView)=%d\n",
               (i + 1) * 300, IsWindowVisible(lv),
               CountFencesUnder(lv), CountFencesUnder(defview));
    }

    ShowWindow(lv, SW_SHOW);   // what explorer does on re-check
    for (int i = 0; i < 8; i++) {
        Sleep(300);
        printf("t+%4dms  LVvisible=%d  fences(LV)=%d  fences(DefView)=%d\n",
               (i + 1) * 300, IsWindowVisible(lv),
               CountFencesUnder(lv), CountFencesUnder(defview));
    }
}

static DWORD ReadHideIconsReg() {
    DWORD v = 0xDEAD, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
        L"HideIcons", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v;
}

// Replicate MenuPopup::Create's placement math with the LIVE taskbar/tray
// geometry, to find out why the tray menu lands below/behind the tray icon.
static void TrayCalc() {
    // Match the app: Per-Monitor V2 DPI awareness (physical pixels).
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    typedef BOOL (WINAPI* SetCtxT)(HANDLE);
    auto setCtx = u32 ? (SetCtxT)(void*)GetProcAddress(u32, "SetProcessDpiAwarenessContext") : nullptr;
    if (setCtx) setCtx((HANDLE)-4);

    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    RECT tb{}; if (taskbar) GetWindowRect(taskbar, &tb);
    HWND tray = taskbar ? FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    RECT tr{}; if (tray) GetWindowRect(tray, &tr);
    RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    POINT cur{}; GetCursorPos(&cur);
    HDC h = GetDC(nullptr);
    float s = GetDeviceCaps(h, LOGPIXELSX) / 96.0f;
    ReleaseDC(nullptr, h);

    printf("dpiScale=%.2f\n", s);
    printf("taskbar  %ld,%ld %ldx%ld visible=%d\n", tb.left, tb.top, tb.right - tb.left, tb.bottom - tb.top, taskbar ? IsWindowVisible(taskbar) : -1);
    printf("trayArea %ld,%ld %ldx%ld\n", tr.left, tr.top, tr.right - tr.left, tr.bottom - tr.top);
    printf("workArea %ld,%ld %ldx%ld\n", work.left, work.top, work.right - work.left, work.bottom - work.top);
    printf("cursor   %ld,%ld\n", cur.x, cur.y);

    // The app opens the menu at the cursor position over the tray icon.
    POINT at = cur;
    if (tray) { at.x = (tr.left + tr.right) / 2; at.y = (tr.top + tr.bottom) / 2; }

    HMONITOR mon = MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    printf("monitor  rc=%ld,%ld-%ld,%ld  rcWork=%ld,%ld-%ld,%ld\n",
           mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom,
           mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom);

    // Menu size as measured for the current tray menu: 7 items + 2 separators,
    // Metrics rowH=30 sepH=9 padV=5 → contentH = (5+7*30+2*9+5)*s
    int contentH = (int)std::ceil((10.0f + 7 * 30.0f + 2 * 9.0f) * s);
    int w = 220;   // approx; width does not matter for the vertical question
    int maxH = (mi.rcWork.bottom - mi.rcWork.top) - (int)(16 * s);
    int hh = contentH < maxH ? contentH : maxH;
    int x = at.x, y = at.y;
    printf("at=%d,%d  contentH=%d maxH=%d h=%d\n", at.x, at.y, contentH, maxH, hh);
    bool flip = (y + hh > mi.rcWork.bottom);
    if (flip) y = at.y - hh;
    printf("flip=%d -> y=%d\n", flip ? 1 : 0, y);
    if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
    int ylo = mi.rcWork.top;
    int yhi = mi.rcWork.top > mi.rcWork.bottom - hh ? mi.rcWork.top : mi.rcWork.bottom - hh;
    int y2 = y < ylo ? ylo : (y > yhi ? yhi : y);
    printf("clamp yhi=%d -> final rect %d,%d %dx%d (bottom=%d, taskbarTop=%ld)\n",
           yhi, x, y2, w, hh, y2 + hh, tb.top);
}

// Open the running app's tray menu remotely (its owner window handles
// WM_APP+1 like a tray right-click), then inspect the live menu window:
// its real rect vs the taskbar, and whether an outside click dismisses it.
static void CloseAllMenus();   // fwd

static void DumpMenuWnds(const char* tag) {
    HWND m = nullptr;
    int n = 0;
    while ((m = FindWindowExW(nullptr, m, L"OpenFencesFluentMenu", nullptr)) != nullptr) {
        RECT r{}; GetWindowRect(m, &r);
        printf("%s menu[%d] %p rect=%ld,%ld-%ld,%ld visible=%d ex=%08lX\n",
               tag, n++, (void*)m, r.left, r.top, r.right, r.bottom,
               IsWindowVisible(m), (unsigned long)GetWindowLongPtrW(m, GWL_EXSTYLE));
    }
    if (!n) printf("%s (no menu windows)\n", tag);
}

static void SendClick(int x, int y) {
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_MOUSE;
    in[0].mi.dx = x * 65536 / GetSystemMetrics(SM_CXSCREEN);
    in[0].mi.dy = y * 65536 / GetSystemMetrics(SM_CYSCREEN);
    in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_LEFTDOWN;
    in[1].mi = in[0].mi;
    in[1].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_LEFTUP;
    UINT sent = SendInput(2, in, sizeof(INPUT));
    printf("SendInput(%d,%d): %u/2 err=%lu\n", x, y, sent, GetLastError());
    Sleep(80);
    POINT p{(LONG)x, (LONG)y};
    HWND hit = WindowFromPoint(p);
    wchar_t cls[64] = {};
    GetClassNameW(hit, cls, 64);
    printf("WindowFromPoint(%d,%d) = %p class=%ls\n", x, y, (void*)hit, cls);
}

static void DumpFore(const char* tag) {
    HWND fg = GetForegroundWindow();
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    printf("%s foreground=%p class=%ls\n", tag, (void*)fg, cls);
}

static void SendEsc() {
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = VK_ESCAPE;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

// Find a point whose hit is the desktop icon list (empty desktop area),
// avoiding the user's fences. Virtualized 96-DPI space.
static bool FindDesktopPoint(POINT& out) {
    for (int y = 620; y <= 980; y += 60) {
        for (int x = 1100; x <= 1680; x += 60) {
            wchar_t cls[64] = {};
            GetClassNameW(WindowFromPoint(POINT{(LONG)x, (LONG)y}), cls, 64);
            if (wcscmp(cls, L"SysListView32") == 0) { out = {x, y}; return true; }
        }
    }
    return false;
}

static void OpenTrayMenu(HWND owner, const RECT& tr) {
    SetForegroundWindow(GetConsoleWindow());
    AllowSetForegroundWindow(ASFW_ANY);
    SetCursorPos((tr.left + tr.right) / 2, (tr.top + tr.bottom) / 2);
    Sleep(150);
    LRESULT lr = 0;
    SendMessageTimeoutW(owner, 0x8001, 0, MAKELPARAM(WM_RBUTTONUP, 0),
                        SMTO_NORMAL, 2500, (DWORD_PTR*)&lr);
}

static void MenuTest() {
    HWND owner = FindWindowW(L"OpenFencesOwner", nullptr);
    if (!owner) { printf("openfences owner window not found (is it running?)\n"); return; }
    if (FindWindowW(L"OpenFencesFluentMenu", nullptr)) {
        printf("menus already open — run closeall first\n"); return;
    }
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    HWND tray = taskbar ? FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    RECT tr{}; if (tray) GetWindowRect(tray, &tr);
    POINT oldCur{}; GetCursorPos(&oldCur);
    POINT dpt{};
    bool hasDpt = FindDesktopPoint(dpt);
    printf("desktop click point: %ld,%ld (found=%d)\n", dpt.x, dpt.y, hasDpt ? 1 : 0);

    // ── Round A: click empty desktop while the menu is open ──
    OpenTrayMenu(owner, tr);
    DumpMenuWnds("A open:");
    DumpFore("A open:");
    Sleep(300);
    if (hasDpt) SendClick(dpt.x, dpt.y);
    Sleep(700);
    DumpMenuWnds("A after desktop click:");
    DumpFore("A after desktop click:");
    if (FindWindowW(L"OpenFencesFluentMenu", nullptr)) {
        printf("=> A: desktop click did NOT dismiss\n");
        CloseAllMenus();
    } else {
        printf("=> A: desktop click dismissed the menu\n");
    }

    // ── Round B: click another app's window (takes activation) ──
    OpenTrayMenu(owner, tr);
    DumpMenuWnds("B open:");
    DumpFore("B open:");
    Sleep(300);
    SendClick(1267, 800);   // the user's terminal (see earlier WindowFromPoint)
    Sleep(700);
    DumpMenuWnds("B after app click:");
    DumpFore("B after app click:");
    if (FindWindowW(L"OpenFencesFluentMenu", nullptr)) {
        printf("=> B: app click did NOT dismiss; trying Esc\n");
        SendEsc();
        Sleep(500);
        DumpMenuWnds("B after Esc:");
        if (FindWindowW(L"OpenFencesFluentMenu", nullptr)) CloseAllMenus();
    } else {
        printf("=> B: app click dismissed the menu\n");
    }

    SetCursorPos(oldCur.x, oldCur.y);
}

// Close every stuck FluentMenu: post a press into each window's shadow
// margin (the margin branch cancels the session without selecting).
static void CloseAllMenus() {
    for (int round = 0; round < 10; round++) {
        HWND m = FindWindowW(L"OpenFencesFluentMenu", nullptr);
        if (!m) { printf("all menus closed\n"); return; }
        while (m) {
            PostMessageW(m, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(2, 2));
            PostMessageW(m, WM_LBUTTONUP, 0, MAKELPARAM(2, 2));
            m = FindWindowExW(nullptr, m, L"OpenFencesFluentMenu", nullptr);
        }
        Sleep(400);
    }
    DumpMenuWnds("closeall gave up:");
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "closeall") == 0) {
        CloseAllMenus();
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "menutest") == 0) {
        MenuTest();
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "traycalc") == 0) {
        TrayCalc();
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "watchertest") == 0) {
        WatcherTest();
        return 0;
    }
    SHELLSTATEW ss{};
    SHGetSetSettings(&ss, SSF_HIDEICONS, FALSE);
    printf("SSF_HIDEICONS currently: %u   reg HideIcons=%u\n",
           ss.fHideIcons ? 1u : 0u, (unsigned)ReadHideIconsReg());
    printf("== desktop tree ==\n");
    DumpTree();

    if (argc > 1 && strcmp(argv[1], "toggle") == 0) {
        SetHideIconsReg(1);
        printf("reg right after set: %u\n", (unsigned)ReadHideIconsReg());
        for (int i = 0; i < 6; i++) {
            Sleep(500);
            HWND lv = nullptr;
            HWND progman = FindWindowW(L"Progman", nullptr);
            HWND dv = progman ? FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr) : nullptr;
            if (dv) lv = FindWindowExW(dv, nullptr, L"SysListView32", nullptr);
            printf("t+%dms  LV visible=%d\n", (i + 1) * 500,
                   lv ? IsWindowVisible(lv) : -1);
            if (lv && !IsWindowVisible(lv)) break;
        }
        printf("== after HideIcons=1 ==\n");
        DumpTree();

        // Restore the user's original setting.
        SetHideIconsReg(0);
        Sleep(1000);
        printf("== restored HideIcons=0 (reg=%u) ==\n", (unsigned)ReadHideIconsReg());
        DumpTree();
    }
    return 0;
}
