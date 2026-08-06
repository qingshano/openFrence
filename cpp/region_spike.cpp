// region_spike — does blanking SysListView32's window region hide the desktop
// icons while keeping the fence children visible AND clickable?
//
// Hiding the listview (SW_HIDE) also hides the fences, because they are its
// WS_CHILD children. The idea under test: leave the window visible and set an
// EMPTY window region on it instead. The region clips the listview's own icon
// painting, but do child windows survive it — rendering and hit-testing?
//
// The real desktop cannot be probed from this session (the terminal window
// covers every desktop point, so WindowFromPoint always hits the terminal).
// Instead the `replica` mode builds a topmost popup that reproduces the exact
// window relationship — parent C (stand-in for SysListView32) containing a
// plain child F1 and a WS_EX_LAYERED child F2 (stand-in for a fence) — and
// probes WindowFromPoint at each child's center in three phases:
//   phase 1: C has no region            (baseline)
//   phase 2: C has an EMPTY region      (the proposed fix state)
//   phase 3: region restored to NULL
// If F1/F2 stay hittable in phase 2, the fix works. The popup stays open ~15s
// so the visual side (do the children still draw?) can be eyeballed too.
//
// Modes:
//   region_spike replica — run the self-contained experiment
//   region_spike hide    — set the empty region on the real SysListView32
//   region_spike show    — restore it (NULL region)
//   region_spike test    — WindowFromPoint at fence centers (only meaningful
//                          when no other window covers them)
//   region_spike desksize — dump live desktop icon geometry
//   region_spike zoom [n] — inject n Ctrl+wheel notches in/out and dump the
//                           geometry each step (calibrates rect -> glyph size)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <wchar.h>
#include <initializer_list>

static HWND FindDesktopLV() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return nullptr;
    HWND defview = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    if (!defview) return nullptr;
    return FindWindowExW(defview, nullptr, L"SysListView32", nullptr);
}

// ── live-desktop probe modes (hide / show / test) ──────────────────────────

static BOOL CALLBACK EnumTopCover(HWND hwnd, LPARAM lp) {
    POINT p = *(POINT*)lp;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW")) return TRUE;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) return TRUE;
    return !PtInRect(&rc, p);   // stop (FALSE) when a covering window is found
}

static bool Covered(POINT p) { return !EnumWindows(EnumTopCover, (LPARAM)&p); }

// Measure the desktop's LIVE icon geometry: registry IconSize, item count,
// both item spacings, and LVIR_ICON/LVIR_BOUNDS rects of the first items,
// read out of explorer's address space. Ground truth for the size the
// desktop currently draws its icons at — the registry value is only
// persisted lazily and lags Ctrl+wheel zoom.
static void DumpGeom(const char* tag) {
    HWND lv = FindDesktopLV();
    if (!lv) { printf("%s: no lv\n", tag); return; }

    DWORD logical = 0, cb = sizeof(logical);
    RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\Shell\\Bags\\1\\Desktop", L"IconSize",
        RRF_RT_DWORD, nullptr, &logical, &cb);

    DWORD_PTR count = 0;
    SendMessageTimeoutW(lv, LVM_GETITEMCOUNT, 0, 0, SMTO_ABORTIFHUNG, 500, &count);

    DWORD_PTR spL = 0, spS = 0;
    SendMessageTimeoutW(lv, LVM_GETITEMSPACING, FALSE, 0, SMTO_ABORTIFHUNG, 500, &spL);
    SendMessageTimeoutW(lv, LVM_GETITEMSPACING, TRUE, 0, SMTO_ABORTIFHUNG, 500, &spS);

    printf("%s reg=%lu count=%llu spacingL cx=%d cy=%d spacingS cx=%d cy=%d\n",
           tag, logical, (unsigned long long)count,
           (int)(short)LOWORD((DWORD)spL), (int)(short)HIWORD((DWORD)spL),
           (int)(short)LOWORD((DWORD)spS), (int)(short)HIWORD((DWORD)spS));

    if (count == 0) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(lv, &pid);
    HANDLE proc = OpenProcess(
        PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);
    if (!proc) { printf("%s: OpenProcess err=%lu\n", tag, GetLastError()); return; }

    void* mem = VirtualAllocEx(proc, nullptr, sizeof(RECT), MEM_COMMIT, PAGE_READWRITE);
    if (!mem) {
        printf("%s: VirtualAllocEx err=%lu\n", tag, GetLastError());
        CloseHandle(proc);
        return;
    }
    for (int i = 0; i < 6 && i < (int)count; i++) {
        for (int kind : { LVIR_ICON, LVIR_BOUNDS }) {
            RECT in = {};
            in.left = kind;   // LVM_GETITEMRECT reads the kind from rect.left
            WriteProcessMemory(proc, mem, &in, sizeof(RECT), nullptr);
            DWORD_PTR res = 0;
            SendMessageTimeoutW(lv, LVM_GETITEMRECT, i, (LPARAM)mem,
                                SMTO_ABORTIFHUNG, 500, &res);
            RECT back = {};
            ReadProcessMemory(proc, mem, &back, sizeof(RECT), nullptr);
            printf("%s item %d %s res=%llu [%ld,%ld %ldx%ld]\n", tag, i,
                   kind == LVIR_ICON ? "ICON  " : "BOUNDS",
                   (unsigned long long)res, back.left, back.top,
                   back.right - back.left, back.bottom - back.top);
        }
    }
    VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
    CloseHandle(proc);
}

