#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include "render.h"

#define WM_FENCE_DELETE (WM_APP + 100)
#define WM_FENCE_RESHOW (WM_APP + 101)   // posted to undo an external hide (Win+D)

struct FenceData {
    std::wstring id, title;
    int x, y, w, h;
    bool visible = true;
};

class FenceWindow {
public:
    FenceWindow(const FenceData& data);
    ~FenceWindow();

    HWND Hwnd() const { return m_hwnd; }
    void Show();
    void Hide();
    void Redraw();
    void SetIcons(const std::vector<IconEntry>& entries);
    // atX/atY = desired cell *center* (drop point); the icon snaps to the
    // nearest free cell of the icon grid
    void AddIcon(const std::wstring& name, const std::wstring& path, float atX, float atY);
    /// Re-snap every icon onto the icon grid and resolve overlaps. Call after
    /// anything that moves the grid (resize, title-bar height change).
    void RelayoutIcons();
    /// Follow a desktop icon-size change (polled by the owner window, which
    /// measures the live size once per tick via QueryDesktopIconSizePx and
    /// passes it to every fence): adopt the new glyph/cell metrics, re-seat
    /// the icons, repaint. Skipped while a drag/move/resize gesture is live
    /// so it is never fought mid-gesture; the fence catches up on the next
    /// poll once the gesture ends.
    void SyncDesktopIconSize(int physicalPx);
    /// Debounced frosted-glass backdrop re-capture (~150 ms, on the fence's
    /// own timer-2). The owner window fans this out on WM_DISPLAYCHANGE /
    /// WM_SETTINGCHANGE: those broadcasts only reach top-level windows, and
    /// fences are children of explorer's icon list, so they never see them.
    /// Safe to call repeatedly — SetTimer restarts the countdown.
    void ScheduleBackdropRefresh();
    void SetDragOver(bool over);
    FenceData GetData() const;
    RenderContext& GetRender() { return *m_render; }
    const std::vector<IconEntry>& Icons() const { return m_icons; }

    void EnterRename();
    void ExitRename(bool commit);

    void ToggleCollapse();
    bool IsCollapsed() const { return m_collapsed; }
    /// While collapsed, re-apply the window height so a live-edited title bar
    /// height shows immediately (no-op when expanded).
    void SyncCollapsedSize();

