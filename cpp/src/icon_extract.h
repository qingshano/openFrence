#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vector>

/// Shared shell-icon pixel extraction.
///
/// Both the fence renderer and the Fluent context menu need raw premultiplied
/// BGRA pixels out of shell-provided bitmaps/icons; the alpha-channel
/// sanitizing rules live here so every consumer treats them identically.

/// Extract premultiplied BGRA pixels from an HBITMAP (menu item bitmaps,
/// icon color planes). Handles the common shell quirks: an all-zero alpha
/// channel (opaque image with junk alpha) is forced opaque, and straight
/// (non-premultiplied) alpha is premultiplied for Direct2D. Returns false
/// when the handle is not a usable bitmap of sane dimensions.
bool ExtractBitmapPixels(HBITMAP hb, std::vector<BYTE>& out, int& w, int& h);

/// Extract premultiplied BGRA pixels from an HICON's color plane. Returns
/// false for monochrome icons (no color plane) or invalid handles.
bool ExtractIconPixels(HICON icon, std::vector<BYTE>& out, int& w, int& h);

/// Tight bounding box (inclusive) of all pixels with alpha > 0.
/// Returns false when the image is fully transparent.
bool TightBounds(const BYTE* bgra, int w, int h,
                 int& x0, int& y0, int& x1, int& y1);
