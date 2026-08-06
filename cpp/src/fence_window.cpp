#include "fence_window.h"
#include "settings_panel.h"
#include "context_menu.h"
#include "menu_icons.h"
#include "config.h"
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>     // IShellFolder, PIDL helpers (shell context menu)
#include <shobjidl.h>   // IContextMenu
#include <shlwapi.h>
#include <commctrl.h>   // TrackMouseEvent (hover leave tracking)
#include <cmath>

HWND FenceWindow::s_owner = nullptr;
int  FenceWindow::s_lang = 1; // 0=EN, 1=ZH — default Chinese

const wchar_t* FenceWindow::Loc(const wchar_t* en, const wchar_t* zh) {
    return s_lang == 1 ? zh : en;
}

// Inset between the fence border and the icon grid (also the free-drag clamp)
static const float kGridPad = 4.0f;

// Process is DPI-aware (see main.cpp), so this returns the real scale factor
static float DpiScale() {
    HDC h = GetDC(nullptr);
    int d = GetDeviceCaps(h, LOGPIXELSX);
    ReleaseDC(nullptr, h);
    return d / 96.0f;
}

// The desktop parent runs on explorer's DPI awareness (Per-Monitor v2 on
// modern Windows, matching our manifest). On the off chance the host context
// differs from ours, Windows refuses to create the child — so for the
// creation call we adopt the host's context and restore ours right after.
#ifndef DPI_AWARENESS_CONTEXT_UNAWARE
DECLARE_HANDLE(DPI_AWARENESS_CONTEXT);
#endif
typedef DPI_AWARENESS_CONTEXT (WINAPI *GetWinDpiCtxT)(HWND);
typedef DPI_AWARENESS_CONTEXT (WINAPI *SetThreadDpiCtxT)(DPI_AWARENESS_CONTEXT);

static DPI_AWARENESS_CONTEXT AdoptHostAwareness(HWND host) {
    if (!host) return nullptr;
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    auto getCtx = (GetWinDpiCtxT)(void*)GetProcAddress(u32, "GetWindowDpiAwarenessContext");
    auto setCtx = (SetThreadDpiCtxT)(void*)GetProcAddress(u32, "SetThreadDpiAwarenessContext");
    if (!getCtx || !setCtx) return nullptr;
    DPI_AWARENESS_CONTEXT hostCtx = getCtx(host);
    return hostCtx ? setCtx(hostCtx) : nullptr;  // nullptr = nothing to restore
}

static void RestoreAwareness(DPI_AWARENESS_CONTEXT prev) {
    if (!prev) return;
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    auto setCtx = (SetThreadDpiCtxT)(void*)GetProcAddress(u32, "SetThreadDpiAwarenessContext");
    if (setCtx) setCtx(prev);
}

HWND FenceWindow::DesktopHost() {
    // On Windows 11 the desktop is one layered surface owned by
    // SHELLDLL_DefView (it paints the wallpaper *and* composites the icon
    // list). Arbitrary children of Progman/WorkerW are never painted into
    // that surface and therefore never appear on screen. The one window
    // whose normal child painting the shell does composite is the desktop
    // icon list (SysListView32), so fences parent themselves to it — they
    // render, and they become part of the desktop tree (immune to Win+D).
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
    if (!progman) return nullptr;
    if (HWND lv = listViewOf(progman)) return lv;
    // Wallpaper/slideshow layouts: Progman → WorkerW → SHELLDLL_DefView
    for (HWND ww = FindWindowExW(progman, nullptr, L"WorkerW", nullptr); ww;
         ww = FindWindowExW(progman, ww, L"WorkerW", nullptr))
        if (HWND lv = listViewOf(ww)) return lv;
    // Rare shell states put the WorkerW at the top level
    for (HWND ww = FindWindowExW(nullptr, nullptr, L"WorkerW", nullptr); ww;
         ww = FindWindowExW(nullptr, ww, L"WorkerW", nullptr))
        if (HWND lv = listViewOf(ww)) return lv;
    return progman;   // no icon list (icons disabled?) — best effort
}

FenceWindow::FenceWindow(const FenceData& data)
    : m_title(data.title), m_id(data.id), m_x(data.x), m_y(data.y)
{
    m_render = std::make_unique<RenderContext>(data.w, data.h);

    // Fences live *inside* the desktop window tree (see DesktopHost). That
    // is what makes Win+D leave them alone: the shell's "show desktop" sweep
    // only minimizes foreign top-level windows, never the desktop's own
    // children. If the desktop is not up yet (no explorer), fall back to a
    // plain popup; the owner window re-creates us when explorer (re)starts.
    HWND host = DesktopHost();
    POINT pos{ data.x, data.y };
    if (host) ScreenToClient(host, &pos);   // child coords are parent-relative

    DPI_AWARENESS_CONTEXT prevCtx = AdoptHostAwareness(host);
    m_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_ACCEPTFILES,
        ClassName(), data.title.c_str(), host ? WS_CHILD : WS_POPUP,
        pos.x, pos.y, data.w, data.h,
        host, nullptr, GetModuleHandle(nullptr), this);
    RestoreAwareness(prevCtx);

    if (!m_hwnd) return;   // no usable desktop / unexpected failure
    SetWindowLongPtr(m_hwnd, GWLP_USERDATA, (LONG_PTR)this);
    if (host)
        SetWindowPos(m_hwnd, HWND_TOP, 0, 0, 0, 0,   // above the desktop icons
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    DragAcceptFiles(m_hwnd, TRUE);
    ShowWindow(m_hwnd, SW_SHOW);
    Redraw();
    CaptureBackdrop();   // initial frosted-glass snapshot
}

FenceWindow::~FenceWindow() {
    // If the settings panel is open for this fence, tear it down first so it
    // never keeps referencing a fence that is going away.
    SettingsPanel::CloseActiveFor(this);
    // When explorer restarts, the desktop tree takes our child window down
    // with it — by the time we run, the HWND is already dead.
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    if (m_cursorTimer) KillTimer(m_hwnd, 1);
    DragAcceptFiles(m_hwnd, FALSE);
    DestroyWindow(m_hwnd);
}

void FenceWindow::Show() { m_appHidden = false; ShowWindow(m_hwnd, SW_SHOW); Redraw(); }
void FenceWindow::Hide() { m_appHidden = true;  ShowWindow(m_hwnd, SW_HIDE); }

void FenceWindow::Redraw() {
    std::wstring displayTitle = m_renaming
        ? (m_renameBuf + (m_cursorVisible ? L"|" : L""))
        : m_title;
    // The marquee band only exists once the gesture passes the click
    // threshold — a plain click must not flash it.
    RECT marq = {};
    bool showMarq = m_marquee && m_marqueeMoved;
    if (showMarq) marq = MarqueeRect();
    m_render->BeginDraw();
    // Explorer-style states: hover plate, selection plates, the translucent
    // "lifted" look for the whole dragged selection (only once the drag has
    // actually moved), and the rubber band while one is live.
    m_render->DrawFence(displayTitle, m_icons, m_renaming, m_dragOver, m_collapsed,
                        m_collapsed ? -1 : m_hoverIcon,
                        &m_selectedPaths,
                        m_dragMoved && m_dragIcon >= 0,
                        showMarq ? &marq : nullptr,
                        m_chevronHover, m_chevronDown);
    m_render->EndDraw();
    m_render->Present(m_hwnd, m_x, m_y);
}