// Press the app's global hotkey (Ctrl+Shift+H) that toggles icon hiding.
static void SendToggleHotkey() {
    INPUT in[6] = {};
    in[0].type = in[1].type = in[2].type = INPUT_KEYBOARD;
    in[3].type = in[4].type = in[5].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_CONTROL;
    in[1].ki.wVk = VK_SHIFT;
    in[2].ki.wVk = 'H';
    in[3].ki.wVk = 'H';            in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    in[4].ki.wVk = VK_SHIFT;       in[4].ki.dwFlags = KEYEVENTF_KEYUP;
    in[5].ki.wVk = VK_CONTROL;     in[5].ki.dwFlags = KEYEVENTF_KEYUP;
    UINT sent = SendInput(6, in, sizeof(INPUT));
    printf("SendInput -> %u/6\n", sent);
}

static void ProbeLive(HWND lv) {
    struct Ctx { HWND lv; int found = 0, ok = 0; } ctx{lv};
    EnumChildWindows(lv, [](HWND h, LPARAM lp) -> BOOL {
        auto* c = (Ctx*)lp;
        wchar_t cls[64] = {};
        GetClassNameW(h, cls, 64);
        if (wcscmp(cls, L"OpenFencesFence") || GetParent(h) != c->lv) return TRUE;
        c->found++;
        RECT rc; GetWindowRect(h, &rc);
        POINT p{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
        if (Covered(p)) { printf("  fence %p: center covered by another window\n", (void*)h); return TRUE; }
        HWND at = WindowFromPoint(p);
        bool ok = at == h;
        if (ok) c->ok++;
        printf("  fence %p center -> %p %s\n", (void*)h, (void*)at, ok ? "OK" : "MISMATCH");
        return TRUE;
    }, (LPARAM)&ctx);
    printf("%d fences, %d hittable\n", ctx.found, ctx.ok);
}

// ── replica experiment ─────────────────────────────────────────────────────

static LRESULT CALLBACK PlainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TIMER) { DestroyWindow(h); return 0; }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

static void Reg(const wchar_t* name, HBRUSH br) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = PlainProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = br;
    wc.lpszClassName = name;
    RegisterClassW(&wc);
}

