#include "icon_extract.h"

bool ExtractBitmapPixels(HBITMAP hb, std::vector<BYTE>& out, int& w, int& h) {
    BITMAP bm = {};
    if (!GetObjectW(hb, sizeof(bm), &bm)) return false;
    if (bm.bmWidth <= 0 || bm.bmHeight <= 0 || bm.bmWidth > 512 || bm.bmHeight > 512)
        return false;
    w = bm.bmWidth; h = bm.bmHeight;

    HDC screen = GetDC(nullptr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;          // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    out.assign((size_t)w * h * 4, 0);
    int got = GetDIBits(screen, hb, 0, h, out.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    if (got != h) { out.clear(); return false; }

    // Decide alpha handling. If every alpha is 0 the bitmap is likely an
    // opaque image with junk in the alpha channel -> force opaque.
    bool anyAlpha = false, allOpaque = true;
    for (size_t i = 3; i < out.size(); i += 4) {
        if (out[i] != 0) anyAlpha = true;
        if (out[i] != 255) allOpaque = false;
    }
    if (!anyAlpha) { for (size_t i = 3; i < out.size(); i += 4) out[i] = 255; return true; }
    if (allOpaque) return true;
    // Real alpha. If any color channel exceeds alpha the pixels are straight
    // (not premultiplied) -> premultiply for D2D.
    bool premul = true;
    for (size_t i = 0; i < out.size(); i += 4) {
        BYTE a = out[i + 3];
        if (out[i] > a || out[i + 1] > a || out[i + 2] > a) { premul = false; break; }
    }
    if (!premul) {
        for (size_t i = 0; i < out.size(); i += 4) {
            BYTE a = out[i + 3];
            out[i + 0] = (BYTE)((out[i + 0] * a) / 255);
            out[i + 1] = (BYTE)((out[i + 1] * a) / 255);
            out[i + 2] = (BYTE)((out[i + 2] * a) / 255);
        }
    }
    return true;
}

bool ExtractIconPixels(HICON icon, std::vector<BYTE>& out, int& w, int& h) {
    if (!icon) return false;
    ICONINFO ii = {};
    if (!GetIconInfo(icon, &ii)) return false;
    bool ok = false;
    if (ii.hbmColor)
        ok = ExtractBitmapPixels(ii.hbmColor, out, w, h);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask)  DeleteObject(ii.hbmMask);
    return ok;
}

bool TightBounds(const BYTE* bgra, int w, int h,
                 int& x0, int& y0, int& x1, int& y1) {
    x0 = w; y0 = h; x1 = -1; y1 = -1;
    for (int y = 0; y < h; y++) {
        const BYTE* row = bgra + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            if (row[(size_t)x * 4 + 3] == 0) continue;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    return x1 >= 0;
}