void FenceWindow::SetIcons(const std::vector<IconEntry>& entries) {
    m_icons = entries;
    Redraw();
}

void FenceWindow::AddIcon(const std::wstring& name, const std::wstring& path,
                          float atX, float atY) {
    // Snap onto the icon grid: the drop point picks the nearest cell, and a
    // taken cell defers to the nearest free one. Dropping a whole file
    // selection therefore fills cells around the drop point instead of
    // stacking every icon on top of the others.
    IconGrid g = GetIconGrid();
    int c = (int)floorf((atX - g.x0) / g.cw);
    int r = (int)floorf((atY - g.y0) / g.ch);
    c = (std::min)((std::max)(c, 0), g.cols - 1);
    r = (std::min)((std::max)(r, 0), g.rows - 1);
    int cell = r * g.cols + c;

    // The same file already lives in this fence: move its icon to the drop
    // cell (pushing the occupant aside) instead of adding a duplicate.
    // Paths are case-insensitive on Windows.
    for (int i = 0; i < (int)m_icons.size(); i++) {
        if (lstrcmpiW(m_icons[i].path.c_str(), path.c_str()) == 0) {
            PushOccupantOf(cell, { i });
            PlaceIconAt(m_icons[i], cell);
            Redraw();
            Config::MarkDirty();
            return;
        }
    }

    IconEntry e;
    e.name = name;
    e.path = path;
    PlaceIconAt(e, FindFreeCell(cell, OccupiedCells({})));
    m_icons.push_back(std::move(e));
    Redraw();
    Config::MarkDirty();
}

int FenceWindow::HitTestIcon(int mx, int my) const {
    const auto& app = m_render->Appearance();
    // Topmost icon wins → iterate in reverse draw order
    for (int i = (int)m_icons.size() - 1; i >= 0; i--) {
        if ((float)mx >= m_icons[i].x && (float)mx < m_icons[i].x + app.cellW &&
            (float)my >= m_icons[i].y && (float)my < m_icons[i].y + app.cellH)
            return i;
    }
    return -1;
}

// Path-keyed membership: the bring-to-front reorder a drag performs must
// not drop the selection, so no selection state may hang off indexes.
static bool ContainsPath(const std::vector<std::wstring>& paths,
                         const std::wstring& path) {
    return std::find(paths.begin(), paths.end(), path) != paths.end();
}

bool FenceWindow::IsPathSelected(const std::wstring& path) const {
    return ContainsPath(m_selectedPaths, path);
}

void FenceWindow::ClearSelection() { m_selectedPaths.clear(); }

void FenceWindow::SelectOnly(int idx) {
    m_selectedPaths.clear();
    if (idx >= 0 && idx < (int)m_icons.size())
        m_selectedPaths.push_back(m_icons[idx].path);
}

int FenceWindow::SelectedIndex() const {
    for (int i = 0; i < (int)m_icons.size(); i++)
        if (IsPathSelected(m_icons[i].path)) return i;
    return -1;
}

void FenceWindow::BringSelectedToFront() {
    // Selected icons draw above the rest (draw order = vector order). The
    // relative order inside each group is kept, and selection is path-keyed,
    // so callers must re-locate any index they still hold.
    if (m_selectedPaths.empty()) return;
    std::vector<IconEntry> rest, sel;
    rest.reserve(m_icons.size());
    sel.reserve(m_selectedPaths.size());
    for (auto& e : m_icons) {
        if (IsPathSelected(e.path)) sel.push_back(std::move(e));
        else                        rest.push_back(std::move(e));
    }
    m_icons.clear();
    for (auto& e : rest) m_icons.push_back(std::move(e));
    for (auto& e : sel)  m_icons.push_back(std::move(e));
}

void FenceWindow::ClampIconPos(float& x, float& y) const {
    const auto& app = m_render->Appearance();
    float minX = kGridPad;
    float minY = app.titleH + kGridPad;
    float maxX = (float)m_render->Width()  - app.cellW - kGridPad;
    float maxY = (float)m_render->Height() - app.cellH - kGridPad;
    // (max)(…, minX) keeps the clamp sane if the fence is smaller than a cell
    x = (std::min)((std::max)(x, minX), (std::max)(maxX, minX));
    y = (std::min)((std::max)(y, minY), (std::max)(maxY, minY));
}

// ── Icon grid ──

FenceWindow::IconGrid FenceWindow::GetIconGrid() const {
    const auto& app = m_render->Appearance();
    float W = (float)m_render->Width(), H = (float)m_render->Height();
    IconGrid g;
    g.cw = app.cellW; g.ch = app.cellH;
    g.x0 = kGridPad;
    g.y0 = app.titleH + kGridPad;
    // Only whole cells count; any leftover margin stays free at the right and
    // bottom edges (the desktop behaves the same).
    g.cols = (std::max)(1, (int)((W - 2 * kGridPad) / g.cw));
    g.rows = (std::max)(1, (int)((H - kGridPad - g.y0) / g.ch));
    return g;
}

int FenceWindow::CellOf(const IconEntry& e) const {
    IconGrid g = GetIconGrid();
    // Map the icon's *center* so a half-cell offset still rounds to the cell
    // the icon visually belongs to.
    int c = (int)floorf((e.x + g.cw * 0.5f - g.x0) / g.cw);
    int r = (int)floorf((e.y + g.ch * 0.5f - g.y0) / g.ch);
    c = (std::min)((std::max)(c, 0), g.cols - 1);
    r = (std::min)((std::max)(r, 0), g.rows - 1);
    return r * g.cols + c;
}

void FenceWindow::PlaceIconAt(IconEntry& e, int cell) const {
    IconGrid g = GetIconGrid();
    int c = cell % g.cols, r = cell / g.cols;
    // Whole-pixel positions: fractional offsets force bilinear resampling
    // and make the glyph look blurry.
    e.x = floorf(g.x0 + c * g.cw + 0.5f);
    e.y = floorf(g.y0 + r * g.ch + 0.5f);
}

std::vector<int> FenceWindow::OccupiedCells(const std::vector<int>& excludes) const {
    std::vector<int> occ;
    occ.reserve(m_icons.size());
    for (int i = 0; i < (int)m_icons.size(); i++)
        if (std::find(excludes.begin(), excludes.end(), i) == excludes.end())
            occ.push_back(CellOf(m_icons[i]));
    return occ;
}

int FenceWindow::FindFreeCell(int prefer, const std::vector<int>& occupied) const {
    IconGrid g = GetIconGrid();
    auto taken = [&](int idx) {
        return std::find(occupied.begin(), occupied.end(), idx) != occupied.end();
    };
    if (!taken(prefer)) return prefer;
    // Walk Chebyshev rings around the preferred cell, columns first so a
    // crowded drop fills downward, then to the right — desktop-like.
    int pc = prefer % g.cols, pr = prefer / g.cols;
    auto dist = [](int a, int b) { int d = a - b; return d < 0 ? -d : d; };
    for (int d = 1, maxD = (std::max)(g.cols, g.rows); d <= maxD; d++) {
        for (int c = pc - d; c <= pc + d; c++) {
            for (int r = pr - d; r <= pr + d; r++) {
                if ((std::max)(dist(c, pc), dist(r, pr)) != d) continue;   // ring only
                if (c < 0 || c >= g.cols || r < 0 || r >= g.rows) continue;
                int idx = r * g.cols + c;
                if (!taken(idx)) return idx;
            }
        }
    }
    return prefer;   // fence completely full — overlap is unavoidable
}