// Give a WS_EX_LAYERED window an opaque solid surface via UpdateLayeredWindow.
static void PaintLayeredSolid(HWND h, int w, int hh, BYTE r, BYTE g, BYTE b) {
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -hh;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    BYTE* p = (BYTE*)bits;
    for (int i = 0; i < w * hh; i++) {
        p[4 * i + 0] = b; p[4 * i + 1] = g; p[4 * i + 2] = r; p[4 * i + 3] = 255;
    }
    SelectObject(mem, dib);
    SIZE sz{w, hh};
    POINT srcTop{0, 0};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(h, screen, nullptr, &sz, mem, &srcTop, 0, &bf, ULW_ALPHA);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

static void Probe(HWND expected, const char* who) {
    RECT rc;
    GetWindowRect(expected, &rc);
    POINT p{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
    HWND at = WindowFromPoint(p);
    wchar_t cls[64] = {};
    if (at) GetClassNameW(at, cls, 64);
    printf("  %-22s -> %p \"%ls\" %s\n", who, (void*)at, cls,
           at == expected ? "OK" : "MISMATCH");
}

// Does any visible top-level window (other than the desktop) intersect r?
static bool RectCovered(const RECT& r) {
    struct Ctx { RECT r; bool covered = false; } ctx{r};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = (Ctx*)lp;
        wchar_t cls[64] = {};
        GetClassNameW(h, cls, 64);
        if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW")) return TRUE;
        if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
        RECT w;
        if (!GetWindowRect(h, &w)) return TRUE;
        RECT inter;
        if (IntersectRect(&inter, &c->r, &w)) { c->covered = true; return FALSE; }
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.covered;
}

// Inject Ctrl+wheel at the current cursor position — the desktop zooms its
// icons when the wheel lands on SysListView32. delta notches; >0 zooms in.
static void CtrlWheel(int notches) {
    INPUT kd = {}; kd.type = INPUT_KEYBOARD; kd.ki.wVk = VK_CONTROL;
    SendInput(1, &kd, sizeof(INPUT));
    int n = notches < 0 ? -notches : notches;
    for (int i = 0; i < n; i++) {
        INPUT wh = {}; wh.type = INPUT_MOUSE;
        wh.mi.dwFlags = MOUSEEVENTF_WHEEL;
        wh.mi.mouseData = (DWORD)(int)(notches > 0 ? WHEEL_DELTA : -WHEEL_DELTA);
        SendInput(1, &wh, sizeof(INPUT));
        Sleep(150);
    }
    kd.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &kd, sizeof(INPUT));
}

// Same, but deliver the wheel straight to the listview via SendMessage while
// a REAL Ctrl press is down — works even when other windows cover the
// desktop (a real wheel event would hit the covering window instead).
static void CtrlWheelDirect(HWND lv, int notches) {
    INPUT kd = {}; kd.type = INPUT_KEYBOARD; kd.ki.wVk = VK_CONTROL;
    SendInput(1, &kd, sizeof(INPUT));
    Sleep(100);
    RECT rc;
    GetClientRect(lv, &rc);
    int n = notches < 0 ? -notches : notches;
    for (int i = 0; i < n; i++) {
        short delta = (short)(notches > 0 ? WHEEL_DELTA : -WHEEL_DELTA);
        SendMessageTimeoutW(lv, WM_MOUSEWHEEL,
            MAKEWPARAM(MK_CONTROL, (WORD)delta),
            MAKELPARAM(rc.right / 2, rc.bottom / 2),
            SMTO_ABORTIFHUNG, 500, nullptr);
        Sleep(150);
    }
    kd.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &kd, sizeof(INPUT));
}

// LVIR_ICON height of one item, read out of explorer's address space.
static long IconHeightOf(HWND lv, int i) {
    DWORD pid = 0;
    GetWindowThreadProcessId(lv, &pid);
    HANDLE proc = OpenProcess(
        PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);
    if (!proc) return -1;
    long out = -1;
    void* mem = VirtualAllocEx(proc, nullptr, sizeof(RECT), MEM_COMMIT, PAGE_READWRITE);
    if (mem) {
        RECT in = {};
        in.left = LVIR_ICON;
        WriteProcessMemory(proc, mem, &in, sizeof(RECT), nullptr);
        DWORD_PTR res = 0;
        SendMessageTimeoutW(lv, LVM_GETITEMRECT, i, (LPARAM)mem,
                            SMTO_ABORTIFHUNG, 500, &res);
        if (res) {
            RECT back = {};
            if (ReadProcessMemory(proc, mem, &back, sizeof(RECT), nullptr))
                out = back.bottom - back.top;
        }
        VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
    }
    CloseHandle(proc);
    return out;
}

// Drive the desktop zoom all the way to its maximum (glyph = 256px jumbo,
// the one size known for certain) and count the notches, then zoom back out
// notch by notch until the original height is restored. The height measured
// at the plateau pins the LVIR_ICON-rect -> glyph mapping.
static void ZoomToMax() {
    HWND lv = FindDesktopLV();
    if (!lv) { printf("SysListView32 not found\n"); return; }
    long h0 = IconHeightOf(lv, 0);
    printf("start: LVIR_ICON height = %ld\n", h0);
    if (h0 < 0) return;

    int k = 0;
    long last = h0;
    for (; k < 40; k++) {
        CtrlWheelDirect(lv, +1);
        Sleep(450);
        long h = IconHeightOf(lv, 0);
        printf("notch %+2d: h=%ld%s\n", k + 1, h,
               (h == last && h >= 0) ? "  (plateau)" : "");
        if (h < 0) break;
        if (h == last) break;
        last = h;
    }
    DumpGeom("at max  ");

    for (int j = 0; j < k + 2; j++) {
        long h = IconHeightOf(lv, 0);
        printf("restore %-2d: h=%ld\n", j, h);
        if (h == h0) break;
        CtrlWheelDirect(lv, -1);
        Sleep(450);
    }
    DumpGeom("end     ");
}

