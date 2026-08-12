#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <atlbase.h>
#include <string>
#include <vector>
#include <memory>
#include "render.h"

class FenceWindow;

/// Unified, self-drawn appearance settings panel (dark Fluent).
///
/// One instance at a time, opened from a fence's right-click menu. It runs a
/// nested blocking message loop so the call site (the fence's WM_RBUTTONUP
/// handler) does not return until the panel is dismissed; this keeps the
/// fence alive for the whole lifetime of the panel. All edits apply to the
/// fence live (RebuildStyles + Redraw).
class SettingsPanel {
public:
    /// Open the panel for `fence`. If a panel is already open it is simply
    /// brought to the front. Blocks until the panel is closed.
    static void Run(FenceWindow* fence);
    /// Defensive close used by ~FenceWindow: if the active panel belongs to
    /// `fence`, tear it down so it never references a destroyed fence.
    static void CloseActiveFor(const FenceWindow* fence);

private:
    explicit SettingsPanel(FenceWindow* fence);
    ~SettingsPanel();
    SettingsPanel(const SettingsPanel&) = delete;
    SettingsPanel& operator=(const SettingsPanel&) = delete;

    // One layout pass fills geometry + hit rects; drawing and input both read
    // the same members so they can never drift apart.
    void ComputeLayout();
    void DrawPanel();
    void Repaint();
    void ApplyToFence();
    void CreatePanelBrushes();
    void CreatePanelFormats();
    void LoadFonts();

    // Input
    int  HitTest(int mx, int my) const;
    void OnLButtonDown(int mx, int my);
    void OnMouseMove(int mx, int my);
    void OnLButtonUp(int mx, int my);
    void OnMouseWheel(int delta, int sx, int sy);
    void PickFont(int idx);
    void SetSizeFromX(int mx);
    void SetOpacityFromX(int mx);
    void SetSizeValue(int v);
    void SetOpacityValue(int v);
    void ScrollFontList(int deltaRows);
    void AdjustTitleH(float deltaDesign);
    void SetPanelColor(int idx);
    void SetTitleColor(int idx);
    void ChoosePanelColor();
    void ChooseTitleColor();

    void RunLoop();

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static const wchar_t* Loc(const wchar_t* en, const wchar_t* zh);

    static SettingsPanel* s_active;

    FenceWindow* m_fence = nullptr;
    HWND m_fenceHwnd = nullptr;   // captured at open for safe late deletion
    HWND m_hwnd = nullptr;
    std::unique_ptr<RenderContext> m_render;

    int m_x = 0, m_y = 0;
    int m_panelW = 0, m_panelH = 0;
    float m_s = 1.0f;             // DPI scale factor

    bool m_closing = false;
    bool m_deleteFence = false;

    // Font list
    std::vector<std::wstring> m_fonts;
    int m_fontScrollTop = 0;      // index of the first visible row
    int m_fontRows = 5;           // visible rows (may shrink on small screens)
    int m_selFont = -1;           // index of the currently selected font

    // Interaction state
    int  m_dragSlider = 0;        // 0 none, 1 font size, 2 opacity
    bool m_scrollDrag = false;
    int  m_scrollDragStartY = 0;
    int  m_scrollDragStartTop = 0;
    int  m_hover = 0;
    int  m_hoverRow = -1;         // font row under the cursor (list only)

    // Geometry (physical px), rebuilt by ComputeLayout
    RECT m_closeBtn = {};
    RECT m_fontList = {};         // viewport showing the font rows
    RECT m_fontScroll = {};       // scrollbar track
    RECT m_sizeSlider = {};
    RECT m_opSlider = {};
    RECT m_panelSwatch[8] = {};
    RECT m_titleSwatch[8] = {};
    RECT m_panelMore = {};
    RECT m_titleMore = {};
    RECT m_thMinus = {};
    RECT m_thPlus = {};
    RECT m_titlePosSeg[3] = {};   // title alignment: left / center / right
    RECT m_displayModeSeg[2] = {}; // display mode: grid / list
    RECT m_deleteBtn = {};
    float m_rowH = 26.0f;
    // Label-band Y positions for drawing (set by ComputeLayout)
    float m_fontLblY = 0, m_sizeLblY = 0, m_opLblY = 0, m_panelLblY = 0,
          m_titleLblY = 0, m_titlePosLblY = 0, m_thLblY = 0, m_sepY = 0,
          m_displayModeLblY = 0;

    struct Hit { int id; RECT rc; };
    std::vector<Hit> m_hits;

    // Panel-local palette (independent of the fence appearance)
    CComPtr<ID2D1SolidColorBrush> m_brTint, m_brBorder, m_brHighlight, m_brText,
        m_brText2, m_brAccent, m_brHover, m_brSel, m_brDanger, m_brTrack,
        m_brThumb, m_brField, m_brSwatchRing;
    // Fixed preset swatches are cached once (not rebuilt on every redraw).
    CComPtr<ID2D1SolidColorBrush> m_brPanelSw[8], m_brTitleSw[8];
    CComPtr<IDWriteTextFormat> m_fmtHeader, m_fmtLabel, m_fmtValue, m_fmtSymbol,
        m_fmtCenter;
};
