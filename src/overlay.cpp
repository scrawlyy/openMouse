// openMouse — per-seat pointer rendering
// SPDX-License-Identifier: MIT
//
// Windows has exactly one hardware cursor. Every seat that does not currently
// own it gets its pointer drawn here instead: a layered, click-through,
// topmost window holding a 32bpp premultiplied-alpha bitmap of an arrow in the
// seat's colour.
#include "openmouse.h"

namespace om {
namespace {

// Classic arrow outline. The tip sits at (1,1) so the hotspot offset is (1,1).
constexpr POINT kArrow[] = {
    {1, 1}, {1, 27}, {7, 20}, {12, 31}, {16, 29}, {11, 19}, {19, 19}
};
constexpr int kArrowPts = (int)(sizeof(kArrow) / sizeof(kArrow[0]));
constexpr int kBmpW = 22;
constexpr int kBmpH = 34;

// Even-odd point-in-polygon. Exact and cheap at this size; avoids the GDI
// round trip and the ambiguity of distinguishing "drawn black" from
// "untouched transparent" in an ARGB buffer.
bool InsideArrow(double px, double py) {
    bool in = false;
    for (int i = 0, j = kArrowPts - 1; i < kArrowPts; j = i++) {
        const double xi = kArrow[i].x, yi = kArrow[i].y;
        const double xj = kArrow[j].x, yj = kArrow[j].y;
        if (((yi > py) != (yj > py)) &&
            (px < (xj - xi) * (py - yi) / (yj - yi) + xi))
            in = !in;
    }
    return in;
}

const wchar_t* kOverlayClass = L"openMouse.CursorOverlay";

void EnsureClass(HINSTANCE inst) {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = inst;
    wc.lpszClassName = kOverlayClass;
    wc.hCursor       = nullptr;
    RegisterClassExW(&wc);
    done = true;
}

} // namespace

// Reads the real Windows arrow out of the system cursor and recolours it, so
// each seat's pointer has the native silhouette, the native anti-aliasing and
// the native hotspot instead of a hand-rasterised approximation.
//
// The tint maps luminance onto the seat colour: the white body of the arrow
// becomes the seat colour and the black outline stays black, which keeps the
// pointer legible on any background exactly as the stock cursor is.
//
// GetDIBits is used rather than DrawIconEx because GDI does not reliably carry
// the alpha channel through a blit, and the alpha is the whole point here.
bool CursorOverlay::RenderFromSystemCursor(COLORREF color, std::vector<BYTE>& out) {
    HCURSOR hc = (HCURSOR)LoadImageW(nullptr, IDC_ARROW, IMAGE_CURSOR, 0, 0,
                                     LR_SHARED | LR_DEFAULTSIZE);
    if (!hc) return false;

    ICONINFO ii{};
    if (!GetIconInfo(hc, &ii)) return false;

    // Ownership: GetIconInfo hands us copies of the bitmaps, ours to delete.
    struct BmpGuard {
        HBITMAP a, b;
        ~BmpGuard() { if (a) DeleteObject(a); if (b) DeleteObject(b); }
    } guard{ ii.hbmMask, ii.hbmColor };

    if (!ii.hbmColor) return false;      // classic monochrome cursor — fall back

    BITMAP bm{};
    if (!GetObjectW(ii.hbmColor, sizeof(bm), &bm)) return false;
    if (bm.bmWidth <= 0 || bm.bmHeight <= 0 || bm.bmWidth > 256 || bm.bmHeight > 256)
        return false;

    const int w = bm.bmWidth, h = bm.bmHeight;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;     // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    std::vector<BYTE> src((size_t)w * h * 4, 0);
    HDC screen = GetDC(nullptr);
    const int got = GetDIBits(screen, ii.hbmColor, 0, h, src.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    if (got != h) return false;

    bool anyAlpha = false;
    for (size_t i = 3; i < src.size(); i += 4)
        if (src[i]) { anyAlpha = true; break; }
    if (!anyAlpha) return false;         // no usable alpha — fall back

    const BYTE r = GetRValue(color), g = GetGValue(color), b = GetBValue(color);
    out.assign((size_t)w * h * 4, 0);

    // Two special colours fall out of the luminance mapping naturally:
    //   FFFFFF — white × luminance is the identity, i.e. the stock Windows
    //            arrow, untinted.
    //   000000 — would be an invisible black blob under the plain mapping, so
    //            it instead inverts luminance: black body, white outline —
    //            the Windows "black" pointer scheme.
    const bool blackScheme = (color == RGB(0, 0, 0));

    for (size_t i = 0; i < src.size(); i += 4) {
        const BYTE sb = src[i], sg = src[i + 1], sr = src[i + 2], sa = src[i + 3];
        if (!sa) continue;

        // Rec. 601 luminance: 1.0 on the arrow's white body, 0.0 on its
        // outline. Tinting by it recolours the body and leaves the edge dark.
        int lum = (sr * 77 + sg * 150 + sb * 29) >> 8;

        BYTE tr = r, tg = g, tb = b;
        if (blackScheme) { lum = 255 - lum; tr = tg = tb = 255; }

        // Store premultiplied BGRA, which is what UpdateLayeredWindow wants.
        out[i]     = (BYTE)((tb * lum / 255) * sa / 255);
        out[i + 1] = (BYTE)((tg * lum / 255) * sa / 255);
        out[i + 2] = (BYTE)((tr * lum / 255) * sa / 255);
        out[i + 3] = sa;
    }

    w_    = w;
    h_    = h;
    hotX_ = (int)ii.xHotspot;
    hotY_ = (int)ii.yHotspot;
    return true;
}

void CursorOverlay::RenderFallbackArrow(COLORREF color, std::vector<BYTE>& out) {
    w_ = kBmpW;
    h_ = kBmpH;
    hotX_ = 1;
    hotY_ = 1;
    out.assign((size_t)w_ * h_ * 4, 0);

    const BYTE r = GetRValue(color), g = GetGValue(color), b = GetBValue(color);

    std::vector<bool> inside((size_t)w_ * h_, false);
    for (int y = 0; y < h_; ++y)
        for (int x = 0; x < w_; ++x)
            inside[(size_t)y * w_ + x] = InsideArrow(x + 0.5, y + 0.5);

    for (int y = 0; y < h_; ++y) {
        for (int x = 0; x < w_; ++x) {
            BYTE* p = out.data() + ((size_t)y * w_ + x) * 4;
            if (!inside[(size_t)y * w_ + x]) continue;

            bool edge = false;
            for (int dy = -1; dy <= 1 && !edge; ++dy)
                for (int dx = -1; dx <= 1 && !edge; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w_ || ny >= h_ ||
                        !inside[(size_t)ny * w_ + nx])
                        edge = true;
                }
            if (edge) { p[0] = 20; p[1] = 20; p[2] = 20; p[3] = 255; }
            else      { p[0] = b;  p[1] = g;  p[2] = r;  p[3] = 255; }
        }
    }
}

bool CursorOverlay::Create(HINSTANCE inst, int seatIndex, COLORREF color) {
    EnsureClass(inst);
    w_ = kBmpW;
    h_ = kBmpH;

    // Title carries the seat index so overlays are identifiable in Spy++ or
    // window-enumeration tools during debugging.
    wchar_t title[32];
    swprintf(title, 32, L"openMouse seat %d", seatIndex);

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        kOverlayClass, title, WS_POPUP,
        0, 0, w_, h_,
        nullptr, nullptr, inst, nullptr);
    if (!hwnd_) return false;