// Calibration run: zoom the REAL desktop in (and back) with injected
// Ctrl+wheel and dump the live geometry before/after. That yields the
// LVIR_ICON-rect -> glyph-size mapping without guessing. Prefers genuine
// input at a free desktop spot; falls back to messaging the listview
// directly when everything is covered. The zoom level ends where it started.
static void ZoomTest(int notches) {
    HWND lv = FindDesktopLV();
    if (!lv) { printf("SysListView32 not found\n"); return; }
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int px = -1, py = -1;
    for (int y = work.top + 8; py < 0 && y + 64 <= work.bottom; y += 32)
        for (int x = work.left + 8; py < 0 && x + 64 <= work.right; x += 32) {
            RECT r{x, y, x + 64, y + 64};
            if (!RectCovered(r)) { px = x + 32; py = y + 32; }
        }
    bool direct = px < 0;
    if (direct) printf("desktop fully covered -> messaging the listview directly\n");
    else { printf("cursor -> (%d,%d)\n", px, py); SetCursorPos(px, py); }
    Sleep(300);
    DumpGeom("before  ");
    if (direct) CtrlWheelDirect(lv, notches); else CtrlWheel(notches);
    Sleep(800);
    DumpGeom("zoomed  ");
    if (direct) CtrlWheelDirect(lv, -notches); else CtrlWheel(-notches);
    Sleep(800);
    DumpGeom("restored");
}

