#include "settings_panel.h"
#include "fence_window.h"
#include "config.h"
#include <windowsx.h>
#include <commdlg.h>
#include <algorithm>
#include <cmath>

// ── Hit-test ids ──
// Layout and input share one geometry pass (ComputeLayout fills m_hits), so
// what you see is exactly what you click.
enum : int {
    HIT_NONE = 0,
    HIT_CLOSE, HIT_FONT_LIST, HIT_FONT_SCROLL,
    HIT_SIZE_SLIDER, HIT_OPACITY_SLIDER,
    HIT_PANEL_SWATCH0 = 100,          // +0..7
    HIT_TITLE_SWATCH0 = 120,          // +0..7
    HIT_PANEL_MORE = 140, HIT_TITLE_MORE = 141,
    HIT_TITLEH_MINUS, HIT_TITLEH_PLUS,
    HIT_TITLEPOS0 = 160,           // +0 left, +1 center, +2 right
    HIT_DISPMODE0 = 180,           // +0 grid, +1 list
    HIT_DELETE_FENCE = 200,
};

// Dark preset tints (panel body / title bar). The first entry of each row is
// the fence default, so the current look always shows as selected on open.
static const float kPanelColors[8][3] = {
    {0.08f,0.08f,0.10f}, {0.10f,0.10f,0.12f}, {0.13f,0.13f,0.15f}, {0.05f,0.05f,0.06f},
    {0.10f,0.12f,0.18f}, {0.12f,0.10f,0.16f}, {0.09f,0.13f,0.11f}, {0.15f,0.11f,0.09f},
};
static const float kTitleColors[8][3] = {
    {0.05f,0.05f,0.07f}, {0.07f,0.07f,0.09f}, {0.10f,0.10f,0.12f}, {0.03f,0.03f,0.04f},
    {0.07f,0.10f,0.16f}, {0.10f,0.08f,0.14f}, {0.07f,0.11f,0.09f}, {0.13f,0.09f,0.07f},
};

static int   clampi(int v, int lo, int hi)   { return v < lo ? lo : (v > hi ? hi : v); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static float DpiScale() {
    HDC h = GetDC(nullptr);
    int d = GetDeviceCaps(h, LOGPIXELSX);
    ReleaseDC(nullptr, h);
    return d / 96.0f;
}

SettingsPanel* SettingsPanel::s_active = nullptr;

const wchar_t* SettingsPanel::Loc(const wchar_t* en, const wchar_t* zh) {
    return FenceWindow::GetLanguage() == 1 ? zh : en;
}

// ── Font enumeration ──

static int CALLBACK EnumFontFamProc(const LOGFONTW* lf, const TEXTMETRICW*,
                                    DWORD, LPARAM lp) {
    auto* v = (std::vector<std::wstring>*)lp;
    if (v->size() >= 1024) return 0;            // bounded; stop enumerating
    if (lf->lfFaceName[0] == L'@') return 1;    // skip vertical (@) faces
    v->push_back(lf->lfFaceName);
    return 1;
}

// ── Lifetime ──

void SettingsPanel::Run(FenceWindow* fence) {
    if (!fence) return;
    if (s_active) {                             // single instance: just raise
        if (s_active->m_hwnd) SetForegroundWindow(s_active->m_hwnd);
        return;
    }
    bool doDelete = false;
    HWND fenceHwnd = nullptr;
    {
        SettingsPanel panel(fence);
        panel.RunLoop();
        doDelete = panel.m_deleteFence;
        fenceHwnd = panel.m_fenceHwnd;
    }   // panel fully destroyed here before the delete is posted
    if (doDelete && fenceHwnd)
        PostMessageW(FenceWindow::Owner(), WM_FENCE_DELETE, (WPARAM)fenceHwnd, 0);
}

void SettingsPanel::CloseActiveFor(const FenceWindow* fence) {
    if (s_active && s_active->m_fence == fence) {
        s_active->m_closing = true;
        if (s_active->m_hwnd) DestroyWindow(s_active->m_hwnd);
        s_active->m_hwnd = nullptr;
    }
}

SettingsPanel::SettingsPanel(FenceWindow* fence) : m_fence(fence) {
    m_fenceHwnd = fence->Hwnd();
    m_s = DpiScale();
    s_active = this;

    static bool clsReg = false;
    if (!clsReg) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"OpenFencesSettings";
        RegisterClassExW(&wc);
        clsReg = true;
    }

    // Size + position. The panel tries the fence's right, falls back to the
    // left, and is clamped to the monitor work area. If it still does not
    // fit vertically, the font-list viewport shrinks (min 3 visible rows).
    HMONITOR mon = MonitorFromWindow(m_fenceHwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    int waH = mi.rcWork.bottom - mi.rcWork.top;

    m_fontRows = 5;
    auto heightFor = [&](int rows) { return (int)((527 + rows * 26) * m_s); };
    m_panelH = heightFor(m_fontRows);
    while (m_panelH > waH && m_fontRows > 3) { m_fontRows--; m_panelH = heightFor(m_fontRows); }
    m_panelW = (int)(360 * m_s);

    RECT frc; GetWindowRect(m_fenceHwnd, &frc);
    int x = frc.right + 12;
    if (x + m_panelW > mi.rcWork.right) x = frc.left - 12 - m_panelW;
    x = clampi(x, mi.rcWork.left, (std::max)(mi.rcWork.left, mi.rcWork.right - m_panelW));
    int y = clampi(frc.top, mi.rcWork.top, (std::max)(mi.rcWork.top, mi.rcWork.bottom - m_panelH));
    m_x = x; m_y = y;

    m_render = std::make_unique<RenderContext>(m_panelW, m_panelH);
    CreatePanelBrushes();
    CreatePanelFormats();
    LoadFonts();

    m_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        L"OpenFencesSettings", L"Settings", WS_POPUP,
        m_x, m_y, m_panelW, m_panelH,
        nullptr, nullptr, GetModuleHandleW(nullptr), this);
    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, (LONG_PTR)this);

    // Draw + Present before ShowWindow so the first visible frame is clean.
    Repaint();
    ShowWindow(m_hwnd, SW_SHOW);
    SetForegroundWindow(m_hwnd);
    SetFocus(m_hwnd);
}

