// openMouse — multi-pointer input for Windows
// SPDX-License-Identifier: MIT
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace om {

// ---------------------------------------------------------------- constants

constexpr int   kMaxSeats        = 8;
constexpr UINT  kTrayCallbackMsg = WM_APP + 1;
constexpr UINT  kTrayIconId      = 1;

// ---------------------------------------------------------------- device id

// A raw-input device is identified by its device interface path, e.g.
//   \\?\HID#VID_046D&PID_C52B&MI_01&Col01#7&2f8e7b0d&0&0000#{...}
// This is stable across reboots for a given physical port+device, which is
// what we bind against. Device *handles* are not stable and must never be
// persisted.
struct DeviceInfo {
    HANDLE       handle = nullptr;   // volatile, valid for this session only
    std::wstring path;               // stable identity, used in config
    std::wstring friendlyName;       // best-effort, for the UI
    DWORD        type = 0;           // RIM_TYPEMOUSE | RIM_TYPEKEYBOARD
};

std::vector<DeviceInfo> EnumerateDevices();
std::wstring            DevicePathFromHandle(HANDLE h);
std::wstring            FriendlyNameFromPath(const std::wstring& path);

// ---------------------------------------------------------------- seats

struct Seat {
    int      index   = 0;
    COLORREF color   = RGB(255, 0, 0);
    bool     enabled = false;

    // Virtual-desktop coordinates of this seat's pointer.
    POINT    pos{0, 0};

    // The window this seat last clicked into. Keyboard events bound to this
    // seat are steered here. Null until the seat clicks something.
    HWND     focusTarget = nullptr;

    // Button state, so we can synthesise correct down/up pairs and detect
    // drags in progress. x1/x2 are the browser back/forward side buttons.
    bool     lDown = false, rDown = false, mDown = false;
    bool     x1Down = false, x2Down = false;

    // When the previous left press physically ARRIVED and when it was
    // actually INJECTED. The cross-window settle delays injection by ~250 ms,
    // which compresses the app-perceived gap between successive clicks — two
    // deliberate single clicks can land within the double-click window and
    // select a word, and a focus-click before a real double-click makes a
    // paragraph-selecting triple. These timestamps let injection restore the
    // hand's true rhythm.
    DWORD    lastPressArrive = 0, lastPressInject = 0;
};

// ---------------------------------------------------------------- config

struct Config {
    // Per-seat device bindings, keyed by device interface path.
    std::map<std::wstring, int> mouseBinding;
    std::map<std::wstring, int> keyboardBinding;

    std::vector<COLORREF> seatColors = {
        RGB(0x4A, 0x90, 0xD9),   // seat 0 — blue
        RGB(0xE8, 0x6A, 0x33),   // seat 1 — orange
        RGB(0x5C, 0xB8, 0x5C),   // seat 2 — green
        RGB(0xB0, 0x60, 0xC0),   // seat 3 — purple
    };

    // Keyboard capture takes over ALL keyboards system-wide. If this process
    // stalls, so does typing. Off by default; see README "Safety".
    bool captureKeyboards = false;

    // Pointer speed multiplier per seat index.
    std::vector<double> sensitivity = {1.0, 1.0, 1.0, 1.0};

    // Draw a coloured ring around the seat that currently owns the real
    // system cursor.
    bool markActiveSeat = true;

    int  seatCount = 2;

    bool Load(const std::wstring& path);
    bool Save(const std::wstring& path) const;
    static std::wstring DefaultPath();
};

// ---------------------------------------------------------------- overlay

// One transparent, click-through, always-on-top window per seat, used to draw
// that seat's pointer when it does not own the real system cursor.
class CursorOverlay {
public:
    bool Create(HINSTANCE inst, int seatIndex, COLORREF color);
    void Destroy();
    void MoveTo(POINT ptVirtual);
    void Show(bool visible);
    bool IsVisible() const { return visible_; }

private:
    void Render(COLORREF color);
    bool RenderFromSystemCursor(COLORREF color, std::vector<BYTE>& out);
    void RenderFallbackArrow(COLORREF color, std::vector<BYTE>& out);

    HWND     hwnd_    = nullptr;
    HBITMAP  bitmap_  = nullptr;
    int      w_ = 0, h_ = 0;
    int      hotX_ = 1, hotY_ = 1;   // from the system cursor when available
    bool     visible_ = false;
};

// ---------------------------------------------------------------- utilities

RECT VirtualScreenRect();
void ClampToVirtualScreen(POINT& p);
void LogLine(const wchar_t* fmt, ...);
void OpenLogFile();
void CloseLogFile();

// Signature stamped into dwExtraInfo on every event this process injects, so
// that the injected event can be recognised and dropped when it comes back to
// us through the raw input stream. Without this the engine feeds on its own
// output — see RunProbeEcho().
constexpr ULONG_PTR kInjectTag = 0x4F4D5331;   // 'OMS1'

} // namespace om