int FenceWindow::FindPushCell(int prefer, const std::vector<int>& occupied, int dirX) const {
    IconGrid g = GetIconGrid();
    auto taken = [&](int idx) {
        return std::find(occupied.begin(), occupied.end(), idx) != occupied.end();
    };
    int pc = prefer % g.cols, pr = prefer / g.cols;
    int maxD = (std::max)(g.cols, g.rows);
    // Scan one row outward from pc: the drag-direction side first, then the
    // other side, nearest cells first — pushed icons slide along the row.
    auto scanRow = [&](int r) -> int {
        if (r < 0 || r >= g.rows) return -1;
        for (int dc = 0; dc <= maxD; dc++) {
            int passes = (dc == 0) ? 1 : 2;
            for (int pass = 0; pass < passes; pass++) {
                int side;
                if (dc == 0)       side = 0;
                else if (pass == 0) side = (dirX < 0) ? -1 : 1;
                else                side = (dirX < 0) ? 1 : -1;
                int c = pc + side * dc;
                if (c < 0 || c >= g.cols) continue;
                int idx = r * g.cols + c;
                if (!taken(idx)) return idx;
            }
        }
        return -1;
    };
    // Same row first; only a completely full row spills, downward before up.
    for (int dr = 0; dr <= maxD; dr++) {
        if (dr == 0) {
            int f = scanRow(pr);
            if (f >= 0) return f;
        } else {
            int f = scanRow(pr + dr);
            if (f >= 0) return f;
            f = scanRow(pr - dr);
            if (f >= 0) return f;
        }
    }
    return prefer;   // fence completely full — overlap unavoidable
}

void FenceWindow::RelayoutIcons() {
    if (m_collapsed) return;   // body is folded away; positions stay frozen
    std::vector<int> taken;
    taken.reserve(m_icons.size());
    for (auto& ic : m_icons) {
        int cell = FindFreeCell(CellOf(ic), taken);
        PlaceIconAt(ic, cell);
        taken.push_back(cell);
    }
    Redraw();
}

void FenceWindow::SyncDesktopIconSize(int physicalPx) {
    if (!m_hwnd || physicalPx <= 0) return;
    // Never fight a live gesture — the icon being dragged (or the whole
    // fence being moved/resized, or a marquee on the move) works in the
    // current metrics; catch up instead.
    if (m_dragIcon >= 0 || m_moving || m_resizeMask || m_marquee) return;
    if (!m_render->AdoptDesktopIconSize(physicalPx)) return;
    if (m_collapsed) {
        // The grid cannot be rebuilt while folded (height = title bar only).
        m_relayoutPending = true;
        return;
    }
    RelayoutIcons();   // re-seats every icon on the resized grid and redraws
}

void FenceWindow::PushOccupantOf(int cell, const std::vector<int>& excludes,
                                 int dirX) {
    for (int j = 0; j < (int)m_icons.size(); j++) {
        if (std::find(excludes.begin(), excludes.end(), j) != excludes.end())
            continue;
        if (CellOf(m_icons[j]) != cell) continue;
        // Occupancy excludes only the floating (dragged) group; the
        // occupant's own claim on `cell` keeps the search from sending it
        // right back. The cells the group lifted from count as free, so
        // pushes naturally flow into the gaps the drag leaves behind. dirX
        // steers the search along the row in the drag's direction.
        PlaceIconAt(m_icons[j], FindPushCell(cell, OccupiedCells(excludes), dirX));
        return;   // one occupant per cell
    }
}

void FenceWindow::PushGroupOut() {
    // Clear bystanders from every cell the dragged group currently covers.
    // Distinct cells only — members folded onto one cell during the drag
    // must not trigger the same sweep twice.
    std::vector<int> covered;
    covered.reserve(m_dragGroup.size());
    for (const auto& d : m_dragGroup) {
        if (d.idx < 0 || d.idx >= (int)m_icons.size()) continue;
        int c = CellOf(m_icons[d.idx]);
        if (std::find(covered.begin(), covered.end(), c) == covered.end())
            covered.push_back(c);
    }
    for (int c : covered) PushOccupantOf(c, m_dragExcludes, m_dragDirX);
}

void FenceWindow::SettleDragGroup() {
    if (m_dragIcon >= 0 && !m_dragGroup.empty()) {
        // Settle the pressed icon first, then the rest of the group in draw
        // order: each snaps to the nearest cell that is free of both the
        // bystanders and the already-settled members. Before the drag
        // threshold every member sits on its own cell, so this settles in
        // place — no visible change for a plain click.
        std::vector<int> order;
        order.reserve(m_dragGroup.size());
        for (const auto& d : m_dragGroup) if (d.idx == m_dragIcon) order.push_back(d.idx);
        for (const auto& d : m_dragGroup) if (d.idx != m_dragIcon) order.push_back(d.idx);
        std::vector<int> taken = OccupiedCells(m_dragExcludes);
        taken.reserve(taken.size() + order.size());
        for (int idx : order) {
            if (idx < 0 || idx >= (int)m_icons.size()) continue;
            int cell = FindFreeCell(CellOf(m_icons[idx]), taken);
            PlaceIconAt(m_icons[idx], cell);
            taken.push_back(cell);
        }
    }
    if (m_dragMoved) Config::MarkDirty();   // icons changed cells
    m_dragIcon = -1;
    m_dragHoverCell = -1;
    m_dragDirX = 0;
    m_dragMoved = false;
    m_dragGroup.clear();
    m_dragExcludes.clear();
    Redraw();
}

// ── Marquee (rubber-band) multi-select ──

RECT FenceWindow::MarqueeRect() const {
    // Anchor→cursor, normalized and clamped to the fence body: the band
    // never paints over the title bar even when the captured cursor sweeps
    // above it, and it cannot leak past the window edges.
    int w = (int)m_render->Width(), h = (int)m_render->Height();
    int top = (int)(m_render->Appearance().titleH + 0.5f);
    int l = (std::min)(m_marqAX, m_marqCX), r = (std::max)(m_marqAX, m_marqCX);
    int t = (std::min)(m_marqAY, m_marqCY), b = (std::max)(m_marqAY, m_marqCY);
    l = (std::min)((std::max)(l, 0), w);
    r = (std::min)((std::max)(r, 0), w);
    t = (std::min)((std::max)(t, top), h);
    b = (std::min)((std::max)(b, top), h);
    return RECT{ l, t, r, b };
}

