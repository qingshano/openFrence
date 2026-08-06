#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vector>

/// Glyphs rasterized for the Fluent menus (tray + fence title bar). Each
/// maps 1:1 to a menu command; the shapes follow the Fluent/Segoe MDL2
/// vocabulary closely enough to read instantly at 16 logical px.
enum class MenuGlyph {
    Plus,      // New Fence
    Eye,       // Show All Fences (fences currently hidden)
    EyeOff,    // Hide All Fences (fences currently visible)
    Grid,      // Hide/Show Desktop Icons
    Globe,     // Language submenu
    Power,     // Exit
    Pencil,    // Rename fence
    Sliders,   // Appearance Settings
    Trash,     // Delete Fence
    Remove,    // Remove icon from fence (circle + minus)
    Rocket,    // Start with Windows (autostart toggle)
    Folder,    // Open config file location
};

/// Device-pixel size menu glyphs should be drawn at: 16 logical px scaled
/// by the display DPI and rounded UP — rounding down blurs the stroke grid
/// at 125 % / 150 % scaling.
int MenuGlyphSizePx();

/// Rasterize one glyph into a 32 bpp top-down DIB section of sizePx×sizePx.
/// Pixels are written as PREMULTIPLIED WHITE (R = G = B = A = coverage):
///  • icon_extract.cpp force-opaques all-zero-alpha bitmaps, so plain
///    white-with-alpha would not survive the Fluent path;
///  • it also skips re-premultiplying pixels whose channels never exceed
///    alpha, so premultiplied input passes through untouched;
///  • the classic TrackPopupMenu fallback renders the bitmap itself and
///    honours the same alpha channel.
/// Returns null on allocation failure (callers then show the item without
/// an icon rather than failing the whole menu).
HBITMAP CreateMenuGlyphBitmap(MenuGlyph glyph, int sizePx);

/// RAII owner for a batch of glyph HBITMAPs. The menu API only stores the
/// handles by reference (MIIM_BITMAP), so the bitmaps must stay alive until
/// the menu interaction — Fluent or classic fallback — has fully returned.
/// Destroy this only after TrackPopupMenu/FluentMenu::Run came back.
struct GlyphBitmapSet {
    std::vector<HBITMAP> bmps;

    GlyphBitmapSet() = default;
    GlyphBitmapSet(const GlyphBitmapSet&) = delete;
    GlyphBitmapSet& operator=(const GlyphBitmapSet&) = delete;
    ~GlyphBitmapSet() { clear(); }

    /// Create + adopt one glyph bitmap; returns the handle (or null).
    HBITMAP add(MenuGlyph g, int sizePx) {
        HBITMAP b = CreateMenuGlyphBitmap(g, sizePx);
        bmps.push_back(b);
        return b;
    }
    void clear() {
        for (HBITMAP b : bmps) if (b) DeleteObject(b);
        bmps.clear();
    }
};