SettingsPanel::~SettingsPanel() {
    if (s_active == this) s_active = nullptr;
    if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
}

// ── Nested blocking message loop ──

static bool IsOutsideButtonDown(UINT m) {
    switch (m) {
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDBLCLK: case WM_XBUTTONDBLCLK:
    case WM_NCLBUTTONDOWN: case WM_NCRBUTTONDOWN: case WM_NCMBUTTONDOWN:
    case WM_NCXBUTTONDOWN: case WM_NCLBUTTONDBLCLK: case WM_NCRBUTTONDBLCLK:
    case WM_NCMBUTTONDBLCLK: case WM_NCXBUTTONDBLCLK:
        return true;
    }
    return false;
}

void SettingsPanel::RunLoop() {
    MSG msg;
    while (!m_closing) {
        BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0) {                     // WM_QUIT: re-post and bail out
            PostQuitMessage((int)msg.wParam);
            break;
        }
        if (got == -1) continue;

        // Any button press outside the panel closes it, and the message is
        // swallowed so it cannot leak through to a fence (drag / menu).
        if (IsOutsideButtonDown(msg.message) && msg.hwnd != m_hwnd) {
            m_closing = true;
            break;
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (m_hwnd && !IsWindow(m_hwnd)) break;
    }
}

// ── Panel palette (brushes + text formats) ──

void SettingsPanel::CreatePanelBrushes() {
    auto* t = m_render->Target();
    auto mk = [&](float r, float g, float b, float a, ID2D1SolidColorBrush** out) {
        t->CreateSolidColorBrush(D2D1::ColorF(r, g, b, a), out);
    };
    mk(0.125f, 0.125f, 0.14f, 1.0f, &m_brTint);        // solid surface (no translucency)
    mk(1, 1, 1, 0.08f, &m_brBorder);
    mk(1, 1, 1, 0.05f, &m_brHighlight);
    mk(1, 1, 1, 0.92f, &m_brText);
    mk(1, 1, 1, 0.55f, &m_brText2);
    mk(0.30f, 0.76f, 1.0f, 1.0f, &m_brAccent);         // #4CC2FF
    mk(1, 1, 1, 0.06f, &m_brHover);
    mk(0.30f, 0.76f, 1.0f, 0.22f, &m_brSel);
    mk(0.98f, 0.28f, 0.24f, 1.0f, &m_brDanger);
    mk(1, 1, 1, 0.35f, &m_brTrack);
    mk(1, 1, 1, 0.92f, &m_brThumb);
    mk(1, 1, 1, 0.05f, &m_brField);                    // input wells / buttons
    mk(0.30f, 0.76f, 1.0f, 0.90f, &m_brSwatchRing);
    // Preset color swatches: build once, reuse on every redraw.
    for (int i = 0; i < 8; i++) {
        mk(kPanelColors[i][0], kPanelColors[i][1], kPanelColors[i][2], 1.0f, &m_brPanelSw[i]);
        mk(kTitleColors[i][0], kTitleColors[i][1], kTitleColors[i][2], 1.0f, &m_brTitleSw[i]);
    }
}