    static void RegisterClass(HINSTANCE hInst);
    /// The desktop window fences are parented to: Progman, or the WorkerW
    /// that hosts SHELLDLL_DefView. Being a child of the desktop tree is
    /// what keeps fences visible through Win+D (the shell only hides
    /// foreign top-level windows). Null when explorer is not up yet.
    static HWND DesktopHost();
    static void SetOwner(HWND owner) { s_owner = owner; }
    static HWND Owner() { return s_owner; }
    static void SetLanguage(int lang) { s_lang = lang; }
    static int  GetLanguage() { return s_lang; }
    static const wchar_t* Loc(const wchar_t* en, const wchar_t* zh);
    static const wchar_t* ClassName() { return L"OpenFencesFence"; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void CaptureBackdrop();   // re-snapshot the desktop behind the fence
    int  HitTestIcon(int mx, int my) const;   // → icon index or -1
    void ClampIconPos(float& x, float& y) const;

    // ── Icon grid (desktop-style snap-to-grid placement) ──
    // The fence body is tiled with cellW×cellH cells; icons always rest on a
    // cell. Placement is free (any cell), but positions snap on release and
    // cells never hold two icons — same rules as the Windows desktop.
    struct IconGrid { int cols, rows; float x0, y0, cw, ch; };
    IconGrid GetIconGrid() const;                 // grid metrics for this fence
    int  CellOf(const IconEntry& e) const;        // nearest cell index (r*cols+c)
    void PlaceIconAt(IconEntry& e, int cell) const;   // seat on the cell top-left
    int  FindFreeCell(int prefer, const std::vector<int>& occupied) const;
    // Same-row-first, drag-direction-first variant used when pushing a cell's
    // occupant out of the way (desktop icons flow sideways along the row).
    int  FindPushCell(int prefer, const std::vector<int>& occupied, int dirX) const;
    std::vector<int> OccupiedCells(const std::vector<int>& excludes) const;
    void PushOccupantOf(int cell, const std::vector<int>& excludes, int dirX = 0);
    void PushGroupOut();             // clear bystanders from every cell the
                                     // dragged group currently covers
    void SettleDragGroup();          // snap every member + end the drag
    static HWND s_owner;
    static int s_lang;

    HWND m_hwnd = nullptr;
    std::unique_ptr<RenderContext> m_render;
    std::wstring m_title, m_id;
    std::vector<IconEntry> m_icons;
    int m_x = 0, m_y = 0;

    bool m_dragOver = false;
    // True while the app itself has hidden the fence (tray "hide all").
    // External hides (Win+D) leave it false and are undone via WM_FENCE_RESHOW.
    bool m_appHidden = false;

    // In-fence icon dragging (mouse capture based). The press picks up the
    // WHOLE selection: one icon when the press collapsed the selection to a
    // single icon, a group when it landed on a member of a marquee
    // multi-selection. The group follows the cursor by one shared delta;
    // bystanders are pushed aside live and every member snaps to its own
    // free cell on release.
    int   m_dragIcon = -1;        // the pressed icon (gesture sentinel)
    int   m_dragHoverCell = -1;   // grid cell currently under the pressed icon
    int   m_dragDirX = 0;         // last horizontal drag direction (-1 / +1);
                                  // pushed occupants flow that way along the row
    struct DragStart { int idx; float x, y; };   // per-member lift-off state;
    std::vector<DragStart> m_dragGroup;          // indexes stay valid for the
                                                 // whole gesture (no reorders
                                                 // happen mid-drag)
    std::vector<int> m_dragExcludes;             // group indexes, for occupancy
    bool  m_dragMoved = false;    // cursor passed the system drag threshold
    int   m_dragGrabX = 0, m_dragGrabY = 0;   // LBUTTONDOWN point (client)

    // Explorer-style icon states. Hover is an index; the selection is a set
    // of paths (keyed, not indexed, so it survives the bring-to-front
    // reorder a drag performs).
    int m_hoverIcon = -1;
    std::vector<std::wstring> m_selectedPaths;
    bool IsPathSelected(const std::wstring& path) const;
    void ClearSelection();
    void SelectOnly(int idx);     // selection = { icon }; idx < 0 clears
    int  SelectedIndex() const;   // first selected icon's index, or -1
    void BringSelectedToFront();  // draw order: selected icons on top

    // Marquee (left-drag rubber band) multi-select, desktop style: the band
    // selects every icon whose cell it touches, live while it sweeps. Ctrl
    // at press time keeps the current selection and adds to it instead of
    // replacing it; a press that never passes the drag threshold is a plain
    // click and only clears the selection.
    bool m_marquee = false;
    bool m_marqueeMoved = false;   // cursor passed the system drag threshold
    bool m_marqueeAdd = false;     // Ctrl held at press → additive
    int  m_marqAX = 0, m_marqAY = 0;   // anchor (client)
    int  m_marqCX = 0, m_marqCY = 0;   // live cursor (client)
    std::vector<std::wstring> m_marqueeBase;   // selection at press (additive)
    RECT MarqueeRect() const;      // normalized, clamped to the fence body
    void UpdateMarqueeSelection();

    // Manual move / resize. A desktop-child window cannot ride DefWindowProc's
    // HTCAPTION drag or edge-size loops (those only work for top-level
    // windows), so both are reimplemented with mouse capture.
    bool m_moving = false;
    int  m_moveOffX = 0, m_moveOffY = 0;    // cursor offset from the origin
    int  m_resizeMask = 0;                  // 1=left 2=right 4=top 8=bottom
    int  m_resizeHT = 0;                    // HT* code → sizing cursor while dragging
    RECT m_resizeStart = {};                // rect at drag start (parent-client)
    int  m_resizePtX = 0, m_resizePtY = 0;  // cursor at drag start (parent-client)

    // Rename focus: while renaming we attach our input queue to the
    // foreground (explorer) thread so SetFocus can reach this desktop child.
    // 0 = not attached; otherwise the thread id we attached to.
    DWORD m_focusAttached = 0;

    bool m_renaming = false;
    std::wstring m_renameBuf;
    bool m_cursorVisible = false;
    UINT_PTR m_cursorTimer = 0;

    // Collapse / expand (arrow in the title bar's top-right corner).
    // Caption-button semantics: the press only ARMS the button (plate shows);
    // the fence toggles on release, and only while the cursor is still over
    // the button — same as real window caption buttons.
    bool m_collapsed = false;
    bool m_chevronHover = false;
    bool m_chevronDown = false;
    int  m_expandedH = 0;       // full height remembered while collapsed
    // Icon metrics changed while collapsed: the grid cannot be rebuilt with
    // the window folded (its height is just the title bar), so the relayout
    // is deferred until the fence expands again.
    bool m_relayoutPending = false;
};
