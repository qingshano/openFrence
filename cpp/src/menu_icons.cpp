#include "menu_icons.h"
#include <cmath>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Menu glyph rasterizer.
//
// The tray and fence menus carry no shell image list, so their icons are
// rasterized here from vector primitive tables: each glyph is a handful of
// segments / circles / ellipses / rounded rects defined in a 16×16 design
// grid, and every output pixel gets an analytic coverage value (distance to
// the nearest primitive vs. the stroke half-width, smoothed over one pixel).
// That yields resolution-independent, anti-aliased strokes at any DPI size
// without shipping bitmap assets or taking a dependency on an icon font.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Distance from point (px,py) to the segment (x1,y1)-(x2,y2).
static float DistSeg(float px, float py, float x1, float y1, float x2, float y2) {
    float vx = x2 - x1, vy = y2 - y1;
    float wx = px - x1, wy = py - y1;
    float len2 = vx * vx + vy * vy;
    float t = len2 < 1e-5f ? 0.0f : (wx * vx + wy * vy) / len2;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    float dx = px - (x1 + t * vx), dy = py - (y1 + t * vy);
    return std::sqrt(dx * dx + dy * dy);
}

// Signed distance to a rounded rectangle centered at (cx,cy) with half
// extents (hw,hh) and corner radius r. Negative inside.
static float SdRoundRect(float px, float py, float cx, float cy,
                         float hw, float hh, float r) {
    float qx = std::fabs(px - cx) - (hw - r);
    float qy = std::fabs(py - cy) - (hh - r);
    float ax = (std::max)(qx, 0.0f), ay = (std::max)(qy, 0.0f);
    return std::sqrt(ax * ax + ay * ay) + (std::min)((std::max)(qx, qy), 0.0f) - r;
}