static int RunReplica() {
    Reg(L"RegionSpikeW", CreateSolidBrush(RGB(60, 60, 80)));    // popup
    Reg(L"RegionSpikeC", CreateSolidBrush(RGB(130, 45, 45)));   // fake listview
    Reg(L"RegionSpikeF", CreateSolidBrush(RGB(45, 145, 65)));   // plain child

    const int W = 460, H = 340;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int px = 200, py = 150;   // fallback position
    for (int y = 0; y + H <= sh && RectCovered(RECT{px, py, px + W, py + H}); y += 40) {
        bool found = false;
        for (int x = 0; x + W <= sw; x += 40) {
            RECT r{x, y, x + W, y + H};
            if (!RectCovered(r)) { px = x; py = y; found = true; break; }
        }
        if (found) break;
    }
    printf("placing replica at (%d,%d)\n", px, py);

    HWND w = CreateWindowExW(0, L"RegionSpikeW", L"region spike (auto-closes)",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU,
                             px, py, W, H, nullptr, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    RECT crc;
    GetClientRect(w, &crc);
    HWND c = CreateWindowExW(0, L"RegionSpikeC", nullptr, WS_CHILD | WS_VISIBLE,
                             0, 0, crc.right, crc.bottom, w, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    HWND f1 = CreateWindowExW(0, L"RegionSpikeF", nullptr, WS_CHILD | WS_VISIBLE,
                              20, 20, 130, 90, c, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    HWND f2 = CreateWindowExW(WS_EX_LAYERED, L"RegionSpikeF", nullptr,
                              WS_CHILD | WS_VISIBLE,
                              170, 20, 130, 90, c, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    if (!f2) printf("!! layered child create failed (err=%lu)\n", GetLastError());
    else PaintLayeredSolid(f2, 130, 90, 70, 120, 220);   // blue layered "fence"

    SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    ShowWindow(w, SW_SHOW);
    SetForegroundWindow(w);
    UpdateWindow(w);

    printf("replica: popup=%p parent C=%p plain F1=%p layered F2=%p\n",
           (void*)w, (void*)c, (void*)f1, (void*)f2);

    auto ProbeAll = [&](const char* phase) {
        printf("%s\n", phase);
        Probe(f1, "plain child");
        if (f2) Probe(f2, "layered child");
        Probe(c, "parent away from kids");
    };
    ProbeAll("phase 1: no region on C");
    SetWindowRgn(c, CreateRectRgn(0, 0, 0, 0), TRUE);
    ProbeAll("phase 2: EMPTY region on C");
    SetWindowRgn(c, NULL, TRUE);
    ProbeAll("phase 3: region restored");

    printf("window stays open 15s for visual inspection; Alt+F4 closes\n");
    SetTimer(w, 1, 15000, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

// Can we SetParent a window of OUR process between two windows owned by
// explorer (lv -> defview -> back)? Creates a throwaway layered child under
// SysListView32, reparents it, checks parentage and screen position, destroys.
static int TestReparent() {
    HWND lv = FindDesktopLV();
    HWND defview = lv ? GetParent(lv) : nullptr;
    if (!lv || !defview) { printf("desktop tree not found\n"); return 1; }
    printf("lv=%p defview=%p\n", (void*)lv, (void*)defview);

    HWND t = CreateWindowExW(WS_EX_LAYERED, L"RegionSpikeF", L"spike",
                             WS_CHILD | WS_VISIBLE, 3, 3, 40, 40, lv, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    if (!t) { printf("create child of lv FAILED err=%lu\n", GetLastError()); return 1; }
    printf("created %p under lv (cross-process parenting OK)\n", (void*)t);

    RECT before;
    GetWindowRect(t, &before);

    HWND r = SetParent(t, defview);
    printf("SetParent(defview) -> %p err=%lu  GetParent=%p\n",
           (void*)r, GetLastError(), (void*)GetParent(t));
    if (r) {
        POINT p{before.left, before.top};
        ScreenToClient(defview, &p);
        SetWindowPos(t, nullptr, p.x, p.y, 40, 40, SWP_NOZORDER | SWP_NOACTIVATE);
        RECT now;
        GetWindowRect(t, &now);
        printf("  screen rect kept: before=[%ld,%ld] after=[%ld,%ld]\n",
               before.left, before.top, now.left, now.top);
    }

    HWND r2 = SetParent(t, lv);
    printf("SetParent(lv) back -> %p err=%lu  GetParent=%p\n",
           (void*)r2, GetLastError(), (void*)GetParent(t));

    DestroyWindow(t);
    printf("destroyed\n");
    return (r && r2) ? 0 : 1;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        printf("usage: region_spike replica|reparent|desksize|zoom [n]|max|hide|test|show\n");
        return 1;
    }
    if (!wcscmp(argv[1], L"replica")) return RunReplica();
    if (!wcscmp(argv[1], L"desksize")) { DumpGeom("now     "); return 0; }
    if (!wcscmp(argv[1], L"zoom")) {
        int n = argc > 2 ? (int)wcstol(argv[2], nullptr, 10) : 3;
        if (n == 0) n = 3;
        ZoomTest(n);
        return 0;
    }
    if (!wcscmp(argv[1], L"max")) { ZoomToMax(); return 0; }
    if (!wcscmp(argv[1], L"reparent")) {
        Reg(L"RegionSpikeF", CreateSolidBrush(RGB(45, 145, 65)));
        return TestReparent();
    }

    HWND lv = FindDesktopLV();
    if (!lv) { printf("SysListView32 not found\n"); return 1; }
    printf("SysListView32 = %p\n", (void*)lv);
    if (!wcscmp(argv[1], L"hide")) {
        HRGN empty = CreateRectRgn(0, 0, 0, 0);
        printf("SetWindowRgn(empty) -> %d\n", SetWindowRgn(lv, empty, TRUE));
    } else if (!wcscmp(argv[1], L"show")) {
        printf("SetWindowRgn(NULL) -> %d\n", SetWindowRgn(lv, NULL, TRUE));
    } else if (!wcscmp(argv[1], L"test")) {
        ProbeLive(lv);
    } else if (!wcscmp(argv[1], L"toggle")) {
        SendToggleHotkey();
    } else if (!wcscmp(argv[1], L"list")) {
        HWND defview = GetParent(lv);
        printf("lv visible=%d defview=%p\n", IsWindowVisible(lv) ? 1 : 0, (void*)defview);
        // Fences are children, so EnumWindows never sees them; walk defview's
        // subtree and keep only direct children of lv or defview.
        EnumChildWindows(defview, [](HWND h, LPARAM lp) -> BOOL {
            HWND lv2 = (HWND)lp;
            HWND dv = GetParent(lv2);
            wchar_t cls[64] = {};
            GetClassNameW(h, cls, 64);
            if (wcscmp(cls, L"OpenFencesFence")) return TRUE;
            HWND par = GetParent(h);
            if (par != lv2 && par != dv) return TRUE;
            wchar_t pcls[64] = {};
            if (par) GetClassNameW(par, pcls, 64);
            printf("  fence %p visible=%d parent=%p \"%ls\"%s\n", (void*)h,
                   IsWindowVisible(h) ? 1 : 0, (void*)par, pcls,
                   par == lv2 ? "" : "   (parked off the listview)");
            return TRUE;
        }, (LPARAM)lv);
    } else {
        printf("unknown mode\n");
        return 1;
    }
    return 0;
}