void SettingsPanel::CreatePanelFormats() {
    auto* dw = m_render->WriteFactory();
    float s = m_s;
    auto mk = [&](float px, DWRITE_FONT_WEIGHT w, IDWriteTextFormat** out) {
        dw->CreateTextFormat(L"Segoe UI", nullptr, w, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, px, L"en-US", out);
    };
    mk(15 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD, &m_fmtHeader);
    mk(13 * s, DWRITE_FONT_WEIGHT_NORMAL, &m_fmtLabel);
    mk(13 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD, &m_fmtValue);
    mk(14 * s, DWRITE_FONT_WEIGHT_NORMAL, &m_fmtSymbol);
    mk(13 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD, &m_fmtCenter);

    if (m_fmtHeader) m_fmtHeader->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (m_fmtLabel)  m_fmtLabel->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (m_fmtSymbol) {
        m_fmtSymbol->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_fmtSymbol->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (m_fmtCenter) {
        m_fmtCenter->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_fmtCenter->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (m_fmtValue) {
        m_fmtValue->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_fmtValue->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
}

void SettingsPanel::LoadFonts() {
    HDC hdc = GetDC(nullptr);
    LOGFONTW lf = {};
    lf.lfCharSet = DEFAULT_CHARSET;
    EnumFontFamiliesExW(hdc, &lf, EnumFontFamProc, (LPARAM)&m_fonts, 0);
    ReleaseDC(nullptr, hdc);

    // Enumeration reports each charset variant; dedupe by face name.
    std::sort(m_fonts.begin(), m_fonts.end());
    m_fonts.erase(std::unique(m_fonts.begin(), m_fonts.end()), m_fonts.end());

    const auto& app = m_fence->GetRender().Appearance();
    for (int i = 0; i < (int)m_fonts.size(); i++) {
        if (lstrcmpiW(m_fonts[i].c_str(), app.fontName) == 0) { m_selFont = i; break; }
    }
    if (m_selFont >= 0) {   // bring the current font into view
        m_fontScrollTop = clampi(m_selFont, 0,
            (std::max)(0, (int)m_fonts.size() - m_fontRows));
    }
}

// ── Layout (single pass → geometry + hit rects) ──

void SettingsPanel::ComputeLayout() {
    float s = m_s;
    m_hits.clear();
    m_hits.reserve(32);

    auto R = [&](float x, float y, float w, float h) {
        return RECT{ (LONG)x, (LONG)y, (LONG)(x + w), (LONG)(y + h) };
    };
    auto addHit = [&](int id, RECT rc) { m_hits.push_back({id, rc}); };

    float W = (float)m_panelW;
    float cx = 20 * s;                 // content left
    float cw = 320 * s;                // content width
    m_rowH = 26 * s;

    // Header
    m_closeBtn = R(W - 40 * s, 6 * s, 32 * s, 32 * s);
    addHit(HIT_CLOSE, m_closeBtn);

    // Font label + list viewport + scrollbar
    m_fontLblY = 52 * s;
    float listTop = 70 * s;
    float sbW = 8 * s, gap = 4 * s;
    float listW = cw - sbW - gap;
    float listH = m_fontRows * m_rowH;
    m_fontList = R(cx, listTop, listW, listH);
    addHit(HIT_FONT_LIST, m_fontList);
    m_fontScroll = R(cx + listW + gap, listTop, sbW, listH);
    addHit(HIT_FONT_SCROLL, m_fontScroll);

    float listBot = listTop + listH;

    // Font size
    m_sizeLblY = listBot + 12 * s;
    m_sizeSlider = R(cx, listBot + 30 * s, cw, 26 * s);
    addHit(HIT_SIZE_SLIDER, m_sizeSlider);

    // Opacity
    m_opLblY = listBot + 66 * s;
    m_opSlider = R(cx, listBot + 84 * s, cw, 26 * s);
    addHit(HIT_OPACITY_SLIDER, m_opSlider);

    // Panel color swatches
    m_panelLblY = listBot + 122 * s;
    float sw = 26 * s, swGap = 6 * s;
    float swY = listBot + 140 * s;
    for (int i = 0; i < 8; i++) {
        m_panelSwatch[i] = R(cx + i * (sw + swGap), swY, sw, sw);
        addHit(HIT_PANEL_SWATCH0 + i, m_panelSwatch[i]);
    }
    m_panelMore = R(cx + 8 * (sw + swGap), swY, sw, sw);
    addHit(HIT_PANEL_MORE, m_panelMore);

    // Title bar color swatches
    m_titleLblY = listBot + 176 * s;
    float twY = listBot + 194 * s;
    for (int i = 0; i < 8; i++) {
        m_titleSwatch[i] = R(cx + i * (sw + swGap), twY, sw, sw);
        addHit(HIT_TITLE_SWATCH0 + i, m_titleSwatch[i]);
    }
    m_titleMore = R(cx + 8 * (sw + swGap), twY, sw, sw);
    addHit(HIT_TITLE_MORE, m_titleMore);

    // Title name position (left / center / right) — 3-way segmented control
    m_titlePosLblY = listBot + 232 * s;
    float segW = (cw - 8 * s) / 3, tpY = listBot + 250 * s;
    for (int i = 0; i < 3; i++) {
        m_titlePosSeg[i] = R(cx + i * (segW + 4 * s), tpY, segW, 26 * s);
        addHit(HIT_TITLEPOS0 + i, m_titlePosSeg[i]);
    }

    // Title bar height stepper
    m_thLblY = listBot + 288 * s;
    float btn = 26 * s, stY = listBot + 306 * s;
    m_thMinus = R(cx, stY, btn, btn);
    addHit(HIT_TITLEH_MINUS, m_thMinus);
    m_thPlus = R(cx + cw - btn, stY, btn, btn);
    addHit(HIT_TITLEH_PLUS, m_thPlus);

    // Display mode (grid / list) — 2-way segmented control
    m_displayModeLblY = listBot + 344 * s;
    float dmSegW = (cw - 4 * s) / 2, dmY = listBot + 362 * s;
    for (int i = 0; i < 2; i++) {
        m_displayModeSeg[i] = R(cx + i * (dmSegW + 4 * s), dmY, dmSegW, 26 * s);
        addHit(HIT_DISPMODE0 + i, m_displayModeSeg[i]);
    }

    // Separator + delete (language lives in the tray menu, not here)
    m_sepY = listBot + 400 * s;
    m_deleteBtn = R(cx, listBot + 412 * s, cw, 30 * s);
    addHit(HIT_DELETE_FENCE, m_deleteBtn);
}

// ── Drawing ──

void SettingsPanel::DrawPanel() {
    ComputeLayout();
    auto* t = m_render->Target();
    if (!t) return;
    m_render->BeginDraw();

    float s = m_s;
    float W = (float)m_panelW, H = (float)m_panelH;
    float r = 12 * s;

    auto RF = [](const RECT& rc) {
        return D2D1_RECT_F{(float)rc.left, (float)rc.top, (float)rc.right, (float)rc.bottom};
    };
    auto fillRR = [&](const RECT& rc, float rad, ID2D1Brush* b) {
        t->FillRoundedRectangle(D2D1_ROUNDED_RECT{RF(rc), rad, rad}, b);
    };
    auto drawStr = [&](const std::wstring& str, const RECT& rc, IDWriteTextFormat* f, ID2D1Brush* b) {
        t->DrawText(str.c_str(), (UINT32)str.size(), f, RF(rc), b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    // Surface: solid dark fill (deliberately NOT frosted glass — the fence
    // owns that effect; a settings panel reads better opaque), rounded,
    // bordered
    t->Clear(D2D1::ColorF(0, 0, 0, 0));
    D2D1_ROUNDED_RECT rr{{0, 0, W, H}, r, r};
    t->FillRoundedRectangle(rr, m_brTint.p);
    D2D1_RECT_F hl{0, 0, W, 1.5f * s};
    t->FillRectangle(hl, m_brHighlight.p);
    t->DrawRoundedRectangle(rr, m_brBorder.p, 1.0f);

    // Header
    drawStr(Loc(L"Appearance", L"外观设置"),
        RECT{ (LONG)(20 * s), 0, (LONG)(W - 40 * s), (LONG)(44 * s) },
        m_fmtHeader.p, m_brText.p);
    if (m_hover == HIT_CLOSE)
        fillRR(m_closeBtn, 6 * s, m_brHover.p);
    drawStr(L"\u2715", m_closeBtn, m_fmtSymbol.p, m_brText2.p);

    auto drawLabel = [&](const std::wstring& str, float y) {
        t->DrawText(str.c_str(), (UINT32)str.size(), m_fmtLabel.p,
            D2D1_RECT_F{20 * s, y, 220 * s, y + 18 * s}, m_brText2.p);
    };
    auto drawValueR = [&](const std::wstring& str, float y) {
        t->DrawText(str.c_str(), (UINT32)str.size(), m_fmtValue.p,
            D2D1_RECT_F{W - 100 * s, y, W - 20 * s, y + 18 * s}, m_brText.p);
    };

    // ── Font list ──
    drawLabel(Loc(L"Font", L"字体"), m_fontLblY);
    fillRR(m_fontList, 6 * s, m_brField.p);
    int total = (int)m_fonts.size();
    for (int row = 0; row < m_fontRows; row++) {
        int idx = m_fontScrollTop + row;
        if (idx >= total) break;
        float ry = (float)m_fontList.top + row * m_rowH;
        RECT rowR{ m_fontList.left + 2, (LONG)ry, m_fontList.right - 2, (LONG)(ry + m_rowH) };
        bool sel = (idx == m_selFont);
        bool hov = (m_hover == HIT_FONT_LIST && m_hoverRow == row);
        if (sel)      fillRR(rowR, 5 * s, m_brSel.p);
        else if (hov) fillRR(rowR, 5 * s, m_brHover.p);

        // WYSIWYG: render each font name in its own face
        CComPtr<IDWriteTextFormat> rf;
        m_render->WriteFactory()->CreateTextFormat(m_fonts[idx].c_str(), nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 13 * s, L"en-US", &rf);
        if (rf) {
            rf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            RECT tr{ rowR.left + 8, rowR.top, rowR.right - 8, rowR.bottom };
            t->DrawText(m_fonts[idx].c_str(), (UINT32)m_fonts[idx].size(), rf.p,
                RF(tr), sel ? m_brText.p : m_brText2.p, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    // Scrollbar
    fillRR(m_fontScroll, 3 * s, m_brField.p);
    if (total > m_fontRows) {
        float trackH = (float)(m_fontScroll.bottom - m_fontScroll.top);
        float thumbH = (std::max)(20 * s, trackH * m_fontRows / total);
        float range = (float)(total - m_fontRows);
        float pos = range > 0 ? (m_fontScrollTop / range) : 0.0f;
        float thumbTop = (float)m_fontScroll.top + pos * (trackH - thumbH);
        RECT thR{ m_fontScroll.left, (LONG)thumbTop, m_fontScroll.right, (LONG)(thumbTop + thumbH) };
        fillRR(thR, 3 * s, m_brTrack.p);
    }

    // ── Sliders ──
    const auto& app = m_fence->GetRender().Appearance();
    int sizeVal = (int)(app.fontSize / s + 0.5f);
    int opVal = (int)(app.bg[3] * 100 + 0.5f);

    auto drawSlider = [&](const RECT& rc, float t01) {
        float cy = (rc.top + rc.bottom) / 2.0f;
        float x0 = rc.left + 9 * s, x1 = rc.right - 9 * s;
        D2D1_RECT_F track{ x0, cy - 2 * s, x1, cy + 2 * s };
        t->FillRoundedRectangle(D2D1_ROUNDED_RECT{track, 2 * s, 2 * s}, m_brField.p);
        float fx = x0 + clampf(t01, 0, 1) * (x1 - x0);
        D2D1_RECT_F filled{ x0, cy - 2 * s, fx, cy + 2 * s };
        t->FillRoundedRectangle(D2D1_ROUNDED_RECT{filled, 2 * s, 2 * s}, m_brAccent.p);
        t->FillEllipse(D2D1_ELLIPSE{{fx, cy}, 9 * s, 9 * s}, m_brThumb.p);
    };

    drawLabel(Loc(L"Font Size", L"字体大小"), m_sizeLblY);
    drawValueR(std::to_wstring(sizeVal), m_sizeLblY);
    drawSlider(m_sizeSlider, (sizeVal - 8) / (float)(24 - 8));

    drawLabel(Loc(L"Opacity", L"不透明度"), m_opLblY);
    drawValueR(std::to_wstring(opVal) + L"%", m_opLblY);
    drawSlider(m_opSlider, (opVal - 30) / (float)(100 - 30));

    // ── Color swatches ──
    int panelSel = -1, titleSel = -1;
    for (int i = 0; i < 8; i++) {
        if (fabsf(app.bg[0] - kPanelColors[i][0]) < 0.01f &&
            fabsf(app.bg[1] - kPanelColors[i][1]) < 0.01f &&
            fabsf(app.bg[2] - kPanelColors[i][2]) < 0.01f) panelSel = i;
        if (fabsf(app.title[0] - kTitleColors[i][0]) < 0.01f &&
            fabsf(app.title[1] - kTitleColors[i][1]) < 0.01f &&
            fabsf(app.title[2] - kTitleColors[i][2]) < 0.01f) titleSel = i;
    }
    auto drawSwatch = [&](const RECT& rc, ID2D1Brush* fill, bool selected, int hitId) {
        fillRR(rc, 6 * s, fill);
        if (m_hover == hitId)
            t->DrawRoundedRectangle(D2D1_ROUNDED_RECT{RF(rc), 6 * s, 6 * s}, m_brHover.p, 2.0f);
        if (selected)
            t->DrawRoundedRectangle(D2D1_ROUNDED_RECT{RF(rc), 6 * s, 6 * s}, m_brSwatchRing.p, 2.0f * s);
    };

    drawLabel(Loc(L"Panel Color", L"面板颜色"), m_panelLblY);
    for (int i = 0; i < 8; i++)
        drawSwatch(m_panelSwatch[i], m_brPanelSw[i].p, i == panelSel, HIT_PANEL_SWATCH0 + i);
    fillRR(m_panelMore, 6 * s, m_brField.p);
    if (m_hover == HIT_PANEL_MORE)
        t->DrawRoundedRectangle(D2D1_ROUNDED_RECT{RF(m_panelMore), 6 * s, 6 * s}, m_brHover.p, 2.0f);
    drawStr(L"\u2026", m_panelMore, m_fmtSymbol.p, m_brText2.p);

    drawLabel(Loc(L"Title Bar Color", L"标题栏颜色"), m_titleLblY);
    for (int i = 0; i < 8; i++)
        drawSwatch(m_titleSwatch[i], m_brTitleSw[i].p, i == titleSel, HIT_TITLE_SWATCH0 + i);
    fillRR(m_titleMore, 6 * s, m_brField.p);
    if (m_hover == HIT_TITLE_MORE)
        t->DrawRoundedRectangle(D2D1_ROUNDED_RECT{RF(m_titleMore), 6 * s, 6 * s}, m_brHover.p, 2.0f);
    drawStr(L"\u2026", m_titleMore, m_fmtSymbol.p, m_brText2.p);

    // ── Title name position (left / center / right) ──
    drawLabel(Loc(L"Title Position", L"标题位置"), m_titlePosLblY);
    {
        const wchar_t* segLabels[3] = {
            Loc(L"Left", L"左"), Loc(L"Center", L"中"), Loc(L"Right", L"右"),
        };
        for (int i = 0; i < 3; i++) {
            bool active = (app.titleAlign == i);
            fillRR(m_titlePosSeg[i], 6 * s, active ? m_brAccent.p : m_brField.p);
            if (!active && m_hover == HIT_TITLEPOS0 + i)
                t->DrawRoundedRectangle(D2D1_ROUNDED_RECT{RF(m_titlePosSeg[i]), 6 * s, 6 * s},
                    m_brHover.p, 2.0f);
            drawStr(segLabels[i], m_titlePosSeg[i], m_fmtCenter.p,
                active ? m_brThumb.p : m_brText2.p);
        }
    }

    // ── Title bar height stepper ──
    drawLabel(Loc(L"Title Bar Height", L"标题栏高度"), m_thLblY);

    // ── Display mode (grid / list) ──
    drawLabel(Loc(L"Display Mode", L"显示方式"), m_displayModeLblY);
    {
        const wchar_t* dmLabels[2] = {
            Loc(L"Grid", L"网格"), Loc(L"List", L"列表"),
        };
        int dm = m_fence->GetRender().Appearance().displayMode;
        for (int i = 0; i < 2; i++) {
            bool active = (dm == i);
            fillRR(m_displayModeSeg[i], 6 * s, active ? m_brAccent.p : m_brField.p);
            if (!active && m_hover == HIT_DISPMODE0 + i)
                t->DrawRoundedRectangle(D2D1_ROUNDED_RECT{RF(m_displayModeSeg[i]), 6 * s, 6 * s},
                    m_brHover.p, 2.0f);
            drawStr(dmLabels[i], m_displayModeSeg[i], m_fmtCenter.p,
                active ? m_brThumb.p : m_brText2.p);
        }
    }

    // ── Title bar height stepper ──
    fillRR(m_thMinus, 6 * s, m_brField.p);
    if (m_hover == HIT_TITLEH_MINUS)
        t->DrawRoundedRectangle(D2D1_ROUNDED_RECT{RF(m_thMinus), 6 * s, 6 * s}, m_brHover.p, 2.0f);
    drawStr(L"\u2212", m_thMinus, m_fmtSymbol.p, m_brText.p);
    fillRR(m_thPlus, 6 * s, m_brField.p);
    if (m_hover == HIT_TITLEH_PLUS)
        t->DrawRoundedRectangle(D2D1_ROUNDED_RECT{RF(m_thPlus), 6 * s, 6 * s}, m_brHover.p, 2.0f);
    drawStr(L"+", m_thPlus, m_fmtSymbol.p, m_brText.p);
    int thVal = (int)(app.titleH / s + 0.5f);
    RECT valR{ m_thMinus.right, m_thMinus.top, m_thPlus.left, m_thMinus.bottom };
    drawStr(std::to_wstring(thVal), valR, m_fmtCenter.p, m_brText.p);

    // ── Separator + delete ──
    D2D1_RECT_F sep{20 * s, m_sepY, W - 20 * s, m_sepY + 1 * s};
    t->FillRectangle(sep, m_brBorder.p);
    fillRR(m_deleteBtn, 8 * s, m_brDanger.p);
    if (m_hover == HIT_DELETE_FENCE)
        fillRR(m_deleteBtn, 8 * s, m_brHover.p);
    drawStr(Loc(L"Delete Fence", L"删除围栏"), m_deleteBtn, m_fmtCenter.p, m_brThumb.p);

    m_render->EndDraw();
}

void SettingsPanel::Repaint() {
    if (!m_render || !m_hwnd) return;
    DrawPanel();
    m_render->Present(m_hwnd, m_x, m_y);
}

void SettingsPanel::ApplyToFence() {
    if (!m_fence) return;
    m_fence->GetRender().RebuildStyles();
    m_fence->SyncCollapsedSize();
    m_fence->Redraw();
    Config::MarkDirty();   // appearance changed → persist (debounced)
}

// ── Input ──

int SettingsPanel::HitTest(int mx, int my) const {
    POINT p{mx, my};
    for (int i = (int)m_hits.size() - 1; i >= 0; i--)
        if (PtInRect(&m_hits[i].rc, p)) return m_hits[i].id;
    return HIT_NONE;
}

void SettingsPanel::PickFont(int idx) {
    if (idx < 0 || idx >= (int)m_fonts.size()) return;
    auto& app = m_fence->GetRender().Appearance();
    wcscpy_s(app.fontName, _countof(app.fontName), m_fonts[idx].c_str());
    m_selFont = idx;
    if (idx < m_fontScrollTop) m_fontScrollTop = idx;
    if (idx >= m_fontScrollTop + m_fontRows) m_fontScrollTop = idx - m_fontRows + 1;
    ApplyToFence();
    Repaint();
}

void SettingsPanel::SetSizeValue(int v) {
    v = clampi(v, 8, 24);
    auto& app = m_fence->GetRender().Appearance();
    int cur = (int)(app.fontSize / m_s + 0.5f);
    if (v != cur) { app.fontSize = (float)v * m_s; ApplyToFence(); Repaint(); }
}

void SettingsPanel::SetOpacityValue(int v) {
    v = clampi(v, 30, 100);
    auto& app = m_fence->GetRender().Appearance();
    int cur = (int)(app.bg[3] * 100 + 0.5f);
    if (v != cur) {
        app.bg[3] = v / 100.0f;
        app.title[3] = (std::min)(1.0f, v / 100.0f + 0.10f);
        ApplyToFence();
        Repaint();
    }
}

void SettingsPanel::SetSizeFromX(int mx) {
    float x0 = m_sizeSlider.left + 9 * m_s, x1 = m_sizeSlider.right - 9 * m_s;
    float t = x1 > x0 ? ((float)mx - x0) / (x1 - x0) : 0;
    SetSizeValue(8 + (int)(clampf(t, 0, 1) * (24 - 8) + 0.5f));
}

void SettingsPanel::SetOpacityFromX(int mx) {
    float x0 = m_opSlider.left + 9 * m_s, x1 = m_opSlider.right - 9 * m_s;
    float t = x1 > x0 ? ((float)mx - x0) / (x1 - x0) : 0;
    SetOpacityValue(30 + (int)(clampf(t, 0, 1) * (100 - 30) + 0.5f));
}

void SettingsPanel::AdjustTitleH(float deltaDesign) {
    auto& app = m_fence->GetRender().Appearance();
    float nv = clampf(app.titleH + deltaDesign * m_s, 16 * m_s, 80 * m_s);
    if (fabsf(nv - app.titleH) > 0.5f) {
        app.titleH = nv;
        ApplyToFence();
        m_fence->RelayoutIcons();   // the grid's top edge moved; re-seat icons
        Repaint();
    }
}

void SettingsPanel::SetPanelColor(int idx) {
    auto& app = m_fence->GetRender().Appearance();
    app.bg[0] = kPanelColors[idx][0];
    app.bg[1] = kPanelColors[idx][1];
    app.bg[2] = kPanelColors[idx][2];
    ApplyToFence();
    Repaint();
}

void SettingsPanel::SetTitleColor(int idx) {
    auto& app = m_fence->GetRender().Appearance();
    app.title[0] = kTitleColors[idx][0];
    app.title[1] = kTitleColors[idx][1];
    app.title[2] = kTitleColors[idx][2];
    ApplyToFence();
    Repaint();
}

void SettingsPanel::ChoosePanelColor() {
    auto& app = m_fence->GetRender().Appearance();
    CHOOSECOLORW cc = {sizeof(cc)};
    static COLORREF cust[16] = {};
    cc.lpCustColors = cust;
    cc.rgbResult = RGB((int)(app.bg[0] * 255), (int)(app.bg[1] * 255), (int)(app.bg[2] * 255));
    cc.Flags = CC_RGBINIT | CC_FULLOPEN | CC_ANYCOLOR;
    cc.hwndOwner = m_hwnd;
    if (ChooseColorW(&cc)) {
        app.bg[0] = GetRValue(cc.rgbResult) / 255.0f;
        app.bg[1] = GetGValue(cc.rgbResult) / 255.0f;
        app.bg[2] = GetBValue(cc.rgbResult) / 255.0f;
        ApplyToFence();
        Repaint();
    }
}

void SettingsPanel::ChooseTitleColor() {
    auto& app = m_fence->GetRender().Appearance();
    CHOOSECOLORW cc = {sizeof(cc)};
    static COLORREF cust[16] = {};
    cc.lpCustColors = cust;
    cc.rgbResult = RGB((int)(app.title[0] * 255), (int)(app.title[1] * 255), (int)(app.title[2] * 255));
    cc.Flags = CC_RGBINIT | CC_FULLOPEN | CC_ANYCOLOR;
    cc.hwndOwner = m_hwnd;
    if (ChooseColorW(&cc)) {
        app.title[0] = GetRValue(cc.rgbResult) / 255.0f;
        app.title[1] = GetGValue(cc.rgbResult) / 255.0f;
        app.title[2] = GetBValue(cc.rgbResult) / 255.0f;
        ApplyToFence();
        Repaint();
    }
}

void SettingsPanel::ScrollFontList(int deltaRows) {
    int total = (int)m_fonts.size();
    if (total <= m_fontRows) return;
    int newTop = clampi(m_fontScrollTop + deltaRows, 0, total - m_fontRows);
    if (newTop != m_fontScrollTop) { m_fontScrollTop = newTop; Repaint(); }
}

void SettingsPanel::OnLButtonDown(int mx, int my) {
    // Font rows are not in m_hits; resolve them by row index.
    POINT p{mx, my};
    if (PtInRect(&m_fontList, p)) {
        int row = (int)((my - m_fontList.top) / m_rowH);
        PickFont(m_fontScrollTop + row);
        return;
    }
    int id = HitTest(mx, my);
    switch (id) {
    case HIT_CLOSE:         m_closing = true; break;
    case HIT_DELETE_FENCE:  m_deleteFence = true; m_closing = true; break;
    case HIT_SIZE_SLIDER:   m_dragSlider = 1; SetCapture(m_hwnd); SetSizeFromX(mx); break;
    case HIT_OPACITY_SLIDER: m_dragSlider = 2; SetCapture(m_hwnd); SetOpacityFromX(mx); break;
    case HIT_FONT_SCROLL:
        m_scrollDrag = true;
        m_scrollDragStartY = my;
        m_scrollDragStartTop = m_fontScrollTop;
        SetCapture(m_hwnd);
        break;
    case HIT_TITLEH_MINUS:  AdjustTitleH(-4); break;
    case HIT_TITLEH_PLUS:   AdjustTitleH(+4); break;
    case HIT_TITLEPOS0: case HIT_TITLEPOS0 + 1: case HIT_TITLEPOS0 + 2: {
        auto& app = m_fence->GetRender().Appearance();
        int align = id - HIT_TITLEPOS0;
        if (app.titleAlign != align) { app.titleAlign = align; ApplyToFence(); Repaint(); }
        break;
    }
    case HIT_PANEL_MORE:    ChoosePanelColor(); break;
    case HIT_TITLE_MORE:    ChooseTitleColor(); break;
    case HIT_DISPMODE0: case HIT_DISPMODE0 + 1: {
        auto& app = m_fence->GetRender().Appearance();
        int mode = id - HIT_DISPMODE0;
        if (app.displayMode != mode) {
            if (mode == 1) {   // switching to list: save grid layout
                m_fence->SaveGridLayout();
            }
            app.displayMode = mode;
            ApplyToFence();
            if (mode == 1) {
                m_fence->RelayoutIcons();
            } else {
                m_fence->RestoreGridLayout();
                m_fence->RelayoutIcons();
            }
            Repaint();
        }
        break;
    }
    default:
        if (id >= HIT_PANEL_SWATCH0 && id < HIT_PANEL_SWATCH0 + 8) SetPanelColor(id - HIT_PANEL_SWATCH0);
        else if (id >= HIT_TITLE_SWATCH0 && id < HIT_TITLE_SWATCH0 + 8) SetTitleColor(id - HIT_TITLE_SWATCH0);
        break;
    }
}

void SettingsPanel::OnMouseMove(int mx, int my) {
    POINT p{mx, my};
    int oldHover = m_hover, oldRow = m_hoverRow;
    if (PtInRect(&m_fontList, p)) {
        m_hover = HIT_FONT_LIST;
        m_hoverRow = (int)((my - m_fontList.top) / m_rowH);
    } else {
        m_hover = HitTest(mx, my);
        m_hoverRow = -1;
    }
    if (m_hover != oldHover || m_hoverRow != oldRow) Repaint();

    if (m_dragSlider == 1) SetSizeFromX(mx);
    else if (m_dragSlider == 2) SetOpacityFromX(mx);
    else if (m_scrollDrag) {
        int total = (int)m_fonts.size();
        if (total > m_fontRows) {
            float trackH = (float)(m_fontScroll.bottom - m_fontScroll.top);
            float thumbH = (std::max)(20 * m_s, trackH * m_fontRows / total);
            float freeH = trackH - thumbH;
            int dy = my - m_scrollDragStartY;
            int delta = freeH > 1
                ? (int)((dy / freeH) * (total - m_fontRows) + (dy >= 0 ? 0.5f : -0.5f))
                : 0;
            int newTop = clampi(m_scrollDragStartTop + delta, 0, total - m_fontRows);
            if (newTop != m_fontScrollTop) { m_fontScrollTop = newTop; Repaint(); }
        }
    }
}

void SettingsPanel::OnLButtonUp(int, int) {
    if (m_dragSlider || m_scrollDrag) {
        m_dragSlider = 0;
        m_scrollDrag = false;
        ReleaseCapture();
    }
}

void SettingsPanel::OnMouseWheel(int delta, int sx, int sy) {
    POINT p{sx, sy};
    ScreenToClient(m_hwnd, &p);
    if (PtInRect(&m_fontList, p) || PtInRect(&m_fontScroll, p)) {
        ScrollFontList(delta > 0 ? -3 : 3);
    } else if (PtInRect(&m_sizeSlider, p)) {
        auto& app = m_fence->GetRender().Appearance();
        SetSizeValue((int)(app.fontSize / m_s + 0.5f) + (delta > 0 ? 1 : -1));
    } else if (PtInRect(&m_opSlider, p)) {
        auto& app = m_fence->GetRender().Appearance();
        SetOpacityValue((int)(app.bg[3] * 100 + 0.5f) + (delta > 0 ? 1 : -1));
    }
}

// ── WndProc ──

LRESULT CALLBACK SettingsPanel::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = (SettingsPanel*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        auto* cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (self) self->OnLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSEMOVE:
        if (self) self->OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_LBUTTONUP:
        if (self) self->OnLButtonUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSEWHEEL:
        if (self) self->OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wp),
            GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_KEYDOWN:
        if (self && wp == VK_ESCAPE) self->m_closing = true;
        return 0;
    case WM_DESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (self) self->m_hwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