    Render(color);
    return true;
}

void CursorOverlay::Render(COLORREF color) {
    // Prefer the real system arrow; the hand-drawn polygon is only a fallback
    // for machines where the cursor cannot be read back with usable alpha.
    std::vector<BYTE> pixels;
    if (!RenderFromSystemCursor(color, pixels))
        RenderFallbackArrow(color, pixels);

    if (w_ <= 0 || h_ <= 0 || pixels.size() != (size_t)w_ * h_ * 4) return;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w_;
    bi.bmiHeader.biHeight      = -h_;          // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    bitmap_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap_ || !bits) { ReleaseDC(nullptr, screen); return; }

    memcpy(bits, pixels.data(), pixels.size());

    // The window was created at the fallback size; the system cursor may be
    // larger, so resize before the layered update or the arrow is clipped.
    SetWindowPos(hwnd_, nullptr, 0, 0, w_, h_, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, bitmap_);

    POINT  src{0, 0};
    SIZE   size{w_, h_};
    BLENDFUNCTION bf{};
    bf.BlendOp             = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat         = AC_SRC_ALPHA;

    UpdateLayeredWindow(hwnd_, screen, nullptr, &size, mem, &src, 0, &bf, ULW_ALPHA);

    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

void CursorOverlay::MoveTo(POINT ptVirtual) {
    if (!hwnd_) return;
    // Offset by the real hotspot so the tip lands on the logical position.
    SetWindowPos(hwnd_, HWND_TOPMOST, ptVirtual.x - hotX_, ptVirtual.y - hotY_, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
}

void CursorOverlay::Show(bool visible) {
    if (!hwnd_ || visible == visible_) return;
    ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    visible_ = visible;
}

void CursorOverlay::Destroy() {
    if (bitmap_) { DeleteObject(bitmap_); bitmap_ = nullptr; }
    if (hwnd_)   { DestroyWindow(hwnd_);  hwnd_ = nullptr; }
    visible_ = false;
}

// ---------------------------------------------------------------- utilities

RECT VirtualScreenRect() {
    RECT r;
    r.left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r.top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r.right  = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.bottom = r.top  + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return r;
}

void ClampToVirtualScreen(POINT& p) {
    const RECT r = VirtualScreenRect();
    if (p.x < r.left)       p.x = r.left;
    if (p.y < r.top)        p.y = r.top;
    if (p.x > r.right  - 1) p.x = r.right  - 1;
    if (p.y > r.bottom - 1) p.y = r.bottom - 1;
}

} // namespace om