// Minimum distance from (u,v) to the glyph's primitives (design units).
// Stroked shapes contribute |sdf|, filled shapes the raw sdf, so a single
// coverage conversion handles both.
static float GlyphDist(MenuGlyph g, float u, float v) {
    float m = 1e9f;
    auto seg = [&](float x1, float y1, float x2, float y2) {
        m = (std::min)(m, DistSeg(u, v, x1, y1, x2, y2));
    };
    auto ring = [&](float cx, float cy, float r) {           // stroked circle
        float dx = u - cx, dy = v - cy;
        m = (std::min)(m, std::fabs(std::sqrt(dx * dx + dy * dy) - r));
    };
    auto disc = [&](float cx, float cy, float r) {           // filled circle
        float dx = u - cx, dy = v - cy;
        m = (std::min)(m, std::sqrt(dx * dx + dy * dy) - r);
    };
    // Stroked ellipse. The radial test is rescaled by the smaller radius,
    // which keeps the perceived stroke width roughly even around the rim.
    auto oval = [&](float cx, float cy, float rx, float ry) {
        float dx = u - cx, dy = v - cy;
        float q = std::sqrt((dx * dx) / (rx * rx) + (dy * dy) / (ry * ry));
        m = (std::min)(m, std::fabs(q - 1.0f) * (std::min)(rx, ry));
    };
    auto rrect = [&](float cx, float cy, float hw, float hh, float r) {
        m = (std::min)(m, std::fabs(SdRoundRect(u, v, cx, cy, hw, hh, r)));
    };

    switch (g) {
    case MenuGlyph::Plus:
        seg(8.0f, 3.5f, 8.0f, 12.5f);
        seg(3.5f, 8.0f, 12.5f, 8.0f);
        break;

    case MenuGlyph::Eye:
        oval(8.0f, 8.0f, 5.5f, 3.6f);
        disc(8.0f, 8.0f, 1.7f);
        break;

    case MenuGlyph::EyeOff:
        oval(8.0f, 8.0f, 5.5f, 3.6f);
        disc(8.0f, 8.0f, 1.7f);
        seg(3.5f, 12.5f, 12.5f, 3.5f);                       // the slash
        break;

    case MenuGlyph::Grid:                                    // 2×2 app grid
        rrect(5.0f, 5.0f, 2.1f, 2.1f, 0.9f);
        rrect(11.0f, 5.0f, 2.1f, 2.1f, 0.9f);
        rrect(5.0f, 11.0f, 2.1f, 2.1f, 0.9f);
        rrect(11.0f, 11.0f, 2.1f, 2.1f, 0.9f);
        break;

    case MenuGlyph::Globe:
        ring(8.0f, 8.0f, 5.25f);                             // outline
        seg(2.75f, 8.0f, 13.25f, 8.0f);                      // equator
        oval(8.0f, 8.0f, 2.4f, 5.25f);                       // meridian
        break;

    case MenuGlyph::Power: {                                 // arc + stem
        float dx = u - 8.0f, dy = v - 8.0f;
        float ang = std::atan2(dx, -dy);                     // 0 = straight up
        if (std::fabs(ang) > 0.95f)                          // gap at the top
            m = (std::min)(m, std::fabs(std::sqrt(dx * dx + dy * dy) - 4.75f));
        seg(8.0f, 2.75f, 8.0f, 7.5f);
        break;
    }

    case MenuGlyph::Pencil:                                  // edit
        seg(3.4f, 11.4f, 11.4f, 3.4f);                       // body edge A
        seg(4.6f, 12.6f, 12.6f, 4.6f);                       // body edge B
        seg(11.4f, 3.4f, 12.6f, 4.6f);                       // blunt end
        seg(3.4f, 11.4f, 2.9f, 13.1f);                       // nib edge A
        seg(4.6f, 12.6f, 2.9f, 13.1f);                       // nib edge B
        break;

    case MenuGlyph::Sliders:                                 // settings sliders
        seg(3.0f, 4.5f, 13.0f, 4.5f);  disc(10.5f, 4.5f, 1.6f);
        seg(3.0f, 8.0f, 13.0f, 8.0f);  disc(5.5f, 8.0f, 1.6f);
        seg(3.0f, 11.5f, 13.0f, 11.5f); disc(9.0f, 11.5f, 1.6f);
        break;

    case MenuGlyph::Trash:
        seg(3.5f, 4.5f, 12.5f, 4.5f);                        // lid
        seg(6.5f, 3.0f, 9.5f, 3.0f);                         // handle
        seg(6.5f, 3.0f, 6.5f, 4.5f);
        seg(9.5f, 3.0f, 9.5f, 4.5f);
        rrect(8.0f, 8.9f, 2.9f, 4.1f, 1.0f);                 // body
        seg(6.9f, 6.8f, 6.9f, 11.2f);                        // inner lines
        seg(9.1f, 6.8f, 9.1f, 11.2f);
        break;

    case MenuGlyph::Remove:                                  // circle + minus
        ring(8.0f, 8.0f, 5.4f);
        seg(5.6f, 8.0f, 10.4f, 8.0f);
        break;

    case MenuGlyph::Rocket:                                  // launch / autostart
        oval(8.0f, 6.8f, 2.6f, 4.4f);                        // body capsule
        ring(8.0f, 5.9f, 1.05f);                             // porthole
        seg(5.7f, 9.3f, 3.6f, 12.2f);                        // left fin
        seg(10.3f, 9.3f, 12.4f, 12.2f);                      // right fin
        seg(8.0f, 12.4f, 8.0f, 14.1f);                       // exhaust flame
        break;

    case MenuGlyph::Folder:                                  // config location
        rrect(8.0f, 9.2f, 5.5f, 3.5f, 1.0f);                 // body
        seg(3.0f, 5.7f, 3.0f, 4.0f);                         // tab: up…
        seg(3.0f, 4.0f, 6.3f, 4.0f);                         // …across…
        seg(6.3f, 4.0f, 7.6f, 5.7f);                         // …and slant down
        break;
    }
    return m;
}

} // namespace

int MenuGlyphSizePx() {
    HDC h = GetDC(nullptr);
    int dpi = GetDeviceCaps(h, LOGPIXELSX);
    ReleaseDC(nullptr, h);
    return (int)std::ceil(16.0f * (dpi / 96.0f));
}

HBITMAP CreateMenuGlyphBitmap(MenuGlyph glyph, int sizePx) {
    if (sizePx < 8) sizePx = 8;

    HDC screen = GetDC(nullptr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sizePx;
    bi.bmiHeader.biHeight = -sizePx;         // top-down rows
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        return nullptr;
    }

    // device px → design units (the glyph grid is 16×16)
    const float toDesign = 16.0f / sizePx;
    const float halfWidth = 0.75f;           // stroke half-width, design units
    const float aa = 0.6f;                   // AA half-band, design units

    BYTE* px = (BYTE*)bits;
    for (int y = 0; y < sizePx; y++) {
        for (int x = 0; x < sizePx; x++) {
            float d = GlyphDist(glyph, (x + 0.5f) * toDesign, (y + 0.5f) * toDesign);
            float cov = (halfWidth + aa - d) / (2.0f * aa);
            cov = cov < 0.0f ? 0.0f : (cov > 1.0f ? 1.0f : cov);
            BYTE val = (BYTE)(cov * 255.0f + 0.5f);
            BYTE* p = px + ((size_t)y * sizePx + x) * 4;
            // Premultiplied white (see header for why): BGRA order.
            p[0] = val; p[1] = val; p[2] = val; p[3] = val;
        }
    }
    return dib;
}