void FenceWindow::UpdateMarqueeSelection() {
    RECT band = MarqueeRect();
    const auto& app = m_render->Appearance();
    std::vector<std::wstring> next;
    if (m_marqueeAdd) next = m_marqueeBase;   // Ctrl: keep what was selected
    next.reserve(next.size() + m_icons.size());
    for (const auto& ic : m_icons) {
        // Test against the same cell rect HitTestIcon clicks against, and
        // count mere intersection (not containment) — desktop behavior.
        RECT cell{ (long)ic.x, (long)ic.y,
                   (long)(ic.x + app.cellW), (long)(ic.y + app.cellH) };
        RECT inter;
        if (!IntersectRect(&inter, &band, &cell)) continue;
        if (!ContainsPath(next, ic.path)) next.push_back(ic.path);
    }
    m_selectedPaths = std::move(next);
}

void FenceWindow::SetDragOver(bool over) {
    if (m_dragOver != over) {
        m_dragOver = over;
        Redraw();
    }
}

void FenceWindow::ToggleCollapse() {
    int th = (int)(m_render->Appearance().titleH + 0.5f);
    int w  = (int)m_render->Width();
    if (m_collapsed) {
        // Expand back to the remembered full height
        m_collapsed = false;
        int h = m_expandedH > th ? m_expandedH : th * 4;
        SetWindowPos(m_hwnd, nullptr, 0, 0, w, h,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        // Icon metrics may have changed while folded (desktop icon zoom);
        // the grid is available again now, so apply the deferred relayout.
        if (m_relayoutPending) {
            m_relayoutPending = false;
            RelayoutIcons();
        }
    } else {
        // Collapse down to just the title bar
        m_collapsed = true;
        m_expandedH = (int)m_render->Height();
        SetWindowPos(m_hwnd, nullptr, 0, 0, w, th,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // SetWindowPos raises WM_SIZE → Resize + Redraw, and Redraw already knows
    // the new m_collapsed state (arrow direction, icons skipped).
    Config::MarkDirty();
}

void FenceWindow::SyncCollapsedSize() {
    if (!m_collapsed) return;
    int th = (int)(m_render->Appearance().titleH + 0.5f);
    int w  = (int)m_render->Width();
    SetWindowPos(m_hwnd, nullptr, 0, 0, w, th,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void FenceWindow::ScheduleBackdropRefresh() {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    // Same debounce the move/resize path uses (WM_WINDOWPOSCHANGED); timer-2
    // fires CaptureBackdrop once things go quiet for 150 ms.
    SetTimer(m_hwnd, 2, 150, nullptr);
}

void FenceWindow::CaptureBackdrop() {
    int w = (int)m_render->Width(), h = (int)m_render->Height();
    if (w <= 0 || h <= 0) return;

    // Snapshot the desktop area behind the fence. Windows excludes
    // WS_EX_LAYERED windows (including this one) from GDI screen captures,
    // so the fence does not end up inside its own backdrop and we do not
    // need to hide the window (which would break an in-progress drag).
    HDC screenDC = GetDC(nullptr);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;   // top-down rows
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib) {
        HDC memDC = CreateCompatibleDC(screenDC);
        HGDIOBJ oldBmp = SelectObject(memDC, dib);
        BitBlt(memDC, 0, 0, w, h, screenDC, m_x, m_y, SRCCOPY);
        if (bits) m_render->SetBackdrop((const BYTE*)bits, w, h);
        SelectObject(memDC, oldBmp);
        DeleteDC(memDC);
        DeleteObject(dib);
    }
    ReleaseDC(nullptr, screenDC);
    Redraw();
}

FenceData FenceWindow::GetData() const {
    return { m_id, m_title, m_x, m_y, (int)m_render->Width(), (int)m_render->Height(), true };
}

void FenceWindow::EnterRename() {
    m_renaming = true;
    m_renameBuf = m_title;
    m_cursorVisible = true;
    m_cursorTimer = SetTimer(m_hwnd, 1, 530, nullptr);
    // A desktop child shares explorer's foreground; SetFocus can only reach
    // it with our input queue attached to the foreground thread. Attach for
    // the whole rename session (detached in ExitRename).
    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD self_ = GetCurrentThreadId();
    if (fgThread && fgThread != self_ && AttachThreadInput(self_, fgThread, TRUE))
        m_focusAttached = fgThread;
    SetFocus(m_hwnd);
    Redraw();
}

void FenceWindow::ExitRename(bool commit) {
    if (!m_renaming) return;
    m_renaming = false;
    if (commit && m_renameBuf != m_title) {
        m_title = m_renameBuf;
        Config::MarkDirty();
    }
    KillTimer(m_hwnd, 1);
    m_cursorTimer = 0;
    if (m_focusAttached) {
        AttachThreadInput(GetCurrentThreadId(), m_focusAttached, FALSE);
        m_focusAttached = 0;
    }
    Redraw();
}

// Edge hit-test shared by WM_NCHITTEST (resize behavior) and cursor picking.
// Returns HTLEFT/HTRIGHT/… near a border, HTCLIENT anywhere in the body.
// `collapsed` fences are a bare title pill — nothing to resize there.
static int EdgeHit(HWND hwnd, bool collapsed, int mx, int my) {
    if (collapsed) return HTCLIENT;
    RECT rc; GetClientRect(hwnd, &rc);
    const int E = (int)(8.0f * DpiScale() + 0.5f);
    bool L = mx < E, R = mx > rc.right - E;
    bool T = my < E, B = my > rc.bottom - E;
    if (T && L) return HTTOPLEFT;
    if (T && R) return HTTOPRIGHT;
    if (B && L) return HTBOTTOMLEFT;
    if (B && R) return HTBOTTOMRIGHT;
    if (L) return HTLEFT;
    if (R) return HTRIGHT;
    if (T) return HTTOP;
    if (B) return HTBOTTOM;
    return HTCLIENT;
}

// Pointer shape matching a WM_NCHITTEST code.
static LPCWSTR CursorForHT(int ht) {
    switch (ht) {
    case HTTOPLEFT: case HTBOTTOMRIGHT: return IDC_SIZENWSE;
    case HTTOPRIGHT: case HTBOTTOMLEFT: return IDC_SIZENESW;
    case HTLEFT:     case HTRIGHT:      return IDC_SIZEWE;
    case HTTOP:      case HTBOTTOM:     return IDC_SIZENS;
    default:                            return IDC_ARROW;
    }
}

// ── Shell context menu (the Explorer right-click menu for a file) ──
//
// Resolving one: parse the full path into an absolute PIDL, bind to the parent
// folder's IShellFolder, and ask it for the child's IContextMenu. Displaying:
// let the shell fill a popup (command ids in [1, 0x7FFF]), append our single
// fence-specific verb above that range, then dispatch the chosen id.

static HRESULT CreateShellContextMenu(const std::wstring& path, IContextMenu** ppCM) {
    *ppCM = nullptr;

    PIDLIST_ABSOLUTE pidlFull = nullptr;
    HRESULT hr = SHParseDisplayName(path.c_str(), nullptr, &pidlFull, 0, nullptr);
    if (FAILED(hr) || !pidlFull) return E_FAIL;

    // Child = the file's own ID (points inside pidlFull); parent = the folder.
    LPCITEMIDLIST pidlChild = ILFindLastID(pidlFull);
    PIDLIST_ABSOLUTE pidlParent = ILCloneFull(pidlFull);
    if (pidlParent) ILRemoveLastID(pidlParent);

    CComPtr<IShellFolder> spParent;
    CComPtr<IShellFolder> spDesktop;
    hr = SHGetDesktopFolder(&spDesktop);
    if (SUCCEEDED(hr)) {
        if (!pidlParent || ILIsEmpty(pidlParent)) {
            spParent = spDesktop;                    // file lives on the desktop
        } else {
            hr = spDesktop->BindToObject(pidlParent, nullptr, IID_PPV_ARGS(&spParent));
        }
    }

    if (SUCCEEDED(hr) && spParent && pidlChild) {
        hr = spParent->GetUIObjectOf(nullptr, 1,
            reinterpret_cast<PCUITEMID_CHILD_ARRAY>(&pidlChild),
            IID_IContextMenu, nullptr, (void**)ppCM);
    }

    if (pidlParent) CoTaskMemFree(pidlParent);
    CoTaskMemFree(pidlFull);
    return hr;
}

// The fence-only verb is placed far above the shell's command-id range.
static const int kCmdRemoveFromFence = 0x8001;

// Shows the Explorer menu for `path` at `ptScreen` (screen coords) with a
// "remove from fence" entry appended. Returns false only when no system menu
// could be produced (caller falls back to the plain fence menu). Sets
// `invoked` when a shell command ran and `removeChosen` for our own verb.
static bool RunShellContextMenu(HWND hwnd, const std::wstring& path,
                                const std::wstring& displayName, POINT ptScreen,
                                bool& invoked, bool& removeChosen) {
    invoked = removeChosen = false;

    CComPtr<IContextMenu> spCM;
    if (FAILED(CreateShellContextMenu(path, &spCM)) || !spCM) return false;

    HMENU menu = CreatePopupMenu();
    if (!menu) return false;

    if (FAILED(spCM->QueryContextMenu(menu, 0, 1, 0x7FFF,
                                      CMF_EXPLORE | CMF_CANRENAME))) {
        DestroyMenu(menu);
        return false;
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    std::wstring rm = std::wstring(FenceWindow::Loc(L"Remove from fence", L"从围栏移除")) +
                      L"  \"" + displayName + L"\"";
    AppendMenuW(menu, MF_STRING, kCmdRemoveFromFence, rm.c_str());

    // Win11 look first: we repaint the (identical) HMENU contents ourselves,
    // because Explorer's XAML menu cannot be obtained cross-process. If the
    // Fluent layer fails to start, fall back to the classic system menu.
    SetForegroundWindow(hwnd);
    int idCmd = FluentMenu::Run(hwnd, spCM, menu, path, ptScreen);
    if (idCmd < 0)
        idCmd = TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_RETURNCMD | TPM_RIGHTBUTTON,
                               ptScreen.x, ptScreen.y, 0, hwnd, nullptr);
    DestroyMenu(menu);

    if (idCmd <= 0) return true;                 // dismissed — nothing to run

    if (idCmd == kCmdRemoveFromFence) {
        removeChosen = true;
        return true;
    }
    if (idCmd >= 1 && idCmd <= 0x7FFF) {
        CMINVOKECOMMANDINFO ici = {};
        ici.cbSize = sizeof(ici);
        ici.hwnd = hwnd;
        ici.lpVerb = MAKEINTRESOURCEA(idCmd - 1);
        ici.nShow = SW_SHOWNORMAL;
        if (SUCCEEDED(spCM->InvokeCommand(&ici))) invoked = true;
    }
    return true;
}

void FenceWindow::RegisterClass(HINSTANCE hInst) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = ClassName();
    RegisterClassExW(&wc);
}

LRESULT CALLBACK FenceWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = (FenceWindow*)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        auto* cs = (CREATESTRUCT*)lp;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }
    case WM_DROPFILES: {
        HDROP hdrop = (HDROP)wp;
        if (!self) { DragFinish(hdrop); return 0; }
        self->SetDragOver(false);

        // Drop point in this window's client coordinates
        POINT dropPt = {};
        DragQueryPoint(hdrop, &dropPt);
        float dx = (float)dropPt.x, dy = (float)dropPt.y;

        UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; i++) {
            WCHAR path[MAX_PATH];
            if (DragQueryFileW(hdrop, i, path, MAX_PATH)) {
                // Copy filename to separate buffer before stripping extension,
                // so 'path' keeps the original extension for icon loading.
                WCHAR display[MAX_PATH];
                wcscpy_s(display, PathFindFileNameW(path));
                PathRemoveExtensionW(display);
                if (display[0])
                    self->AddIcon(display, path, dx, dy);
            }
            // No manual cascade: AddIcon snaps every file to the nearest FREE
            // grid cell, so a multi-drop fills cells around the drop point.
        }
        DragFinish(hdrop);
        return 0;
    }
    case WM_NCHITTEST: {
        if (!self) return HTCLIENT;
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        return EdgeHit(hwnd, self->m_collapsed, pt.x, pt.y);
    }
    case WM_SETCURSOR: {
        // A fence is a layered child of *another process's* desktop list, so
        // the usual cursor plumbing (class cursor applied through the parent
        // chain) never reaches it — without this the pointer keeps whatever
        // shape it had before entering. Recompute the hit from the live
        // cursor position: during a resize drag the pointer can leave the
        // edge zone, and we still want the sizing shape in that case.
        if (self) {
            int ht = self->m_resizeMask
                ? self->m_resizeHT
                : [&] { POINT p; GetCursorPos(&p); ScreenToClient(hwnd, &p);
                        return EdgeHit(hwnd, self->m_collapsed, p.x, p.y); }();
            SetCursor(LoadCursorW(nullptr, CursorForHT(ht)));
            return TRUE;
        }
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    case WM_SIZE: {
        UINT w = LOWORD(lp), h = HIWORD(lp);
        if (self && w > 0 && h > 0 && (w != self->m_render->Width() || h != self->m_render->Height())) {
            self->m_render->Resize(w, h);
            // Re-seat the icons on the resized grid (also resolves overlaps
            // when the fence shrinks). Skip while collapsing: the tiny height
            // would crush every icon's row and the layout would be lost on
            // expand. RelayoutIcons redraws on its own.
            if (!self->m_collapsed) self->RelayoutIcons();
            else                    self->Redraw();
        }
        return 0;
    }
    case WM_WINDOWPOSCHANGED: {
        if (self) {
            RECT wr; GetWindowRect(hwnd, &wr);
            self->m_x = wr.left;
            self->m_y = wr.top;
            // Re-snapshot the frosted-glass backdrop shortly after the
            // window settles. SetTimer resets the countdown, so a continuous
            // move/resize only triggers one capture once it stops.
            SetTimer(hwnd, 2, 150, nullptr);
        }
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    case WM_NCLBUTTONDOWN: {
        // Edge resize. DefWindowProc's size loop only works for top-level
        // windows, so for our desktop-child fences we run it manually:
        // remember the starting rect/cursor and follow WM_MOUSEMOVE.
        if (!self) return DefWindowProc(hwnd, msg, wp, lp);
        int code = (int)wp;
        if (code >= HTLEFT && code <= HTBOTTOMRIGHT) {
            int mask = 0;
            if (code == HTLEFT || code == HTTOPLEFT || code == HTBOTTOMLEFT)      mask |= 1;
            if (code == HTRIGHT || code == HTTOPRIGHT || code == HTBOTTOMRIGHT)   mask |= 2;
            if (code == HTTOP || code == HTTOPLEFT || code == HTTOPRIGHT)         mask |= 4;
            if (code == HTBOTTOM || code == HTBOTTOMLEFT || code == HTBOTTOMRIGHT) mask |= 8;
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };   // screen coords
            RECT wr; GetWindowRect(hwnd, &wr);
            HWND parent = GetParent(hwnd);
            if (parent) {                   // screen → parent-client
                ScreenToClient(parent, &pt);
                POINT a{ wr.left, wr.top }, b{ wr.right, wr.bottom };
                ScreenToClient(parent, &a);
                ScreenToClient(parent, &b);
                wr = { a.x, a.y, b.x, b.y };
            }
            self->m_resizeMask = mask;
            self->m_resizeHT = code;      // keep this sizing shape for the drag
            self->m_resizeStart = wr;
            self->m_resizePtX = pt.x;
            self->m_resizePtY = pt.y;
            SetCapture(hwnd);
            SetCursor(LoadCursorW(nullptr, CursorForHT(code)));
            return 0;
        }
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    case WM_LBUTTONDOWN: {
        if (!self) return 0;
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        if (!self->m_renaming && my < (int)self->m_render->Appearance().titleH) {
            // Top-right corner of the title bar = collapse/expand arrow.
            // Caption-button semantics: the press only ARMS the button (the
            // pressed plate shows and capture is taken); the fence toggles
            // on RELEASE, and only while the cursor is still over the
            // button — press, drag away, release cancels, exactly like a
            // real window caption button.
            int arrowL = (int)(self->m_render->Width() - self->m_render->Appearance().titleH);
            if (mx >= arrowL) {
                self->m_chevronDown = true;
                self->m_chevronHover = true;
                SetCapture(hwnd);
                self->Redraw();
                return 0;
            }
            // Rest of the title bar → drag the whole fence. Manual capture
            // drag: DefWindowProc's HTCAPTION loop doesn't move child windows.
            self->m_moveOffX = mx;         // cursor offset from the origin
            self->m_moveOffY = my;
            self->m_moving = true;
            SetCapture(hwnd);
            return 0;
        }
        // Icon → select it and arm a drag inside the fence
        int hit = self->HitTestIcon(mx, my);
        if (hit >= 0) {
            std::wstring pressedPath = self->m_icons[hit].path;
            // Explorer selection rule: pressing an already-selected icon
            // keeps the multi-selection (the whole group will drag
            // together); pressing any other icon collapses it to a single.
            if (!self->IsPathSelected(pressedPath)) self->SelectOnly(hit);
            // Bring the WHOLE selection to the front so it draws above the
            // rest while dragging. That reorders the vector, so re-locate
            // every index by path afterwards.
            self->BringSelectedToFront();
            self->m_dragGroup.clear();
            self->m_dragExcludes.clear();
            for (int i = 0; i < (int)self->m_icons.size(); i++) {
                if (!self->IsPathSelected(self->m_icons[i].path)) continue;
                self->m_dragGroup.push_back({ i, self->m_icons[i].x, self->m_icons[i].y });
                self->m_dragExcludes.push_back(i);
                if (self->m_icons[i].path == pressedPath) hit = i;
            }
            self->m_hoverIcon = hit;
            self->m_dragIcon = hit;         // the pressed icon leads the group
            self->m_dragDirX = 0;
            self->m_dragMoved = false;      // real drag starts past the
            self->m_dragGrabX = mx;         // system drag distance — a plain
            self->m_dragGrabY = my;         // click must not disturb anything
            self->m_dragHoverCell = self->CellOf(self->m_icons[hit]);
            SetCapture(hwnd);
            self->Redraw();
        } else {
            // Empty body → marquee multi-select (desktop style). The press
            // arms the band; the click/drag threshold in WM_MOUSEMOVE
            // decides whether it becomes a plain click (which clears the
            // selection) or a live rubber band. Ctrl keeps the current
            // selection and adds to it instead of replacing it.
            self->m_marquee = true;
            self->m_marqueeMoved = false;
            self->m_marqueeAdd = (wp & MK_CONTROL) != 0;
            if (self->m_marqueeAdd) {
                self->m_marqueeBase = self->m_selectedPaths;
            } else {
                self->m_marqueeBase.clear();
                self->ClearSelection();
            }
            self->m_marqAX = self->m_marqCX = mx;
            self->m_marqAY = self->m_marqCY = my;
            SetCapture(hwnd);
            self->Redraw();
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        // Belt and suspenders alongside WM_SETCURSOR: mouse moves are routed
        // straight to this window, so setting the shape here works even if the
        // desktop list parent swallows WM_SETCURSOR. lParam is client coords.
        if (self) {
            int ht = self->m_resizeMask
                ? self->m_resizeHT
                : EdgeHit(hwnd, self->m_collapsed, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            SetCursor(LoadCursorW(nullptr, CursorForHT(ht)));
        }
        // Hover plate (Explorer style). No capture is active in this branch,
        // so re-arm leave tracking on every move and hit-test the cursor.
        if (self && !self->m_collapsed && !self->m_moving &&
            !self->m_resizeMask && self->m_dragIcon < 0 && !self->m_marquee) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            int h = self->HitTestIcon(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (h != self->m_hoverIcon) {
                self->m_hoverIcon = h;
                self->Redraw();
            }
        }
        // Chevron hover. Works expanded AND collapsed (the arrow is the only
        // control a collapsed fence has), and also while the button is
        // pressed — capture keeps the moves coming here, so the plate follows
        // the cursor on and off the button (under-capture coordinates may be
        // negative, hence the explicit >= 0 checks).
        if (self && !self->m_renaming && !self->m_moving &&
            !self->m_resizeMask && self->m_dragIcon < 0 && !self->m_marquee) {
            int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
            int th = (int)self->m_render->Appearance().titleH;
            int arrowL = (int)(self->m_render->Width() - self->m_render->Appearance().titleH);
            bool in = my >= 0 && my < th && mx >= arrowL;
            if (in != self->m_chevronHover) {
                self->m_chevronHover = in;
                self->Redraw();
            }
        }
        if (self && self->m_marquee) {
            int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
            if (!self->m_marqueeMoved) {
                // The same click/drag discrimination icon drags use: a press
                // that never travels the system drag distance stays a plain
                // click (settled in WM_LBUTTONUP).
                int t = GetSystemMetrics(SM_CXDRAG);
                if (abs(mx - self->m_marqAX) < t && abs(my - self->m_marqAY) < t)
                    return 0;
                self->m_marqueeMoved = true;
            }
            self->m_marqCX = mx;
            self->m_marqCY = my;
            self->UpdateMarqueeSelection();
            self->Redraw();
            return 0;
        }
        if (self && self->m_moving) {
            // Cursor to parent-client coords, keep the grabbed offset
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(hwnd, &p);
            HWND parent = GetParent(hwnd);
            if (parent) ScreenToClient(parent, &p);
            SetWindowPos(hwnd, nullptr, p.x - self->m_moveOffX, p.y - self->m_moveOffY,
                0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        if (self && self->m_resizeMask) {
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(hwnd, &p);
            HWND parent = GetParent(hwnd);
            if (parent) ScreenToClient(parent, &p);
            int dx = p.x - self->m_resizePtX;
            int dy = p.y - self->m_resizePtY;
            RECT r = self->m_resizeStart;
            if (self->m_resizeMask & 1) r.left   += dx;
            if (self->m_resizeMask & 2) r.right  += dx;
            if (self->m_resizeMask & 4) r.top    += dy;
            if (self->m_resizeMask & 8) r.bottom += dy;
            // Enforce a sane minimum so the fence can't vanish
            int th  = (int)(self->m_render->Appearance().titleH + 0.5f);
            int minW = (int)(120.0f * DpiScale() + 0.5f);
            int minH = th + (int)(40.0f * DpiScale() + 0.5f);
            if (r.right - r.left < minW) {
                if (self->m_resizeMask & 1) r.left = r.right - minW;
                else                        r.right = r.left + minW;
            }
            if (r.bottom - r.top < minH) {
                if (self->m_resizeMask & 4) r.top = r.bottom - minH;
                else                        r.bottom = r.top + minH;
            }
            SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        if (self && self->m_dragIcon >= 0) {
            int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
            if (!self->m_dragMoved) {
                // Explorer's click/drag discrimination: an icon press only
                // becomes a drag once the cursor travels the system drag
                // distance. Until then the icons stay put (click = select).
                int t = GetSystemMetrics(SM_CXDRAG);
                if (abs(mx - self->m_dragGrabX) < t &&
                    abs(my - self->m_dragGrabY) < t)
                    return 0;
                self->m_dragMoved = true;
                // Stale overlaps under the lifted group resolve at drag
                // start — squatters are pushed to the nearest free cells.
                self->PushGroupOut();
            }
            // The whole selection rides ONE shared delta (Explorer group
            // drag). For a single-icon selection this is exactly the old
            // offset-follow behavior.
            float dx = (float)(mx - self->m_dragGrabX);
            float dy = (float)(my - self->m_dragGrabY);
            for (const auto& d : self->m_dragGroup) {
                if (d.idx < 0 || d.idx >= (int)self->m_icons.size()) continue;
                float nx = d.x + dx, ny = d.y + dy;
                self->ClampIconPos(nx, ny);
                self->m_icons[d.idx].x = floorf(nx + 0.5f);   // whole pixels
                self->m_icons[d.idx].y = floorf(ny + 0.5f);
            }
            // Push-aside: whenever the pressed icon enters a new cell, sweep
            // every cell the group now covers — bystanders flow out of the
            // way mid-drag, along the row in the drag's direction.
            int hovered = self->CellOf(self->m_icons[self->m_dragIcon]);
            if (hovered != self->m_dragHoverCell) {
                FenceWindow::IconGrid g = self->GetIconGrid();
                int dc = (hovered % g.cols) - (self->m_dragHoverCell % g.cols);
                if (dc != 0) self->m_dragDirX = dc > 0 ? 1 : -1;
                self->m_dragHoverCell = hovered;
                self->PushGroupOut();
            }
            self->Redraw();
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!self) return 0;
        if (self->m_chevronDown) {
            // The press only armed the button — the fence toggles here, on
            // release, and only while the cursor is still over it. Capture
            // routed the moves to us, so re-derive the position from the
            // live cursor. ToggleCollapse redraws through WM_SIZE; the
            // cancelled branch repaints to drop the pressed plate.
            self->m_chevronDown = false;
            ReleaseCapture();
            POINT p; GetCursorPos(&p); ScreenToClient(hwnd, &p);
            int th = (int)self->m_render->Appearance().titleH;
            int arrowL = (int)(self->m_render->Width() - self->m_render->Appearance().titleH);
            bool inside = p.y >= 0 && p.y < th && p.x >= arrowL;
            self->m_chevronHover = inside;
            if (inside) self->ToggleCollapse();
            else        self->Redraw();
            return 0;
        }
        bool wasBusy = self->m_dragIcon >= 0 || self->m_moving ||
                       self->m_resizeMask || self->m_marquee;
        if (self->m_marquee) {
            // A press that never passed the drag threshold was a plain click
            // on empty ground — the selection was already cleared at press
            // time (additive mode left it alone). A real sweep keeps the
            // band's final selection. Either way: drop the band's state.
            self->m_marquee = false;
            self->m_marqueeMoved = false;
            self->m_marqueeAdd = false;
            self->m_marqueeBase.clear();
            self->Redraw();   // erase the rubber band
        }
        if (self->m_dragIcon >= 0) {
            // Mouse capture routed every move to the drag path, so the hover
            // plate is stale — re-derive it from the live cursor before the
            // settle redraw paints the final frame.
            POINT p; GetCursorPos(&p); ScreenToClient(hwnd, &p);
            self->m_hoverIcon = self->HitTestIcon(p.x, p.y);
            self->SettleDragGroup();
        }
        bool geomChanged = self->m_moving || self->m_resizeMask;
        self->m_moving = false;
        self->m_resizeMask = 0;
        if (wasBusy) ReleaseCapture();
        if (geomChanged) Config::MarkDirty();   // fence moved / resized
        return 0;
    }
    case WM_CAPTURECHANGED: {
        // Capture stolen mid-gesture: settle the group instead of leaving it
        // overlapping somebody, and drop any half-drawn rubber band. (Our own
        // ReleaseCapture in WM_LBUTTONUP gets here with the drag/marquee
        // state already cleared — no double settle.)
        if (self) {
            if (self->m_chevronDown) {   // press disarmed without a release
                self->m_chevronDown = false;
                self->Redraw();
            }
            if (self->m_dragIcon >= 0) self->SettleDragGroup();
            if (self->m_marquee) {
                self->m_marquee = false;
                self->m_marqueeMoved = false;
                self->m_marqueeAdd = false;
                self->m_marqueeBase.clear();
                self->Redraw();
            }
            if (self->m_moving || self->m_resizeMask) Config::MarkDirty();
            self->m_moving = false;
            self->m_resizeMask = 0;
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        // Cursor left the fence: drop the hover states. (While a capture is
        // active — drag/move/resize/chevron press — leave notifications are
        // suppressed.)
        if (self && self->m_dragIcon < 0 &&
            !self->m_moving && !self->m_resizeMask && !self->m_marquee) {
            bool dirty = false;
            if (self->m_hoverIcon >= 0) { self->m_hoverIcon = -1;       dirty = true; }
            if (self->m_chevronHover)   { self->m_chevronHover = false; dirty = true; }
            if (dirty) self->Redraw();
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        if (!self) return 0;
        int mx = GET_X_LPARAM(lp);
        int my = GET_Y_LPARAM(lp);
        if (my < (int)self->m_render->Appearance().titleH) {
            // Ignore the collapse-arrow box so it doesn't trigger a rename
            int arrowL = (int)(self->m_render->Width() - self->m_render->Appearance().titleH);
            if (mx >= arrowL) return 0;
            self->EnterRename();
            return 0;
        }
        // Double-click on an icon → open it
        int hit = self->HitTestIcon(mx, my);
        if (hit >= 0) {
            ShellExecuteW(hwnd, L"open", self->m_icons[hit].path.c_str(),
                          nullptr, nullptr, SW_SHOW);
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ClientToScreen(hwnd, &pt);
        POINT cli{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };

        // Detect which icon was clicked (same geometry as rendering)
        int iconHit = -1;
        if (self && !self->m_icons.empty())
            iconHit = self->HitTestIcon(cli.x, cli.y);

        // Right-click selects too, so the menu appears over the selection
        // plate exactly like the Explorer desktop. A press on an icon that is
        // ALREADY selected keeps the whole multi-selection (Explorer keeps the
        // group intact so the menu verbs apply to every member); otherwise
        // the selection collapses to the pressed icon.
        if (self && iconHit >= 0) {
            if (!self->IsPathSelected(self->m_icons[iconHit].path))
                self->SelectOnly(iconHit);
            self->m_hoverIcon = iconHit;
            self->Redraw();
        }

        // Remove icon `idx`, repairing selection/hover bookkeeping.
        auto eraseIcon = [&](int idx) {
            if (!self || idx < 0 || idx >= (int)self->m_icons.size()) return;
            // Drop the removed path from the selection set so no entry
            // dangles pointing at a dead icon.
            auto& sel = self->m_selectedPaths;
            sel.erase(std::remove(sel.begin(), sel.end(), self->m_icons[idx].path),
                      sel.end());
            self->m_icons.erase(self->m_icons.begin() + idx);
            if (self->m_hoverIcon == idx)      self->m_hoverIcon = -1;
            else if (self->m_hoverIcon > idx)  self->m_hoverIcon--;
            self->Redraw();
            Config::MarkDirty();
        };

        // Icon → show the real Explorer context menu (Open / Cut / Copy /
        // Delete / Properties …) with our fence-only verb appended.
        if (self && iconHit >= 0) {
            bool invoked = false, removeChosen = false;
            if (RunShellContextMenu(hwnd, self->m_icons[iconHit].path,
                                    self->m_icons[iconHit].name, pt,
                                    invoked, removeChosen)) {
                if (removeChosen) {
                    eraseIcon(iconHit);
                } else if (invoked) {
                    // If the shell command got rid of the file (e.g. Delete),
                    // don't leave a dangling icon behind in the fence.
                    if (GetFileAttributesW(self->m_icons[iconHit].path.c_str())
                            == INVALID_FILE_ATTRIBUTES)
                        eraseIcon(iconHit);
                }
                return 0;
            }
            // Shell menu unavailable — fall through to the fence menu below.
        }

        // Fence-level menu (background, or fallback when the shell menu fails)
        HMENU menu = CreatePopupMenu();
        if (iconHit >= 0 && self) {
            std::wstring rm = std::wstring(Loc(L"Remove", L"移除")) + L" \"" +
                              self->m_icons[iconHit].name + L"\"";
            AppendMenuW(menu, MF_STRING, 3, rm.c_str());
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        }

        AppendMenuW(menu, MF_STRING, 1, Loc(L"Rename", L"重命名"));
        AppendMenuW(menu, MF_STRING, 4, Loc(L"Appearance Settings...", L"外观设置..."));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 2, Loc(L"Delete Fence", L"删除围栏"));

        // Fluent glyph icons. The set owns the HBITMAPs until the menu
        // interaction below has fully returned (MIIM_BITMAP is by reference).
        GlyphBitmapSet glyphs;
        {
            int gsz = MenuGlyphSizePx();
            auto icon = [&](UINT id, MenuGlyph g) {
                HBITMAP b = glyphs.add(g, gsz);
                if (!b) return;
                MENUITEMINFOW mii = { sizeof(mii) };
                mii.fMask = MIIM_BITMAP;
                mii.hbmpItem = b;
                SetMenuItemInfoW(menu, id, FALSE, &mii);
            };
            if (iconHit >= 0) icon(3, MenuGlyph::Remove);
            icon(1, MenuGlyph::Pencil);    // Rename
            icon(4, MenuGlyph::Sliders);   // Appearance Settings
            icon(2, MenuGlyph::Trash);     // Delete Fence
        }

        SetForegroundWindow(hwnd);
        // Prefer the Win11 Fluent rendering; -1 means it could not be built
        // (empty model / D2D failure) → classic menu, the same fallback the
        // shell icon menu uses. The Fluent loop swallows the dismiss click,
        // which is exactly what a context menu must do over a surface where
        // a stray press would otherwise start a fence drag.
        int cmd = FluentMenu::Run(hwnd, menu, pt);
        if (cmd < 0)
            cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);

        if (self) {
            switch (cmd) {
            case 1: self->EnterRename(); break;
            case 2:
                if (s_owner)
                    PostMessage(s_owner, WM_FENCE_DELETE, (WPARAM)hwnd, 0);
                break;
            case 3: eraseIcon(iconHit); break;
            case 4: SettingsPanel::Run(self); break;
            }
        }
        return 0;
    }

    case WM_CHAR:
        if (self && self->m_renaming && wp >= 32) {
            self->m_renameBuf += (wchar_t)wp;
            self->Redraw();
        }
        return 0;

    case WM_KEYDOWN:
        if (self && self->m_renaming) {
            if (wp == VK_RETURN)  { self->ExitRename(true); }
            if (wp == VK_ESCAPE)  { self->ExitRename(false); }
            if (wp == VK_BACK && !self->m_renameBuf.empty()) {
                self->m_renameBuf.pop_back();
                self->Redraw();
            }
        }
        return 0;

    case WM_KILLFOCUS:
        if (self) self->ExitRename(false);
        return 0;

    case WM_TIMER:
        if (!self) return 0;
        if (wp == 1 && self->m_renaming) {
            self->m_cursorVisible = !self->m_cursorVisible;
            self->Redraw();
        } else if (wp == 2) {
            KillTimer(hwnd, 2);
            self->CaptureBackdrop();
        }
        return 0;

    case WM_SHOWWINDOW:
        // Win+D ("Show desktop") hides every top-level window that is not
        // part of the shell's desktop tree. A fence belongs *on* the desktop,
        // so resist any hide we did not cause ourselves (m_appHidden is set
        // by Hide()). The re-show is posted, not done in-place: by the time
        // the posted message runs, the hide sweep has finished, so the fence
        // cannot be hidden a second time.
        // (lp != 0 means the hide came from a parent state change — as a
        // desktop child we get those legitimately; only fight direct hides.)
        if (self && !wp && !lp && !self->m_appHidden)
            PostMessage(hwnd, WM_FENCE_RESHOW, 0, 0);
        return 0;

    case WM_FENCE_RESHOW:
        if (self && !self->m_appHidden && !IsWindowVisible(hwnd))
            ShowWindow(hwnd, SW_SHOWNA);   // don't steal focus from the desktop
        return 0;

    case WM_SYSCOMMAND:
        // Win+M minimizes every window; a toolwindow fence has no taskbar
        // button to bring it back, so simply refuse to minimize.
        if ((wp & 0xFFF0) == SC_MINIMIZE) return 0;
        return DefWindowProc(hwnd, msg, wp, lp);

    case WM_DESTROY:
        if (self) { SetWindowLongPtr(hwnd, GWLP_USERDATA, 0); }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}
