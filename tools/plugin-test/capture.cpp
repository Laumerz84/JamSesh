// Window helpers for the GUI test: capture a window's pixels (incl. GPU content) and send it mouse /
// keyboard messages without moving the real cursor. All coordinates are screenshot pixels, i.e.
// relative to the window's outer rectangle (GetWindowRect), so clicks land on what screenshots show.
#define NOMINMAX
#include <windows.h>
#include <vector>

namespace
{
    // The plugin's UI is a child window inside the host's window, so mouse messages must go to the
    // deepest visible child under the point (in that child's client coordinates).
    HWND targetAt (HWND top, int x, int y)
    {
        RECT rc {};
        GetWindowRect (top, &rc);
        POINT screen { rc.left + x, rc.top + y };
        HWND h = top;
        for (;;)
        {
            POINT local = screen;
            ScreenToClient (h, &local);
            HWND child = ChildWindowFromPointEx (h, local, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
            if (child == nullptr || child == h)
                return h;
            h = child;
        }
    }

    LPARAM toClient (HWND hwnd, HWND top, int x, int y)
    {
        RECT rc {};
        GetWindowRect (top, &rc);
        POINT p { rc.left + x, rc.top + y };
        ScreenToClient (hwnd, &p);
        return MAKELPARAM (p.x, p.y);
    }

    HWND lastTop = nullptr;
    LPARAM toClient (HWND hwnd, int x, int y) { return toClient (hwnd, lastTop, x, y); }
}

void clickWindow (void* nativeHandle, int x, int y, bool dbl)
{
    lastTop = (HWND) nativeHandle;
    auto hwnd = targetAt (lastTop, x, y);
    const LPARAM pos = toClient (hwnd, x, y);
    if (getenv ("CLICK_DEBUG") != nullptr)
    {
        char cls[128] = {}, topCls[128] = {};
        GetClassNameA (hwnd, cls, 127);
        GetClassNameA (lastTop, topCls, 127);
        printf ("    click -> hwnd %p class '%s' (top %p '%s') client %d,%d\n", (void*) hwnd, cls, (void*) lastTop, topCls,
                (int) (short) LOWORD (pos), (int) (short) HIWORD (pos));
    }
    PostMessage (hwnd, WM_MOUSEMOVE, 0, pos);
    PostMessage (hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pos);
    PostMessage (hwnd, WM_LBUTTONUP, 0, pos);
    if (dbl)
    {
        PostMessage (hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, pos);
        PostMessage (hwnd, WM_LBUTTONUP, 0, pos);
    }
}

void dragWindow (void* nativeHandle, int x0, int y0, int x1, int y1, int steps)
{
    lastTop = (HWND) nativeHandle;
    auto hwnd = targetAt (lastTop, x0, y0);
    PostMessage (hwnd, WM_MOUSEMOVE, 0, toClient (hwnd, x0, y0));
    PostMessage (hwnd, WM_LBUTTONDOWN, MK_LBUTTON, toClient (hwnd, x0, y0));
    for (int i = 1; i <= steps; ++i)
        PostMessage (hwnd, WM_MOUSEMOVE, MK_LBUTTON, toClient (hwnd, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps));
    PostMessage (hwnd, WM_LBUTTONUP, 0, toClient (hwnd, x1, y1));
}

void keyWindow (void* nativeHandle, int virtualKey)
{
    // keys go to whichever window has keyboard focus inside the plugin (fallback: the deepest child)
    HWND focus = GetFocus();
    HWND target = focus != nullptr ? focus : targetAt ((HWND) nativeHandle, 200, 200);
    PostMessage (target, WM_KEYDOWN, (WPARAM) virtualKey, 0);
    PostMessage (target, WM_KEYUP, (WPARAM) virtualKey, 0);
}

// JUCE hit-tests with WindowFromPoint, so the window must really be on top for clicks to land
void setTopmost (void* nativeHandle, bool topmost)
{
    SetWindowPos ((HWND) nativeHandle, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | (topmost ? 0 : SWP_NOACTIVATE));
}

void resizeWindow (void* nativeHandle, int w, int h)
{
    SetWindowPos ((HWND) nativeHandle, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool captureWindow (void* nativeHandle, int& w, int& h, std::vector<unsigned char>& bgra)
{
    RECT rc {};
    if (! GetWindowRect ((HWND) nativeHandle, &rc))
        return false;
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;

    HDC screen = GetDC (nullptr);
    HDC mem = CreateCompatibleDC (screen);
    HBITMAP bmp = CreateCompatibleBitmap (screen, w, h);
    auto old = SelectObject (mem, bmp);
    // PrintWindow renders the window itself (incl. GPU content), so it works even when covered
    const bool ok = PrintWindow ((HWND) nativeHandle, mem, 2 /* PW_RENDERFULLCONTENT */) != 0
                    || BitBlt (mem, 0, 0, w, h, screen, rc.left, rc.top, SRCCOPY | CAPTUREBLT) != 0;

    BITMAPINFO bi {};
    bi.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down rows
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bgra.resize ((size_t) w * (size_t) h * 4);
    GetDIBits (mem, bmp, 0, (UINT) h, bgra.data(), &bi, DIB_RGB_COLORS);

    SelectObject (mem, old);
    DeleteObject (bmp);
    DeleteDC (mem);
    ReleaseDC (nullptr, screen);
    return ok;
}
