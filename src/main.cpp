// openMouse — multi-pointer input for Windows
// SPDX-License-Identifier: MIT
//
// Gives each physical mouse its own pointer on one shared Windows desktop, and
// optionally binds a keyboard to each pointer so that keystrokes are steered to
// the window that pointer last clicked.
//
// Design notes worth knowing before reading:
//
//  * Windows has ONE hardware cursor and ONE foreground window per desktop.
//    We do not change that. Exactly one seat "owns" the real cursor at any
//    moment; every other seat's pointer is drawn by us as a layered window.
//    Clicking transfers ownership. This is a deliberate turn-taking model —
//    see README, "What this cannot do".
//
//  * RIDEV_NOLEGACY suppresses OS-generated legacy messages for a whole device
//    CLASS, never a single device. So capturing one mouse means capturing all
//    of them and re-injecting for the active seat. Same for keyboards. This is
//    the reason a naive per-device implementation is impossible in user mode.
//
//  * Because we take over input system-wide, a hang in this process is a hang
//    in the user's input. A watchdog thread unregisters us if the message loop
//    stalls. Do not remove it.

#include "openmouse.h"
#include <shellapi.h>   // CommandLineToArgvW — not pulled in by WIN32_LEAN_AND_MEAN
#include <atomic>
#include <set>
#include <thread>
#include <cstdio>

namespace om {
namespace {

// ---------------------------------------------------------------- state

Config                    g_cfg;
Seat                      g_seats[kMaxSeats];
CursorOverlay             g_overlay[kMaxSeats];
std::map<HANDLE, int>     g_mouseSeat;      // device handle -> seat
std::map<HANDLE, int>     g_keyboardSeat;

// Interfaces deliberately left dead. This machine's receivers expose extra
// HID collections beyond the ones --identify measured; letting those default
// to seat 0 means a ghost interface can double-drive a seat. Once measured
// bindings exist, unmatched interfaces land here and their events are
// dropped. A device whose handle is in NEITHER map (hotplugged after the
// last rescan) still falls through to seat 0 so new hardware is never dead.
std::set<HANDLE>          g_deadMice;
std::set<HANDLE>          g_deadKeyboards;
int                       g_activeSeat = 0;  // owns the real system cursor
HWND                      g_hwnd = nullptr;
bool                      g_captureActive = false;

std::atomic<uint64_t>     g_heartbeat{0};
std::atomic<bool>         g_running{true};

// Runtime telemetry. Counters only in the hot path — the periodic timer does
// the formatting and the file write. Without this there is no way to tell
// "mouse 2 was routed to the wrong seat" apart from "mouse 2 was routed
// correctly and something else moved the cursor".
struct SeatStats { uint64_t moves = 0, buttons = 0, cursorSets = 0, overlaySets = 0; };
SeatStats                 g_stats[kMaxSeats];
uint64_t                  g_unboundEvents = 0;   // events that fell through to seat 0
std::map<HANDLE, uint64_t> g_perDevice;
bool                      g_overlayOk[kMaxSeats] = {};

// Counted at the point of arrival, before any filtering, so that "no WM_INPUT
// at all" can be told apart from "WM_INPUT arrived and was discarded".
uint64_t g_wmInputTotal = 0, g_rawMouse = 0, g_rawKbd = 0;
uint64_t g_dropInjected = 0, g_dropNullDev = 0;

// Per-event tracing. Off unless --verbose: these fire on every click, every
// ownership change and twice a second, and each line is a synchronous file
// write on the input thread. Invaluable while diagnosing, pure latency and
// unbounded log growth in normal use.
bool g_verbose = false;

// Scancodes currently held down, per seat, for the panic combo and to keep one
// seat's modifiers out of the other seat's typing. Tracked manually because
// with NOLEGACY the async key-state table only reflects what we re-inject.
bool                      g_keyDown[kMaxSeats][512] = {};

// True only when keyboards are captured with RIDEV_NOLEGACY and we are
// responsible for re-injecting them. When false we still receive keyboard raw
// input — that is what keeps the panic key alive — but Windows is delivering
// the keystrokes itself and we must not inject.
bool                      g_keyboardRouting = false;

constexpr USHORT SC_LCTRL = 0x1D, SC_LALT = 0x38, SC_LSHIFT = 0x2A, SC_Q = 0x10;

// ---------------------------------------------------------------- cursor
//
// Windows keeps moving the single hardware cursor for every physical mouse
// even under RIDEV_NOLEGACY — measured, see the note in OnRawMouse. Snapping
// it back after each stray event does restore the position, but the correction
// races the OS at mouse polling rates and reads as violent jitter.
//
// So stop fighting it. Hide the hardware cursor and draw every seat, including
// the one that owns it. The real cursor still drifts, but invisibly, and the
// only moment its position matters is the instant of a click — which we set
// explicitly anyway.
//
// Loaded dynamically so the binary still starts if magnification.dll is
// missing; the engine then falls back to the snap-back correction.
struct MagApi {
    HMODULE lib = nullptr;
    BOOL (WINAPI *Initialize)()            = nullptr;
    BOOL (WINAPI *Uninitialize)()          = nullptr;
    BOOL (WINAPI *ShowSystemCursor)(BOOL)  = nullptr;

    bool Load() {
        if (ShowSystemCursor) return true;
        lib = LoadLibraryW(L"magnification.dll");
        if (!lib) return false;
        Initialize       = (BOOL (WINAPI*)())        (void*)GetProcAddress(lib, "MagInitialize");
        Uninitialize     = (BOOL (WINAPI*)())        (void*)GetProcAddress(lib, "MagUninitialize");
        ShowSystemCursor = (BOOL (WINAPI*)(BOOL))    (void*)GetProcAddress(lib, "MagShowSystemCursor");
        return Initialize && ShowSystemCursor;
    }
};

MagApi g_mag;
bool   g_magReady     = false;
bool   g_cursorHidden = false;

bool HideSystemCursor() {
    if (g_cursorHidden) return true;
    if (!g_mag.Load()) { LogLine(L"[cursor] magnification.dll unavailable"); return false; }
    if (!g_magReady) {
        if (!g_mag.Initialize()) { LogLine(L"[cursor] MagInitialize failed: %lu", GetLastError()); return false; }
        g_magReady = true;
    }
    if (!g_mag.ShowSystemCursor(FALSE)) {
        LogLine(L"[cursor] MagShowSystemCursor(FALSE) failed: %lu", GetLastError());
        return false;
    }
    g_cursorHidden = true;
    LogLine(L"[cursor] hardware cursor hidden — all seats drawn as overlays");
    return true;
}

// Must run on every exit path. If this process dies without restoring, the
// desktop is left with no visible pointer at all.
void RestoreSystemCursor() {
    ClipCursor(nullptr);   // release the 1px cage before anything else
    if (g_magReady && g_mag.ShowSystemCursor) g_mag.ShowSystemCursor(TRUE);
    if (g_magReady && g_mag.Uninitialize)     g_mag.Uninitialize();
    if (g_cursorHidden) LogLine(L"[cursor] hardware cursor restored");
    g_cursorHidden = false;
    g_magReady     = false;
}

// ---------------------------------------------------------------- injection

void RequestExit();

// Last-resort brake against a runaway injection loop. The watchdog cannot
// catch one, because a storm arrives as WM_INPUT and keeps the heartbeat
// climbing — the process looks perfectly healthy while it floods the desktop
// with clicks. Injection is only ever driven by buttons and wheel, so honest
// traffic is nowhere near this rate; anything above it means we are feeding on
// our own output and the only safe move is to let go of every device.
bool InjectAllowed() {
    constexpr DWORD kWindowMs      = 1000;
    constexpr int   kMaxPerWindow  = 300;

    static DWORD windowStart = 0;
    static int   count       = 0;
    static bool  tripped     = false;

    if (tripped) return false;

    const DWORD now = GetTickCount();
    if (windowStart == 0 || (int)(now - windowStart) >= (int)kWindowMs) {
        windowStart = now;
        count       = 0;
    }
    if (++count > kMaxPerWindow) {
        tripped = true;
        LogLine(L"[emergency] %d injections in under %lu ms — runaway loop.",
                count, kWindowMs);
        LogLine(L"[emergency] releasing all input capture and exiting.");
        RequestExit();
        return false;
    }
    return true;
}

// SendInput absolute coordinates are normalised 0..65535. With
// MOUSEEVENTF_VIRTUALDESK that range spans the whole virtual desktop rather
// than just the primary monitor, which is what we want for multi-monitor.
void SendMouseAt(POINT pt, DWORD flags, DWORD mouseData = 0) {
    if (!InjectAllowed()) return;
    const RECT v = VirtualScreenRect();
    const int vw = (v.right - v.left) > 1 ? (v.right - v.left) : 2;
    const int vh = (v.bottom - v.top) > 1 ? (v.bottom - v.top) : 2;

    INPUT in{};
    in.type            = INPUT_MOUSE;
    in.mi.dx           = (LONG)(((int64_t)(pt.x - v.left) * 65535) / (vw - 1));
    in.mi.dy           = (LONG)(((int64_t)(pt.y - v.top)  * 65535) / (vh - 1));
    in.mi.mouseData    = mouseData;
    in.mi.dwFlags      = flags | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in.mi.dwExtraInfo  = kInjectTag;
    SendInput(1, &in, sizeof(INPUT));
}

void SendKeyScan(USHORT scan, bool up, bool e0) {
    if (!InjectAllowed()) return;
    INPUT in{};
    in.type           = INPUT_KEYBOARD;
    in.ki.wVk         = 0;
    in.ki.wScan       = scan;
    in.ki.dwFlags     = KEYEVENTF_SCANCODE
                      | (up ? KEYEVENTF_KEYUP : 0)
                      | (e0 ? KEYEVENTF_EXTENDEDKEY : 0);
    in.ki.dwExtraInfo = kInjectTag;
    SendInput(1, &in, sizeof(INPUT));
}

// SetForegroundWindow is rate-limited by the foreground lock. Attaching our
// input queue to the current foreground thread lifts that restriction, which
// is the documented workaround and what every focus-follows tool uses.
bool ClaimForeground(HWND target) {
    if (!target || !IsWindow(target)) return false;
    HWND root = GetAncestor(target, GA_ROOT);
    if (!root) root = target;
    if (GetForegroundWindow() == root) return true;

    // Windows 11 restricts SetForegroundWindow heavily, and it returns TRUE in
    // cases where it merely flashed the taskbar button and left the foreground
    // alone. Attaching to the outgoing foreground thread is the documented
    // workaround, but attaching to the TARGET thread as well is what makes it
    // stick when the two windows belong to different processes — which is the
    // whole point here, one seat in Edge and the other somewhere else.
    const DWORD me  = GetCurrentThreadId();
    const DWORD fg  = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD tgt = GetWindowThreadProcessId(root, nullptr);

    if (fg  && fg  != me)              AttachThreadInput(me, fg,  TRUE);
    if (tgt && tgt != me && tgt != fg) AttachThreadInput(me, tgt, TRUE);

    BringWindowToTop(root);
    SetForegroundWindow(root);
    SetActiveWindow(root);

    if (tgt && tgt != me && tgt != fg) AttachThreadInput(me, tgt, FALSE);
    if (fg  && fg  != me)              AttachThreadInput(me, fg,  FALSE);

    // Trust the observed state, not the return value.
    return GetForegroundWindow() == root;
}

// Class name of a window, for diagnostics only.
const wchar_t* ClassOf(HWND h) {
    static wchar_t buf[64];
    buf[0] = 0;
    if (h) GetClassNameW(h, buf, 63);
    return buf;
}

// ---------------------------------------------------------------- seats

// The OS keeps applying every physical mouse's motion to the one hardware
// cursor even under NOLEGACY — the drift measured at the start of this
// project. Most of the time drift is invisible, and the cursor is re-parked
// before anything reads it. But during a drag the cursor position IS the
// drag: seat B physically steering the hidden cursor steered seat A's grab,
// and B moving during A's click smeared it into a text selection.
//
// The cage fixes it at the source: a one-pixel ClipCursor rect pinned to the
// active seat's position. The OS clamps every other seat's deltas against
// the cage, so only the seat that owns the cursor can move it. The cage is
// moved on every active-seat event, reasserted by the timer (the system
// drops clip rects on some foreground changes), and released on EVERY exit
// path — a leaked cage would trap the user's only pointer in one pixel.
POINT g_pinnedAt{ 0, 0 };
bool  g_pinned = false;

extern bool g_eatMoves;   // defined with the gate, further down

void PinCursor(POINT p) {
    if (g_eatMoves) {
        // Physical moves never reach the cursor, so there is nothing to fence
        // it against — a plain warp is enough. No clip rect means external
        // automation, accessibility tools and anything else that positions
        // the pointer keep working normally while openMouse runs.
        SetCursorPos(p.x, p.y);
        if (g_pinned) { ClipCursor(nullptr); g_pinned = false; }
        g_pinnedAt = p;
        return;
    }

    // Fallback when move-eating had to be switched off. Clip FIRST: measured
    // on this machine, SetCursorPos to a point outside the active clip rect
    // is silently clamped (does nothing), while moving the clip rect drags
    // the cursor into it. So the rect move does the positioning and the
    // SetCursorPos afterwards is just exactness.
    const RECT r{ p.x, p.y, p.x + 1, p.y + 1 };
    ClipCursor(&r);
    SetCursorPos(p.x, p.y);
    g_pinnedAt = p;
    g_pinned   = true;
}

void UnpinCursor() {
    ClipCursor(nullptr);
    g_pinned = false;
}


// The taskbar, Start menu and tray flyouts render in shell z-bands ABOVE the
// topmost band; no normal window — including our overlays — can draw over
// them. The real hardware cursor CAN. So while the active seat's pointer is
// over shell UI, the hidden real cursor (already pinned to that seat) is made
// visible and the seat's overlay hidden, restoring a usable pointer over the
// taskbar, Start and the tray. Reverts the moment the pointer leaves.
bool g_overShell = false;

bool IsShellWindowAt(POINT p) {
    HWND h = WindowFromPoint(p);
    if (!h) return false;
    HWND root = GetAncestor(h, GA_ROOT);
    wchar_t cls[64] = {};
    GetClassNameW(root ? root : h, cls, 63);
    return !wcscmp(cls, L"Shell_TrayWnd") ||
           !wcscmp(cls, L"Shell_SecondaryTrayWnd") ||
           !wcscmp(cls, L"NotifyIconOverflowWindow") ||
           !wcscmp(cls, L"Windows.UI.Core.CoreWindow") ||          // Start, search
           !wcscmp(cls, L"XamlExplorerHostIslandWindow") ||        // Win11 taskbar
           !wcscmp(cls, L"TopLevelWindowForOverflowXamlIsland") ||
           !wcscmp(cls, L"Xaml_WindowedPopupClass");
}

void UpdateShellCursorFallback(int seat) {
    if (!g_cursorHidden || !g_magReady || !g_mag.ShowSystemCursor) return;
    const bool over = IsShellWindowAt(g_seats[seat].pos);
    if (over == g_overShell) return;
    g_overShell = over;
    g_mag.ShowSystemCursor(over ? TRUE : FALSE);
    g_overlay[seat].Show(!over);
    LogLine(over ? L"[shell] pointer over shell UI — real cursor shown"
                 : L"[shell] left shell UI — overlay resumed");
}

void SetActiveSeat(int seat) {
    if (seat == g_activeSeat || seat < 0 || seat >= g_cfg.seatCount) return;

    const int prev = g_activeSeat;
    g_activeSeat = seat;
    if (g_verbose) LogLine(L"[owner] seat %d -> seat %d", prev, seat);

    // With the hardware cursor hidden every seat keeps its own overlay at all
    // times, so there is nothing to show or hide on handover — only the real
    // cursor to reposition, so the click lands where this seat is pointing.
    if (!g_cursorHidden) {
        if (prev >= 0 && prev < g_cfg.seatCount) {
            g_overlay[prev].MoveTo(g_seats[prev].pos);
            g_overlay[prev].Show(true);
        }
        g_overlay[seat].Show(false);
    } else if (g_overShell) {
        // The outgoing owner was in shell-fallback mode (real cursor shown,
        // its overlay hidden). It is no longer the cursor's seat: give it its
        // overlay back, re-hide the real cursor, and re-evaluate for the new
        // owner's position.
        g_overlay[prev].Show(true);
        if (g_magReady && g_mag.ShowSystemCursor) g_mag.ShowSystemCursor(FALSE);
        g_overShell = false;
    }
    PinCursor(g_seats[seat].pos);
    if (g_cursorHidden) UpdateShellCursorFallback(seat);
}

void MovePointer(int seat, int dx, int dy) {
    Seat& s = g_seats[seat];
    const double k = (seat < (int)g_cfg.sensitivity.size()) ? g_cfg.sensitivity[seat] : 1.0;
    s.pos.x += (LONG)(dx * k);
    s.pos.y += (LONG)(dy * k);
    ClampToVirtualScreen(s.pos);

    // The owning seat still drives the real cursor — that is what keeps hover
    // states and click targeting following it — but when the cursor is hidden
    // it also draws its own arrow, because there is no longer a visible system
    // pointer to represent it.
    if (seat == g_activeSeat) {
        g_stats[seat].cursorSets++;
        PinCursor(s.pos);
        if (g_cursorHidden) {
            g_stats[seat].overlaySets++;
            g_overlay[seat].MoveTo(s.pos);
            UpdateShellCursorFallback(seat);
        }
    } else {
        g_stats[seat].overlaySets++;
        g_overlay[seat].MoveTo(s.pos);
    }
}

void RememberClickTarget(int seat) {
    HWND w = WindowFromPoint(g_seats[seat].pos);
    if (w) g_seats[seat].focusTarget = w;
}

// True while the seat that owns the click stream holds any button down —
// i.e. a drag is in progress. Self-heals from the async key-state table,
// which under NOLEGACY reflects exactly what we injected: if a release was
// lost (device unplugged mid-drag, event dropped), the stale flag would
// otherwise lock every other seat out of clicking forever.
bool OwnerDragging() {
    Seat& o = g_seats[g_activeSeat];
    if (o.lDown  && !(GetAsyncKeyState(VK_LBUTTON)  & 0x8000)) o.lDown  = false;
    if (o.rDown  && !(GetAsyncKeyState(VK_RBUTTON)  & 0x8000)) o.rDown  = false;
    if (o.mDown  && !(GetAsyncKeyState(VK_MBUTTON)  & 0x8000)) o.mDown  = false;
    if (o.x1Down && !(GetAsyncKeyState(VK_XBUTTON1) & 0x8000)) o.x1Down = false;
    if (o.x2Down && !(GetAsyncKeyState(VK_XBUTTON2) & 0x8000)) o.x2Down = false;
    return o.lDown || o.rDown || o.mDown || o.x1Down || o.x2Down;
}

void HandleButton(int seat, USHORT flags, USHORT data) {
    // Physical arrival time, captured before any settle sleeps distort it —
    // the anti-compression pacing below compares against this.
    const DWORD pressArrive = GetTickCount();

    // Any button press takes ownership of the real cursor, so the click lands
    // where this seat's pointer is and hover states follow the clicker.
    const bool isPress =
        (flags & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN |
                  RI_MOUSE_MIDDLE_BUTTON_DOWN |
                  RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN)) != 0;

    // Drag guard. There is one hardware cursor and one logical button state;
    // injecting another seat's buttons or wheel into the middle of a drag
    // teleports the drag point, breaks the grab, and drops the payload
    // somewhere neither person intended. So while the owner holds a button,
    // every other seat's buttons and wheel are swallowed whole. Movement is
    // untouched — the other pointers keep gliding on their overlays. The
    // swallowed click simply never happened: click again once the drag ends.
    if (seat != g_activeSeat && OwnerDragging()) {
        if (isPress)
            LogLine(L"[drag-guard] seat %d press swallowed — seat %d is mid-drag",
                    seat, g_activeSeat);
        return;
    }

    if (isPress && seat != g_activeSeat) SetActiveSeat(seat);

    const POINT p = g_seats[seat].pos;

    // Applications route a click using the pointer position they last saw, not
    // the coordinates carried on the button event — Chromium and Electron in
    // particular. Two things conspire here: the hardware cursor drifts
    // invisibly under us, and a button injected with no preceding move leaves
    // the target still believing the pointer is somewhere else. The result was
    // that focusing a text box took two clicks, the first only serving to tell
    // the app where the pointer had got to.
    //
    // So before any press: put the real cursor where this seat is pointing and
    // deliver an explicit move, which queues ahead of the button and makes the
    // target agree with us before the click lands.
    if (isPress) {
        // Cross-window clicks need help. A click-through injection — letting
        // the click itself activate the window like real hardware — works when
        // the foreground is an app that yields (verified against Notepad,
        // 8/8), but an Electron foreground window does not let go: the click
        // dies mid-activation with focus never moving (verified against a
        // live Claude Code window, 0/3). So when the press targets a window
        // that is not foreground:
        //
        //   1. take the foreground explicitly (attach-dance, synchronous),
        //   2. poll until the switch is actually observable,
        //   3. wait out Chromium's post-activation mouse-ignore window —
        //      150 ms is measurably flaky, 250 ms passed 10 of 11 trials,
        //
        // and only then inject. The residual failure degrades to "click
        // again", never to a double action. The inline wait also keeps the
        // press/release order intact: the physical release queues behind us
        // and is injected after the DOWN no matter how long we sleep here.
        HWND under = WindowFromPoint(p);
        if (under) {
            DWORD pidUnder = 0;
            GetWindowThreadProcessId(under, &pidUnder);
            if (pidUnder != GetCurrentProcessId()) {
                g_seats[seat].focusTarget = under;

                HWND root = GetAncestor(under, GA_ROOT);
                if (root && root != GetForegroundWindow()) {
                    ClaimForeground(under);
                    int polls = 0;
                    while (polls < 20 && GetForegroundWindow() != root) { Sleep(10); ++polls; }
                    if (g_verbose)
                        LogLine(L"[click] seat %d cross-window -> %p [%s]  fg settled in %d ms",
                                seat, root, ClassOf(root), polls * 10);

                    // The settle is an inline sleep, deliberately. A deferred
                    // press flushed from WM_TIMER was tried and reverted:
                    // timers only fire on an empty queue, and raw input keeps
                    // the queue full whenever any mouse moves, so deferred
                    // clicks starved. Known cost of the sleep: a drag BEGUN
                    // cross-window during these 250 ms is time-compressed and
                    // will not register — start cross-window drags with a
                    // separate click first, or accept one dragger at a time.
                    Sleep(250);
                }
            }
        }

        PinCursor(p);
        SendMouseAt(p, MOUSEEVENTF_MOVE);
    }

    // Releases inject only if we injected the matching press. A press
    // swallowed by the drag guard leaves the seat's flag clear, so its
    // release is swallowed with it instead of firing a stray UP.
    if (flags & RI_MOUSE_LEFT_BUTTON_DOWN)   {
        // Anti-compression: the cross-window settle delays injection, so two
        // presses the hand made comfortably apart can inject close enough to
        // read as a double- or triple-click. If the PHYSICAL gap since the
        // previous press was outside the double-click window but the INJECTED
        // gap would be inside it, pad the difference so apps see the rhythm
        // the hand actually produced. Genuine double-clicks (physical gap
        // inside the window) are untouched.
        Seat& s = g_seats[seat];
        const DWORD dct = GetDoubleClickTime();
        DWORD now = GetTickCount();
        if (s.lastPressInject != 0) {
            const DWORD physGap = pressArrive - s.lastPressArrive;
            const DWORD injGap  = now - s.lastPressInject;
            if (physGap > dct && injGap < dct) {
                const DWORD pad = (dct - injGap) < 300 ? (dct - injGap) : 300;
                if (g_verbose)
                    LogLine(L"[pace] seat %d padding %lu ms (physGap=%lu injGap=%lu)",
                            seat, pad, physGap, injGap);
                Sleep(pad);
                now = GetTickCount();
            }
        }
        s.lastPressArrive = pressArrive;
        s.lastPressInject = now;
        if (g_verbose) LogLine(L"[inj] seat %d Ldown %ld,%ld t=%lu", seat, p.x, p.y, now);
        SendMouseAt(p, MOUSEEVENTF_LEFTDOWN);
        s.lDown = true;
        RememberClickTarget(seat);
    }
    if (flags & RI_MOUSE_LEFT_BUTTON_UP)     { if (g_seats[seat].lDown) {
                                                   if (g_verbose) LogLine(L"[inj] seat %d Lup   %ld,%ld t=%lu", seat, p.x, p.y, GetTickCount());
                                                   SendMouseAt(p, MOUSEEVENTF_LEFTUP); g_seats[seat].lDown = false; } }
    if (flags & RI_MOUSE_RIGHT_BUTTON_DOWN)  { SendMouseAt(p, MOUSEEVENTF_RIGHTDOWN);  g_seats[seat].rDown = true;  RememberClickTarget(seat); }
    if (flags & RI_MOUSE_RIGHT_BUTTON_UP)    { if (g_seats[seat].rDown) {
                                                   SendMouseAt(p, MOUSEEVENTF_RIGHTUP); g_seats[seat].rDown = false; } }
    if (flags & RI_MOUSE_MIDDLE_BUTTON_DOWN) { SendMouseAt(p, MOUSEEVENTF_MIDDLEDOWN); g_seats[seat].mDown = true; }
    if (flags & RI_MOUSE_MIDDLE_BUTTON_UP)   { if (g_seats[seat].mDown) {
                                                   SendMouseAt(p, MOUSEEVENTF_MIDDLEUP); g_seats[seat].mDown = false; } }
    // Side buttons — browser back/forward. Dropped entirely before this.
    if (flags & RI_MOUSE_BUTTON_4_DOWN)      { SendMouseAt(p, MOUSEEVENTF_XDOWN, XBUTTON1); g_seats[seat].x1Down = true; }
    if (flags & RI_MOUSE_BUTTON_4_UP)        { if (g_seats[seat].x1Down) {
                                                   SendMouseAt(p, MOUSEEVENTF_XUP, XBUTTON1); g_seats[seat].x1Down = false; } }
    if (flags & RI_MOUSE_BUTTON_5_DOWN)      { SendMouseAt(p, MOUSEEVENTF_XDOWN, XBUTTON2); g_seats[seat].x2Down = true; }
    if (flags & RI_MOUSE_BUTTON_5_UP)        { if (g_seats[seat].x2Down) {
                                                   SendMouseAt(p, MOUSEEVENTF_XUP, XBUTTON2); g_seats[seat].x2Down = false; } }
    // Wheel routing follows the REAL cursor, not the coordinates on the event:
    // with hover-scroll routing (MouseWheelRouting=2, this machine's setting,
    // and the Windows 11 default) the system scrolls whatever window lies
    // under the hardware cursor. That cursor is invisible and parked at the
    // OWNER seat's position — so a non-owner's scroll went to the owner's
    // window. Park it on the scrolling seat first; the owner's next move puts
    // it back. No ownership transfer: scrolling is a hover action, not a
    // focus action, same as Windows treats it.
    if (flags & (RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL))
        PinCursor(p);
    if (flags & RI_MOUSE_WHEEL)              { SendMouseAt(p, MOUSEEVENTF_WHEEL, (DWORD)(SHORT)data); }
    if (flags & RI_MOUSE_HWHEEL)             { SendMouseAt(p, MOUSEEVENTF_HWHEEL, (DWORD)(SHORT)data); }
}

// ---------------------------------------------------------------- raw input

void OnRawMouse(HANDLE dev, const RAWMOUSE& m) {
    // Our own injected input returns through this very path, with a NULL
    // device handle and our signature in ulExtraInformation. Verified by
    // --probe-echo on this machine. Without this guard the first click is an
    // infinite loop — inject, receive, route to a seat, inject again — and the
    // watchdog cannot break it because the storm keeps the heartbeat climbing.
    // Dropping every NULL-handle event also ignores injection from other
    // processes, which is correct: those already reach the OS on their own.
    if (dev == nullptr)                       { ++g_dropNullDev;  return; }
    if (m.ulExtraInformation == kInjectTag)   { ++g_dropInjected; return; }
    if (g_deadMice.count(dev))                { return; }   // ghost interface

    auto it = g_mouseSeat.find(dev);
    // Unbound devices drive seat 0 rather than being dropped, so plugging in a
    // new mouse never leaves the user with dead hardware.
    const int seat = (it != g_mouseSeat.end()) ? it->second : 0;
    if (seat < 0 || seat >= g_cfg.seatCount) return;

    g_perDevice[dev]++;
    if (it == g_mouseSeat.end()) ++g_unboundEvents;
    if (m.usButtonFlags) g_stats[seat].buttons++; else g_stats[seat].moves++;

    if (m.usFlags & MOUSE_MOVE_ABSOLUTE) {
        const RECT v = VirtualScreenRect();
        POINT p;
        p.x = v.left + (LONG)(((int64_t)m.lLastX * (v.right  - v.left)) / 65535);
        p.y = v.top  + (LONG)(((int64_t)m.lLastY * (v.bottom - v.top))  / 65535);
        g_seats[seat].pos = p;
        ClampToVirtualScreen(g_seats[seat].pos);
        if (seat == g_activeSeat) {
            PinCursor(g_seats[seat].pos);
            UpdateShellCursorFallback(seat);
        } else {
            g_overlay[seat].MoveTo(p);
        }
    } else if (m.lLastX || m.lLastY) {
        MovePointer(seat, m.lLastX, m.lLastY);
    }

    if (m.usButtonFlags) {
        // One line per PHYSICAL button event, before any routing. If a single
        // physical press ever shows up here twice from two device handles,
        // that is a duplicate HID collection double-driving a seat.
        if (g_verbose)
            LogLine(L"[phys] dev=%p seat=%d flags=%04X pos=%ld,%ld t=%lu",
                    dev, seat, m.usButtonFlags, g_seats[seat].pos.x, g_seats[seat].pos.y,
                    GetTickCount());
        HandleButton(seat, m.usButtonFlags, m.usButtonData);
    }

    // RIDEV_NOLEGACY does NOT stop Windows moving the single hardware cursor
    // in response to physical mouse motion. It suppresses the legacy *message*
    // stream only. Measured on this machine: with a non-active seat's mouse
    // moving and SetCursorPos never once called, the real cursor still tracked
    // it across 300+ pixels.
    //
    // With move-eating on, physical motion never reaches the cursor at all
    // and there is nothing to correct. Otherwise a non-owning seat's motion
    // has already dragged the cursor off target: snapping it back works but
    // races the OS at polling rates and shows as harsh jitter, so it is a
    // last resort — only when the cursor is visible and move-eating is off.
    if (seat != g_activeSeat && !g_cursorHidden && !g_eatMoves) {
        PinCursor(g_seats[g_activeSeat].pos);
        g_stats[g_activeSeat].cursorSets++;
    }
}

void RequestExit();

void OnRawKeyboard(HANDLE dev, const RAWKEYBOARD& k) {
    // Same self-injection guard as the mouse path. Dead keyboard interfaces
    // are NOT dropped here: with routing off Windows delivers their keys
    // itself anyway, and the panic combo must work from every keyboard.
    if (dev == nullptr || k.ExtraInformation == kInjectTag) return;

    const USHORT scan = k.MakeCode;
    const bool   up   = (k.Flags & RI_KEY_BREAK) != 0;
    if (scan >= 512) return;

    auto it = g_keyboardSeat.find(dev);
    int seat = (it != g_keyboardSeat.end()) ? it->second : 0;
    if (seat < 0 || seat >= g_cfg.seatCount) seat = 0;

    // Held-key state is per seat. Tracking it globally lets one person's Shift
    // or Ctrl apply to the other person's typing once two keyboards are live.
    g_keyDown[seat][scan] = !up;

    // Panic combo: Ctrl+Alt+Shift+Q releases every captured device. Checked
    // before any routing so it works even if routing is what went wrong.
    if (!up && scan == SC_Q &&
        g_keyDown[seat][SC_LCTRL] && g_keyDown[seat][SC_LALT] && g_keyDown[seat][SC_LSHIFT]) {
        LogLine(L"[panic] Ctrl+Alt+Shift+Q — releasing input capture");
        RequestExit();
        return;
    }

    if (!g_keyboardRouting) return;              // steering disabled
    if (g_deadKeyboards.count(dev)) return;      // ghost interface

    // STEERING ONLY — the physical keystroke is never suppressed and never
    // re-injected. Windows delivers it normally; all we do is make sure the
    // right window is in front first.
    //
    // The eat-and-re-inject design that per-seat layouts would need is
    // impossible here: eating a key in a low-level keyboard hook also kills
    // its raw input, which is the only source of device identity. So instead
    // of owning the keystroke we merely aim it.
    //
    // Consequence to expect: the very first keystroke after a seat's window
    // loses focus may still land in the previous window, because it can
    // already be queued there by the time we claim. Subsequent keys land
    // correctly. This is a real limitation, but a benign one — worst case a
    // stray character, never a lost keyboard. NO sleeps here: this runs on
    // the input thread, and stalling it delays the other seat's mouse too.
    if (!up) {
        HWND target = g_seats[seat].focusTarget;
        if (target && IsWindow(target)) {
            HWND root = GetAncestor(target, GA_ROOT);
            if (root && root != GetForegroundWindow()) {
                ClaimForeground(target);
                if (g_verbose)
                    LogLine(L"[key] seat %d steered typing to %p [%s]", seat, root, ClassOf(root));
            }
        }
    }
}

void OnRawInput(HRAWINPUT h) {
    UINT size = 0;
    if (GetRawInputData(h, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0) return;
    if (size == 0 || size > 4096) return;

    BYTE stack[4096];
    if (GetRawInputData(h, RID_INPUT, stack, &size, sizeof(RAWINPUTHEADER)) != size) return;

    ++g_wmInputTotal;
    auto* ri = reinterpret_cast<RAWINPUT*>(stack);
    if (ri->header.dwType == RIM_TYPEMOUSE)         { ++g_rawMouse; OnRawMouse(ri->header.hDevice, ri->data.mouse); }
    else if (ri->header.dwType == RIM_TYPEKEYBOARD) { ++g_rawKbd;   OnRawKeyboard(ri->header.hDevice, ri->data.keyboard); }
}

// ---------------------------------------------------------------- gate
//
// The load-bearing suppression. RIDEV_NOLEGACY was assumed to silence
// physical mouse clicks system-wide; measured with the --monitor hook, it
// does not — every physical click was reaching applications through the
// legacy pipeline IN ADDITION to our per-seat injection. Single clicks
// arrived as doubles, double clicks as quadruples (paragraph select), drags
// as two interleaved button streams. This lived in the engine from the first
// build, disguised as a dozen smaller symptoms.
//
// A WH_MOUSE_LL hook can EAT events by returning nonzero. Physical (non-
// injected) button and wheel events are swallowed here; injected events —
// ours and other software's — pass. Movement passes untouched: the cursor
// cage already owns motion, and eating moves would risk eating our own
// SetCursorPos stream.
//
// Safety: the hook dies with the process; the watchdog and every exit path
// unhook explicitly; if the engine stops injecting while the hook eats,
// keyboards still work (panic key) because this gate touches only the mouse.

HHOOK g_mouseGate = nullptr;

// Eat physical MOVES too, not just buttons. This is what replaces the 1px
// ClipCursor cage: drift is stopped at the source instead of being corrected
// afterwards, so the hardware cursor only ever moves where we put it.
//
// Safe because SetCursorPos generates no low-level hook event at all
// (measured on this machine — a warp cannot be eaten by our own hook), and
// because eating in the MOUSE hook leaves raw input intact (measured: 242
// physical button events still delivered while the gate ate them). Both were
// verified before enabling; the keyboard equivalent is impossible for exactly
// the opposite reason. Undocumented behaviour either way, so it is watched at
// runtime — see the self-blinding guard in WndProc.
bool                  g_eatMoves = true;
std::atomic<uint64_t> g_gateEatenMoves{0};

LRESULT CALLBACK MouseGate(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && lp) {
        const auto* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        if (!(m->flags & LLMHF_INJECTED)) {
            switch (wp) {
            case WM_LBUTTONDOWN: case WM_LBUTTONUP:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP:
            case WM_XBUTTONDOWN: case WM_XBUTTONUP:
            case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL:
                return 1;                      // physical button/wheel: eaten
            case WM_MOUSEMOVE:
                if (g_eatMoves) { g_gateEatenMoves.fetch_add(1); return 1; }
                break;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// There is deliberately NO keyboard gate. One was tried and it broke typing
// completely, in a way worth recording: eating a physical key in a
// WH_KEYBOARD_LL hook ALSO suppresses that key's raw input delivery, so the
// engine went blind (measured: kbd=0 raw events across a whole session while
// the user typed) and never re-injected anything. Keystrokes vanished.
//
// The mouse does not behave this way — eating a physical button in
// WH_MOUSE_LL leaves its WM_INPUT intact (measured: 242 physical button
// events still seen by the engine in the same session). That asymmetry is
// why the mouse gate stays and the keyboard one cannot exist.
//
// Without raw input there is no device identity — KBDLLHOOKSTRUCT carries
// none — so eat-and-re-inject is impossible for keyboards. Keyboard support
// is therefore steering-only; see OnRawKeyboard.

void RemoveMouseGate() {
    if (g_mouseGate) { UnhookWindowsHookEx(g_mouseGate); g_mouseGate = nullptr; }
}

// The hook runs on its installing thread's message pump. The engine's input
// thread sleeps up to ~550 ms inside cross-window click handling, and a
// low-level hook that cannot be dispatched within the system timeout is
// silently BYPASSED — physical clicks would leak through at exactly the
// moments the engine is busy. So the gate lives on a dedicated thread whose
// only job is pumping, which nothing else can block.
DWORD g_gateThreadId = 0;

void GateThreadProc() {
    g_gateThreadId = GetCurrentThreadId();
    g_mouseGate = SetWindowsHookExW(WH_MOUSE_LL, MouseGate, GetModuleHandleW(nullptr), 0);
    if (!g_mouseGate) {
        LogLine(L"[gate] SetWindowsHookEx failed: %lu — physical clicks are NOT suppressed", GetLastError());
        return;
    }
    LogLine(L"[gate] physical mouse suppression active — buttons, wheel%s",
            g_eatMoves ? L" and moves (no cursor cage)" : L" only (cursor cage in use)");
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    RemoveMouseGate();
    LogLine(L"[gate] removed");
}

void StopGateThread() {
    RemoveMouseGate();   // hook first, so nothing is eaten while the thread winds down
    if (g_gateThreadId) PostThreadMessageW(g_gateThreadId, WM_QUIT, 0, 0);
}

// ---------------------------------------------------------------- capture

bool RegisterCapture(HWND hwnd, bool keyboards) {
    RAWINPUTDEVICE rid[2];
    int n = 0;

    // RIDEV_DEVNOTIFY is what actually delivers WM_INPUT_DEVICE_CHANGE. Without
    // it that handler never runs and a mouse plugged in later is never bound.
    //
    // No RIDEV_NOLEGACY, deliberately. Measured on this machine: a background
    // INPUTSINK registration's NOLEGACY does NOT suppress physical clicks
    // system-wide — apps kept receiving the legacy stream alongside our
    // injections, doubling every click. Raw input here is used purely as the
    // per-device attribution tap; the actual suppression is the WH_MOUSE_LL
    // gate (see MouseGate), which provably eats the physical events.
    rid[n].usUsagePage = 0x01;
    rid[n].usUsage     = 0x02;                       // generic mouse
    rid[n].dwFlags     = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
    rid[n].hwndTarget  = hwnd;
    ++n;

    // Keyboards are registered either way, observe-only at this layer: the
    // raw stream is what detects the panic combo and (with routing on) feeds
    // per-seat re-injection. No NOLEGACY — same per-application scoping as
    // the mouse, it would suppress nothing for other apps. The actual
    // suppression while routing is live is the WH_KEYBOARD_LL gate.
    rid[n].usUsagePage = 0x01;
    rid[n].usUsage     = 0x06;                       // generic keyboard
    rid[n].dwFlags     = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
    rid[n].hwndTarget  = hwnd;
    ++n;

    if (!RegisterRawInputDevices(rid, n, sizeof(RAWINPUTDEVICE))) {
        LogLine(L"[error] RegisterRawInputDevices failed: %lu", GetLastError());
        return false;
    }
    g_keyboardRouting = keyboards;
    g_captureActive   = true;
    return true;
}

void UnregisterCapture() {
    if (!g_captureActive) return;
    RAWINPUTDEVICE rid[2];
    rid[0].usUsagePage = 0x01; rid[0].usUsage = 0x02;
    rid[0].dwFlags = RIDEV_REMOVE; rid[0].hwndTarget = nullptr;
    rid[1].usUsagePage = 0x01; rid[1].usUsage = 0x06;
    rid[1].dwFlags = RIDEV_REMOVE; rid[1].hwndTarget = nullptr;
    RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));
    g_keyboardRouting = false;
    g_captureActive   = false;
    // Every exit path funnels through here; a cage left behind would trap
    // the machine's only pointer in one pixel.
    UnpinCursor();
}

void RequestExit() {
    g_running = false;
    StopGateThread();
    UnregisterCapture();
    RestoreSystemCursor();
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

// If the message loop stalls while we hold a system-wide NOLEGACY
// registration, the user has no working mouse or keyboard. Release capture
// from a separate thread so the machine stays usable.
void WatchdogThread() {
    uint64_t last = g_heartbeat.load();
    int stalled = 0;
    while (g_running.load()) {
        Sleep(500);
        const uint64_t now = g_heartbeat.load();
        if (now == last) {
            if (++stalled >= 6) {           // ~3 seconds with no progress
                LogLine(L"[watchdog] message loop stalled — releasing capture");
                StopGateThread();           // physical clicks must flow again
                UnregisterCapture();
                // Releasing capture gives the mice back, but a hidden caged
                // cursor would leave the user with working input and no
                // pointer to see. Restore visibility too.
                RestoreSystemCursor();
                stalled = 0;
            }
        } else {
            stalled = 0;
            last = now;
        }
    }
}

// ---------------------------------------------------------------- binding

// Auto-binding is a runtime fallback only; it is deliberately never written
// back to the config. This machine exposes four HID mouse interfaces for two
// physical mice, and enumeration order does not match physical order — so
// persisting a guess would quietly overwrite whatever --identify established
// and there would be no way to tell a measured binding from a guessed one.
void ApplyBindings() {
    g_mouseSeat.clear();
    g_keyboardSeat.clear();
    g_deadMice.clear();
    g_deadKeyboards.clear();

    const auto devices = EnumerateDevices();
    const bool haveMouseCfg = !g_cfg.mouseBinding.empty();
    const bool haveKbdCfg   = !g_cfg.keyboardBinding.empty();
    int autoMouse = 0, autoKeyboard = 0;

    for (const auto& d : devices) {
        if (d.type == RIM_TYPEMOUSE) {
            auto it = g_cfg.mouseBinding.find(d.path);
            if (it != g_cfg.mouseBinding.end()) {
                g_mouseSeat[d.handle] = it->second;
                LogLine(L"[bind] mouse    seat %d  %s", it->second, d.friendlyName.c_str());
            } else if (haveMouseCfg) {
                // Measured bindings exist, so an unmatched interface is a
                // ghost collection of already-bound hardware. Kill it rather
                // than let it double-drive seat 0.
                g_deadMice.insert(d.handle);
                LogLine(L"[dead] mouse    %s  (unidentified interface, dropped)",
                        d.friendlyName.c_str());
            } else {
                // No measured config at all: auto-bind so first run works.
                const int seat = (autoMouse < g_cfg.seatCount) ? autoMouse : 0;
                g_mouseSeat[d.handle] = seat;
                LogLine(L"[auto] mouse    seat %d  %s  (not identified)",
                        seat, d.friendlyName.c_str());
            }
            ++autoMouse;
        } else {
            auto it = g_cfg.keyboardBinding.find(d.path);
            if (it != g_cfg.keyboardBinding.end()) {
                g_keyboardSeat[d.handle] = it->second;
                LogLine(L"[bind] keyboard seat %d  %s", it->second, d.friendlyName.c_str());
            } else if (haveKbdCfg) {
                g_deadKeyboards.insert(d.handle);
                LogLine(L"[dead] keyboard %s  (unidentified interface, dropped)",
                        d.friendlyName.c_str());
            } else {
                const int seat = (autoKeyboard < g_cfg.seatCount) ? autoKeyboard : 0;
                g_keyboardSeat[d.handle] = seat;
                LogLine(L"[auto] keyboard seat %d  %s  (not identified)",
                        seat, d.friendlyName.c_str());
            }
            ++autoKeyboard;
        }
    }
    if (!haveMouseCfg)
        LogLine(L"[warn] no measured bindings — run openmouse.exe --identify");
}

// ---------------------------------------------------------------- window

void TrayMenu(HWND hwnd);   // defined with the other tray plumbing below

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case kTrayCallbackMsg:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU || lp == WM_LBUTTONUP)
            TrayMenu(hwnd);
        return 0;
    case WM_INPUT:
        g_heartbeat.fetch_add(1);
        OnRawInput(reinterpret_cast<HRAWINPUT>(lp));
        return 0;

    case WM_INPUT_DEVICE_CHANGE:
        // One of these arrives per device the moment we register, so a machine
        // with nine HID interfaces would re-enumerate nine times at startup —
        // each pass hitting the registry for every device. Coalesce the burst
        // onto a single deferred pass.
        SetTimer(hwnd, 3, 500, nullptr);
        return 0;

    case WM_TIMER:
        if (wp == 3) {                      // coalesced device-change rescan
            KillTimer(hwnd, 3);
            ApplyBindings();
            return 0;
        }
        if (wp == 2) {                      // auto-exit timer
            LogLine(L"[exit] run time limit reached — releasing input capture");
            RequestExit();
            return 0;
        }
        g_heartbeat.fetch_add(1);
        if (wp == 1) {
            // The system silently drops clip rects on some foreground
            // changes; reassert the cage wherever it was last placed. Placed,
            // not the active seat's pos — a scrolling seat legitimately holds
            // the cage between wheel ticks. Only relevant in cage mode; with
            // move-eating there is no clip rect to lose.
            if (g_captureActive && g_pinned) PinCursor(g_pinnedAt);

            // Self-blinding guard. Eating physical moves relies on
            // undocumented behaviour: for the mouse it leaves raw input
            // intact, but the keyboard equivalent silently suppresses raw
            // input and swallows the input stream whole. If that ever
            // happened here — a Windows update, a filter driver — the gate
            // would eat every move while the engine saw none, and both
            // pointers would freeze with no way to recover from inside.
            // So: many moves eaten and zero raw input in the same second
            // means we have blinded ourselves. Fall back to the cage.
            static uint64_t lastRaw = 0, lastEaten = 0;
            static int guardTick = 0;
            if (++guardTick >= 4) {                 // once a second
                guardTick = 0;
                const uint64_t raw   = g_rawMouse;
                const uint64_t eaten = g_gateEatenMoves.load();
                if (g_eatMoves && (eaten - lastEaten) > 20 && raw == lastRaw) {
                    g_eatMoves = false;
                    LogLine(L"[gate] move-eating suppressed raw input (%llu eaten, 0 received)"
                            L" — disabled, reverting to cursor cage",
                            (unsigned long long)(eaten - lastEaten));
                }
                lastRaw   = raw;
                lastEaten = eaten;
            }
        }
        if (wp == 1 && g_verbose) {
            // Logged unconditionally while verbose, even when idle: a silent
            // log cannot distinguish "no input arrived" from "instrumentation
            // is broken" — that ambiguity cost a whole debugging round.
            static int tick = 0;
            if (++tick >= 8) {              // every 2 seconds
                tick = 0;
                LogLine(L"[stat] active=%d  wm_input=%llu (mouse=%llu kbd=%llu)  dropped: null=%llu injected=%llu  unbound=%llu",
                        g_activeSeat,
                        (unsigned long long)g_wmInputTotal,
                        (unsigned long long)g_rawMouse,
                        (unsigned long long)g_rawKbd,
                        (unsigned long long)g_dropNullDev,
                        (unsigned long long)g_dropInjected,
                        (unsigned long long)g_unboundEvents);
                for (int i = 0; i < g_cfg.seatCount; ++i)
                    LogLine(L"       seat %d  mv=%llu btn=%llu setcursor=%llu overlay=%llu pos=%ld,%ld",
                            i, (unsigned long long)g_stats[i].moves,
                            (unsigned long long)g_stats[i].buttons,
                            (unsigned long long)g_stats[i].cursorSets,
                            (unsigned long long)g_stats[i].overlaySets,
                            g_seats[i].pos.x, g_seats[i].pos.y);
                POINT real{};
                GetCursorPos(&real);
                LogLine(L"       system cursor actually at %ld,%ld", real.x, real.y);
                for (const auto& kv : g_perDevice) {
                    auto s = g_mouseSeat.find(kv.first);
                    LogLine(L"       device %p -> seat %d  events=%llu", kv.first,
                            s != g_mouseSeat.end() ? s->second : -1,
                            (unsigned long long)kv.second);
                }
            }
        }
        return 0;

    case WM_CLOSE:
        UnregisterCapture();
        RestoreSystemCursor();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- modes

int RunList() {
    const auto devices = EnumerateDevices();
    LogLine(L"openMouse — input devices");
    LogLine(L"");
    for (const auto& d : devices) {
        LogLine(L"  %s  %s", d.type == RIM_TYPEMOUSE ? L"[mouse]   " : L"[keyboard]",
                d.friendlyName.c_str());
        LogLine(L"      %s", d.path.c_str());
    }
    LogLine(L"");
    LogLine(L"Config file: %s", Config::DefaultPath().c_str());
    return 0;
}

// The single most important thing to verify on a new machine: does SendInput
// still reach applications while a RIDEV_NOLEGACY registration is active?
// The whole design rests on the answer being yes. If this prints FAIL, no
// user-mode multi-pointer implementation is possible on this system.
int RunProbe() {
    LogLine(L"openMouse — SendInput / RIDEV_NOLEGACY compatibility probe");
    LogLine(L"");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"openMouse.Probe";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { LogLine(L"FAIL: could not create message window"); return 1; }

    POINT before{};
    GetCursorPos(&before);
    LogLine(L"  cursor before        : %ld,%ld", before.x, before.y);

    if (!RegisterCapture(hwnd, false)) {
        LogLine(L"FAIL: RIDEV_NOLEGACY registration rejected.");
        LogLine(L"      Check App Control / WDAC policy and any input filter drivers.");
        return 1;
    }
    LogLine(L"  NOLEGACY registered  : yes");

    Sleep(300);
    POINT target{ before.x + 120, before.y + 60 };
    ClampToVirtualScreen(target);
    SendMouseAt(target, MOUSEEVENTF_MOVE);
    Sleep(300);

    POINT after{};
    GetCursorPos(&after);
    LogLine(L"  cursor after SendInput: %ld,%ld  (wanted %ld,%ld)",
            after.x, after.y, target.x, target.y);

    const bool moved = (labs(after.x - target.x) <= 2 && labs(after.y - target.y) <= 2);
    UnregisterCapture();
    SetCursorPos(before.x, before.y);

    LogLine(L"");
    if (moved) {
        LogLine(L"  PASS — SendInput reaches the system while NOLEGACY is active.");
        LogLine(L"         The multi-pointer design is viable on this machine.");
        return 0;
    }
    LogLine(L"  FAIL — SendInput did not move the cursor under NOLEGACY.");
    LogLine(L"         User-mode multi-pointer is not possible here; a kernel");
    LogLine(L"         filter driver would be required instead.");
    return 2;
}

// ---------------------------------------------------------------- echo probe
//
// RunProbe answers "does SendInput still reach the system under NOLEGACY".
// It does NOT answer the question that actually decides whether the engine can
// run safely: does our own injected input come BACK to us as WM_INPUT?
//
// If it does, the engine feeds on its own output. A single click becomes
// inject -> WM_INPUT -> route to a seat -> inject -> ... forever, and the
// watchdog cannot save us because a storm keeps the heartbeat climbing. So
// this probe injects exactly once, never in response to input, and reports
// what came back and how it can be told apart from real hardware.

struct EchoStats {
    int total = 0, nullDev = 0, tagged = 0, moves = 0, buttons = 0;
    std::map<HANDLE, int> byDevice;
    ULONG_PTR sampleExtra = 0;
    bool sawExtra = false;
};

EchoStats g_echo;
bool      g_echoRecording = false;

LRESULT CALLBACK EchoWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_INPUT && g_echoRecording) {
        UINT size = 0;
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, nullptr, &size,
                            sizeof(RAWINPUTHEADER)) == 0 && size && size <= 4096) {
            BYTE buf[4096];
            if (GetRawInputData((HRAWINPUT)l, RID_INPUT, buf, &size,
                                sizeof(RAWINPUTHEADER)) == size) {
                auto* ri = reinterpret_cast<RAWINPUT*>(buf);
                if (ri->header.dwType == RIM_TYPEMOUSE) {
                    const RAWMOUSE& rm = ri->data.mouse;
                    ++g_echo.total;
                    g_echo.byDevice[ri->header.hDevice]++;
                    if (ri->header.hDevice == nullptr)          ++g_echo.nullDev;
                    if (rm.ulExtraInformation == kInjectTag)    ++g_echo.tagged;
                    if (rm.usButtonFlags)                       ++g_echo.buttons;
                    else                                        ++g_echo.moves;
                    if (!g_echo.sawExtra) { g_echo.sampleExtra = rm.ulExtraInformation; g_echo.sawExtra = true; }
                }
            }
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void PumpFor(DWORD ms) {
    const DWORD end = GetTickCount() + ms;
    MSG msg;
    while ((int)(GetTickCount() - end) < 0) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(2);
    }
}

int RunProbeEcho() {
    LogLine(L"openMouse — injection feedback probe");
    LogLine(L"");
    LogLine(L"  Question: does input this process injects come back to it as WM_INPUT?");
    LogLine(L"  If yes, the engine must filter its own injections or it will loop.");
    LogLine(L"");

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = EchoWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"openMouse.EchoProbe";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { LogLine(L"FAIL: could not create message window"); return 1; }

    POINT before{};
    GetCursorPos(&before);

    if (!RegisterCapture(hwnd, false)) {
        LogLine(L"FAIL: RIDEV_NOLEGACY registration rejected.");
        return 1;
    }

    // Step 1 — the decisive test. Inject one move and see whether it returns.
    // Deliberately a pure MOUSEEVENTF_MOVE and no buttons: a synthetic click
    // would land on whatever window happens to be under the cursor.
    LogLine(L"  [1/3] Injecting ONE SendInput mouse move (tagged %08llX)...",
            (unsigned long long)kInjectTag);
    g_echo = EchoStats{};
    g_echoRecording = true;
    POINT target{ before.x + 40, before.y + 20 };
    ClampToVirtualScreen(target);
    SendMouseAt(target, MOUSEEVENTF_MOVE);
    PumpFor(700);
    g_echoRecording = false;
    const int  afterMove   = g_echo.total;
    const int  moveTagged  = g_echo.tagged;
    const int  moveNullDev = g_echo.nullDev;
    LogLine(L"        echoed events: %d   hDevice==NULL: %d   tag matched: %d",
            afterMove, moveNullDev, moveTagged);
    if (g_echo.sawExtra)
        LogLine(L"        first ulExtraInformation seen: %08llX",
                (unsigned long long)g_echo.sampleExtra);
    for (const auto& kv : g_echo.byDevice)
        LogLine(L"          hDevice=%p  events=%d", kv.first, kv.second);

    // Step 2 — SetCursorPos runs on every move of the active seat, so whether
    // it echoes matters as much as SendInput.
    LogLine(L"  [2/3] SetCursorPos (runs on every active-seat move)...");
    g_echo = EchoStats{};
    g_echoRecording = true;
    SetCursorPos(before.x, before.y);
    PumpFor(700);
    g_echoRecording = false;
    const int afterSetPos = g_echo.total;
    LogLine(L"        echoed events: %d   tag matched: %d", afterSetPos, g_echo.tagged);

    // Step 3 — only needed to disambiguate a negative result. If nothing
    // echoed, we must rule out "this window receives no raw input at all"
    // before concluding "injection does not feed back".
    int baseline = -1;
    if (afterMove == 0 && afterSetPos == 0) {
        LogLine(L"  [3/3] Nothing echoed. Confirming the window is not simply deaf —");
        LogLine(L"        MOVE A MOUSE NOW, 4 seconds...");
        g_echo = EchoStats{};
        g_echoRecording = true;
        PumpFor(4000);
        g_echoRecording = false;
        baseline = g_echo.total;
        LogLine(L"        real hardware events: %d   distinct devices: %llu",
                baseline, (unsigned long long)g_echo.byDevice.size());
        for (const auto& kv : g_echo.byDevice)
            LogLine(L"          hDevice=%p  events=%d", kv.first, kv.second);
    } else {
        LogLine(L"  [3/3] Skipped — injection already echoed, window is clearly live.");
    }

    UnregisterCapture();
    SetCursorPos(before.x, before.y);

    LogLine(L"");
    LogLine(L"  ---------------- verdict ----------------");

    if (afterMove > 0 || afterSetPos > 0) {
        LogLine(L"  FEEDBACK CONFIRMED — this process sees its own injected input.");
        LogLine(L"    SendInput echo=%d   SetCursorPos echo=%d", afterMove, afterSetPos);
        LogLine(L"    Unfiltered, the engine loops on the first click: inject ->");
        LogLine(L"    WM_INPUT -> route to seat -> inject -> ... The watchdog cannot");
        LogLine(L"    break it, because the storm keeps the heartbeat climbing.");
        LogLine(L"");
        if (afterMove > 0 && moveTagged == afterMove) {
            LogLine(L"    dwExtraInfo tagging is RELIABLE (%d/%d) — filter on",
                    moveTagged, afterMove);
            LogLine(L"    RAWMOUSE.ulExtraInformation == kInjectTag.");
            return 10;
        }
        LogLine(L"    dwExtraInfo tagging covered only %d of %d echoed events.",
                moveTagged, afterMove);
        LogLine(L"    Filter on hDevice==NULL as well (%d/%d were NULL).",
                moveNullDev, afterMove);
        return 11;
    }

    if (baseline == 0) {
        LogLine(L"  INCONCLUSIVE — no injected echo AND no hardware events.");
        LogLine(L"    Cannot tell 'injection does not feed back' apart from");
        LogLine(L"    'this message-only window receives no raw input'. Re-run");
        LogLine(L"    and move a mouse when prompted.");
        return 3;
    }

    LogLine(L"  NO FEEDBACK — the window receives real hardware input (%d events)", baseline);
    LogLine(L"    but not its own injections. No injection filter is required.");
    return 0;
}

// ---------------------------------------------------------------- identify
//
// This machine enumerates four HID mouse interfaces and five keyboard
// interfaces for two physical mice and two physical keyboards — Logitech
// receivers expose several collections each. ApplyBindings assigns seats in
// raw-enumeration order, so it binds seat 0 and seat 1 to whichever interfaces
// happen to come first, and silently drops the rest onto seat 0. Two physical
// mice can therefore end up driving the same pointer.
//
// The only reliable way to map a physical device to its interface path is to
// ask which handle produces events when that device alone is moved. Runs
// WITHOUT RIDEV_NOLEGACY, so the machine stays completely usable throughout.

std::map<HANDLE, int> g_idMouse, g_idKeyboard;
bool                  g_idRecording = false;

LRESULT CALLBACK IdentifyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_INPUT && g_idRecording) {
        UINT size = 0;
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, nullptr, &size,
                            sizeof(RAWINPUTHEADER)) == 0 && size && size <= 4096) {
            BYTE buf[4096];
            if (GetRawInputData((HRAWINPUT)l, RID_INPUT, buf, &size,
                                sizeof(RAWINPUTHEADER)) == size) {
                auto* ri = reinterpret_cast<RAWINPUT*>(buf);
                if (ri->header.hDevice) {
                    if (ri->header.dwType == RIM_TYPEMOUSE) {
                        const RAWMOUSE& rm = ri->data.mouse;
                        if (rm.lLastX || rm.lLastY || rm.usButtonFlags)
                            g_idMouse[ri->header.hDevice]++;
                    } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
                        if (!(ri->data.keyboard.Flags & RI_KEY_BREAK))
                            g_idKeyboard[ri->header.hDevice]++;
                    }
                }
            }
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

HANDLE g_conIn = INVALID_HANDLE_VALUE;

// Pump the message loop while waiting for Enter, so raw input keeps arriving.
// A blocking console read would starve WM_INPUT and record nothing.
bool PumpUntilEnter(DWORD timeoutMs) {
    const DWORD end = GetTickCount() + timeoutMs;
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_conIn != INVALID_HANDLE_VALUE) {
            DWORD pending = 0;
            if (GetNumberOfConsoleInputEvents(g_conIn, &pending) && pending) {
                INPUT_RECORD rec[64];
                DWORD got = 0;
                if (ReadConsoleInputW(g_conIn, rec, pending < 64 ? pending : 64, &got)) {
                    for (DWORD i = 0; i < got; ++i)
                        if (rec[i].EventType == KEY_EVENT &&
                            rec[i].Event.KeyEvent.bKeyDown &&
                            rec[i].Event.KeyEvent.wVirtualKeyCode == VK_RETURN)
                            return true;
                }
            }
        }
        if ((int)(GetTickCount() - end) >= 0) return false;
        Sleep(5);
    }
}

// Returns the handle with the most events, provided it is a clear winner:
// at least kMinEvents total and at least twice the runner-up. Anything less
// means the user moved more than one device and we must not guess.
HANDLE DominantDevice(const std::map<HANDLE, int>& tally, int& outCount, int& outRunnerUp) {
    HANDLE best = nullptr;
    int bestN = 0, secondN = 0;
    for (const auto& kv : tally) {
        if (kv.second > bestN)      { secondN = bestN; bestN = kv.second; best = kv.first; }
        else if (kv.second > secondN) secondN = kv.second;
    }
    outCount = bestN;
    outRunnerUp = secondN;
    return best;
}

// Reports every interface that produces events, not just the dominant one.
// --identify only names its winner, which hides the case that matters here: a
// single physical mouse emitting on two HID collections at once. The second
// collection is not bound to any seat, so it falls through to seat 0 and drags
// the system cursor along with the seat's own pointer.
int RunWatch() {
    LogLine(L"openMouse — raw device watch");
    LogLine(L"");
    LogLine(L"  Observe-only: nothing is suppressed, input works normally.");
    LogLine(L"");

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = IdentifyWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"openMouse.Watch";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { LogLine(L"FAIL: could not create message window"); return 1; }

    RAWINPUTDEVICE rid[2];
    rid[0].usUsagePage = 0x01; rid[0].usUsage = 0x02;
    rid[0].dwFlags = RIDEV_INPUTSINK; rid[0].hwndTarget = hwnd;
    rid[1].usUsagePage = 0x01; rid[1].usUsage = 0x06;
    rid[1].dwFlags = RIDEV_INPUTSINK; rid[1].hwndTarget = hwnd;
    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
        LogLine(L"FAIL: RegisterRawInputDevices failed: %lu", GetLastError());
        return 1;
    }

    g_cfg.Load(Config::DefaultPath());

    LogLine(L"  Move ONLY mouse 2 — the one whose pointer drags the other —");
    LogLine(L"  in wide circles for the next 12 seconds. Starting now.");
    g_idMouse.clear();
    g_idKeyboard.clear();
    g_idRecording = true;
    PumpFor(12000);
    g_idRecording = false;

    rid[0].dwFlags = RIDEV_REMOVE; rid[0].hwndTarget = nullptr;
    rid[1].dwFlags = RIDEV_REMOVE; rid[1].hwndTarget = nullptr;
    RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));

    LogLine(L"");
    LogLine(L"  ---- mouse interfaces that produced events ----");
    if (g_idMouse.empty()) LogLine(L"  (none — was the mouse moved?)");
    for (const auto& kv : g_idMouse) {
        const std::wstring path = DevicePathFromHandle(kv.first);
        auto bound = g_cfg.mouseBinding.find(path);
        LogLine(L"  %6d events   seat %s", kv.second,
                bound != g_cfg.mouseBinding.end()
                    ? std::to_wstring(bound->second).c_str()
                    : L"UNBOUND -> falls through to seat 0");
        LogLine(L"                 %s", path.c_str());
    }
    LogLine(L"");
    if (g_idMouse.size() > 1) {
        LogLine(L"  DIAGNOSIS: one physical mouse is emitting on %llu interfaces.",
                (unsigned long long)g_idMouse.size());
        LogLine(L"  Any of them not bound to this seat drives seat 0 as well,");
        LogLine(L"  which is why moving it drags the other pointer along.");
    } else {
        LogLine(L"  Only one interface emitted — the drag has another cause.");
    }
    LogLine(L"");
    LogLine(L"  This window closes in 20 seconds.");
    Sleep(20000);
    return 0;
}

int RunIdentify() {
    constexpr int kMinEvents = 15;

    g_cfg.Load(Config::DefaultPath());

    LogLine(L"openMouse — device identification");
    LogLine(L"");
    LogLine(L"  This machine reports more HID interfaces than it has physical");
    LogLine(L"  devices, so seats must be bound by observation rather than by");
    LogLine(L"  enumeration order.");
    LogLine(L"");
    LogLine(L"  Normal input is NOT suppressed during this step — the mouse and");
    LogLine(L"  keyboard keep working as usual.");
    LogLine(L"");

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = IdentifyWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"openMouse.Identify";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { LogLine(L"FAIL: could not create message window"); return 1; }

    // Observe only: INPUTSINK without NOLEGACY. Nothing is suppressed.
    RAWINPUTDEVICE rid[2];
    rid[0].usUsagePage = 0x01; rid[0].usUsage = 0x02;
    rid[0].dwFlags = RIDEV_INPUTSINK; rid[0].hwndTarget = hwnd;
    rid[1].usUsagePage = 0x01; rid[1].usUsage = 0x06;
    rid[1].dwFlags = RIDEV_INPUTSINK; rid[1].hwndTarget = hwnd;
    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
        LogLine(L"FAIL: RegisterRawInputDevices failed: %lu", GetLastError());
        return 1;
    }

    g_conIn = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_EXISTING, 0, nullptr);
    if (g_conIn != INVALID_HANDLE_VALUE) {
        SetConsoleMode(g_conIn, ENABLE_PROCESSED_INPUT);
        FlushConsoleInputBuffer(g_conIn);
    }

    std::map<std::wstring, int> newMouse, newKeyboard;
    bool ok = true;

    for (int seat = 0; seat < g_cfg.seatCount && ok; ++seat) {
        // ---- mouse
        LogLine(L"  ---- SEAT %d ----", seat);
        LogLine(L"  Move ONLY the mouse for seat %d in circles, then press Enter.", seat);
        g_idMouse.clear();
        g_idRecording = true;
        const bool gotEnterM = PumpUntilEnter(120000);
        g_idRecording = false;
        if (!gotEnterM) { LogLine(L"  timed out waiting for Enter"); ok = false; break; }

        int n = 0, runner = 0;
        HANDLE dev = DominantDevice(g_idMouse, n, runner);
        if (!dev || n < kMinEvents) {
            LogLine(L"  Not enough movement recorded (%d events). Move the mouse more.", n);
            ok = false; break;
        }
        if (runner * 2 > n) {
            LogLine(L"  Ambiguous: top device %d events, runner-up %d. Move ONE mouse only.",
                    n, runner);
            ok = false; break;
        }
        std::wstring path = DevicePathFromHandle(dev);
        LogLine(L"  -> mouse    %d events  %s", n, FriendlyNameFromPath(path).c_str());
        LogLine(L"     %s", path.c_str());
        if (newMouse.count(path)) {
            LogLine(L"  That is the same mouse already bound to another seat.");
            ok = false; break;
        }
        newMouse[path] = seat;

        // ---- keyboard
        LogLine(L"  Now type on ONLY the keyboard for seat %d, then press Enter.", seat);
        g_idKeyboard.clear();
        g_idRecording = true;
        const bool gotEnterK = PumpUntilEnter(120000);
        g_idRecording = false;
        if (!gotEnterK) { LogLine(L"  timed out waiting for Enter"); ok = false; break; }

        // The Enter keypress itself lands on this keyboard, which is fine —
        // it is one more vote for the right device.
        int kn = 0, krunner = 0;
        HANDLE kdev = DominantDevice(g_idKeyboard, kn, krunner);
        if (!kdev || kn < 3) {
            LogLine(L"  Not enough typing recorded (%d events). Skipping keyboard for this seat.", kn);
        } else if (krunner * 2 > kn) {
            LogLine(L"  Ambiguous keyboard: top %d, runner-up %d. Skipping.", kn, krunner);
        } else {
            std::wstring kpath = DevicePathFromHandle(kdev);
            LogLine(L"  -> keyboard %d events  %s", kn, FriendlyNameFromPath(kpath).c_str());
            LogLine(L"     %s", kpath.c_str());
            if (newKeyboard.count(kpath)) LogLine(L"  Already bound to another seat — skipping.");
            else newKeyboard[kpath] = seat;
        }
        LogLine(L"");
    }

    rid[0].dwFlags = RIDEV_REMOVE; rid[0].hwndTarget = nullptr;
    rid[1].dwFlags = RIDEV_REMOVE; rid[1].hwndTarget = nullptr;
    RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));
    if (g_conIn != INVALID_HANDLE_VALUE) CloseHandle(g_conIn);
    g_conIn = INVALID_HANDLE_VALUE;

    if (!ok) {
        LogLine(L"  Aborted — configuration NOT changed.");
        return 2;
    }

    // Every interface we did not positively identify is left unbound rather
    // than defaulted to seat 0, so a stray collection cannot drive a pointer.
    g_cfg.mouseBinding    = newMouse;
    g_cfg.keyboardBinding = newKeyboard;
    if (!g_cfg.Save(Config::DefaultPath())) {
        LogLine(L"  FAIL: could not write %s", Config::DefaultPath().c_str());
        return 1;
    }
    LogLine(L"  Saved %llu mouse and %llu keyboard bindings to",
            (unsigned long long)newMouse.size(), (unsigned long long)newKeyboard.size());
    LogLine(L"  %s", Config::DefaultPath().c_str());
    LogLine(L"");
    LogLine(L"  Done. This window closes in 15 seconds.");
    Sleep(15000);
    return 0;
}

// Seconds after which the engine shuts itself down, or 0 for no limit. A
// bounded run is the difference between "try it and see" and "try it and hope"
// when the thing under test owns every mouse on the machine.
int g_runSeconds = 0;

// If the process dies with an unhandled exception it takes a system-wide
// input capture, a hidden cursor and a 1px cursor cage down with it. Undo
// all three before Windows Error Reporting gets the corpse.
LONG WINAPI EmergencyRestore(EXCEPTION_POINTERS*) {
    StopGateThread();          // physical clicks must flow again
    UnregisterCapture();       // also releases the cage
    RestoreSystemCursor();
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------- receiver
//
// A visible topmost window that logs every mouse message it receives — the
// app's-eye view of what the engine delivers. Run alongside the engine and
// click into it; the diff between the engine's [inj] lines and this log
// localises any fault to a layer.

HANDLE g_recvLog = INVALID_HANDLE_VALUE;

void RecvLine(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 511, fmt, ap);
    va_end(ap);
    buf[511] = 0;
    if (g_recvLog != INVALID_HANDLE_VALUE) {
        char utf8[1024];
        const int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, 1020, nullptr, nullptr);
        if (n > 1) {
            DWORD w = 0;
            WriteFile(g_recvLog, utf8, n - 1, &w, nullptr);
            WriteFile(g_recvLog, "\r\n", 2, &w, nullptr);
        }
    }
}

LRESULT CALLBACK ReceiverWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static POINT lastMove{ -32768, -32768 };
    const int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
    switch (msg) {
    case WM_LBUTTONDOWN:   RecvLine(L"LDOWN    t=%lu  %d,%d", GetTickCount(), x, y); return 0;
    case WM_LBUTTONUP:     RecvLine(L"LUP      t=%lu  %d,%d", GetTickCount(), x, y); return 0;
    case WM_LBUTTONDBLCLK: RecvLine(L"LDBLCLK  t=%lu  %d,%d", GetTickCount(), x, y); return 0;
    case WM_RBUTTONDOWN:   RecvLine(L"RDOWN    t=%lu  %d,%d", GetTickCount(), x, y); return 0;
    case WM_RBUTTONUP:     RecvLine(L"RUP      t=%lu  %d,%d", GetTickCount(), x, y); return 0;
    case WM_MOUSEWHEEL:    RecvLine(L"WHEEL    t=%lu  delta=%d", GetTickCount(), GET_WHEEL_DELTA_WPARAM(wp)); return 0;
    case WM_MOUSEACTIVATE: RecvLine(L"MOUSEACTIVATE t=%lu", GetTickCount()); break;
    case WM_SETFOCUS:      RecvLine(L"SETFOCUS t=%lu", GetTickCount()); break;
    case WM_KILLFOCUS:     RecvLine(L"KILLFOCUS t=%lu", GetTickCount()); break;
    case WM_MOUSEMOVE:
        if (x != lastMove.x || y != lastMove.y) {
            RecvLine(L"MOVE     t=%lu  %d,%d", GetTickCount(), x, y);
            lastMove = { x, y };
        }
        return 0;
    case WM_DESTROY:       PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int RunReceiver() {
    std::wstring path = Config::DefaultPath();
    const size_t slash = path.rfind(L'\\');
    path = (slash == std::wstring::npos) ? std::wstring(L"receiver.log")
                                         : path.substr(0, slash + 1) + L"receiver.log";
    g_recvLog = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = ReceiverWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"openMouse.ClickReceiver";     // deliberately no CS_DBLCLKS
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName,
                                L"openMouse click test — click in here",
                                WS_OVERLAPPEDWINDOW, 600, 250, 500, 400,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { RecvLine(L"FAIL CreateWindowEx err=%lu", GetLastError()); return 1; }
    ShowWindow(hwnd, SW_SHOW);
    RecvLine(L"receiver up t=%lu", GetTickCount());

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    RecvLine(L"receiver closed t=%lu", GetTickCount());
    CloseHandle(g_recvLog);
    return 0;
}

// ---------------------------------------------------------------- monitor
//
// WH_MOUSE_LL sees every button event entering the legacy pointer pipeline,
// system-wide, with provenance: LLMHF_INJECTED plus our kInjectTag in
// dwExtraInfo separate hardware events, engine injections, and foreign
// injections. If a physical click ever reaches this hook UN-injected while
// the engine holds NOLEGACY, legacy suppression is leaking and every click
// doubles — the prime suspect for "double click selects a paragraph".

HANDLE g_monLog = INVALID_HANDLE_VALUE;

LRESULT CALLBACK MonitorHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && lp) {
        const auto* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        const wchar_t* name = nullptr;
        switch (wp) {
        case WM_LBUTTONDOWN: name = L"LDOWN"; break;
        case WM_LBUTTONUP:   name = L"LUP  "; break;
        case WM_RBUTTONDOWN: name = L"RDOWN"; break;
        case WM_RBUTTONUP:   name = L"RUP  "; break;
        case WM_MOUSEWHEEL:  name = L"WHEEL"; break;
        }
        if (name && g_monLog != INVALID_HANDLE_VALUE) {
            wchar_t buf[256];
            _snwprintf(buf, 255, L"%s t=%lu pos=%ld,%ld injected=%d lowerIL=%d tag=%s\r\n",
                       name, m->time, m->pt.x, m->pt.y,
                       (m->flags & LLMHF_INJECTED) ? 1 : 0,
                       (m->flags & LLMHF_LOWER_IL_INJECTED) ? 1 : 0,
                       m->dwExtraInfo == kInjectTag ? L"ENGINE" : L"none");
            char utf8[512];
            const int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, 508, nullptr, nullptr);
            if (n > 1) { DWORD w = 0; WriteFile(g_monLog, utf8, n - 1, &w, nullptr); }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

int RunMonitor() {
    std::wstring path = Config::DefaultPath();
    const size_t slash = path.rfind(L'\\');
    path = (slash == std::wstring::npos) ? std::wstring(L"monitor.log")
                                         : path.substr(0, slash + 1) + L"monitor.log";
    g_monLog = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, MonitorHook, GetModuleHandleW(nullptr), 0);
    if (!hook) { LogLine(L"[monitor] SetWindowsHookEx failed: %lu", GetLastError()); return 1; }
    LogLine(L"[monitor] running — logging all system button events");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnhookWindowsHookEx(hook);
    CloseHandle(g_monLog);
    return 0;
}

// ---------------------------------------------------------------- janitor
//
// Documentation research turned up two undoing-gaps that in-process cleanup
// cannot close: a ClipCursor rect SURVIVES the death of the process that set
// it (measured: pointer stays welded to the 1px cage after a hard kill until
// some focus change clears it — which the user cannot click to cause), and
// MagShowSystemCursor has no documented crash restoration either. Exit paths
// and exception filters never run under TerminateProcess / Task Manager.
//
// So the engine spawns a janitor of itself: a tiny process that just waits on
// the engine's handle and, when the engine dies FOR ANY REASON, releases the
// cage and re-shows the cursor. Belt for every suspender.

int RunJanitor(DWORD parentPid) {
    OpenLogFile();
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    if (!h) return 1;
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);

    ClipCursor(nullptr);
    if (g_mag.Load() && g_mag.Initialize && g_mag.Initialize()) {
        if (g_mag.ShowSystemCursor) g_mag.ShowSystemCursor(TRUE);
        if (g_mag.Uninitialize)     g_mag.Uninitialize();
    }
    LogLine(L"[janitor] engine pid %lu died — cage released, cursor restored", parentPid);
    return 0;
}

void SpawnJanitor() {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
    wchar_t cmd[MAX_PATH + 40];
    swprintf(cmd, MAX_PATH + 40, L"\"%s\" --janitor=%lu", exe, GetCurrentProcessId());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        LogLine(L"[janitor] spawned (pid %lu watches this engine)", pi.dwProcessId);
    } else {
        LogLine(L"[janitor] spawn failed: %lu — hard kills will leave the cage behind", GetLastError());
    }
}

// ---------------------------------------------------------------- tray

constexpr UINT kTrayCmdStop     = 100;
constexpr UINT kTrayCmdIdentify = 101;
constexpr UINT kTrayCmdLog      = 102;
constexpr UINT kTrayCmdAbout    = 103;

// Relaunch ourselves in another mode. Used by the tray menu.
void RelaunchSelf(const wchar_t* args) {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
    wchar_t cmd[MAX_PATH + 64];
    swprintf(cmd, MAX_PATH + 64, L"\"%s\" %s", exe, args);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

void TrayAdd(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = hwnd;
    nid.uID              = kTrayIconId;
    nid.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayCallbackMsg;
    nid.hIcon            = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"openMouse — right-click to stop (panic: Ctrl+Alt+Shift+Q)");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void TrayRemove(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd;
    nid.uID    = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void TrayMenu(HWND hwnd) {
    wchar_t header[64];
    swprintf(header, 64, L"openMouse — %d seats%s", g_cfg.seatCount,
             g_cfg.captureKeyboards ? L", keyboards steered" : L"");

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_DISABLED, kTrayCmdAbout, header);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayCmdIdentify, L"Re-identify devices…");
    AppendMenuW(menu, MF_STRING, kTrayCmdLog,      L"Open log folder");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayCmdStop,     L"Stop openMouse\tCtrl+Alt+Shift+Q");

    POINT pt{};
    GetCursorPos(&pt);
    // TrackPopupMenu on a tray icon needs the window foreground, or the menu
    // refuses to dismiss when the user clicks away. Documented quirk.
    SetForegroundWindow(hwnd);
    const UINT cmd = (UINT)TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);

    switch (cmd) {
    case kTrayCmdStop:
        LogLine(L"[tray] stop requested");
        RequestExit();
        break;
    case kTrayCmdIdentify:
        // Identification needs unsuppressed input and an interactive console,
        // so hand over rather than run alongside: spawn it, then stand down.
        LogLine(L"[tray] re-identify requested — handing over");
        RelaunchSelf(L"--identify");
        RequestExit();
        break;
    case kTrayCmdLog: {
        std::wstring dir = Config::DefaultPath();
        const size_t slash = dir.rfind(L'\\');
        if (slash != std::wstring::npos) dir = dir.substr(0, slash);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    }
}

int RunMain(HINSTANCE inst) {
    SetUnhandledExceptionFilter(EmergencyRestore);

    // A clip rect survives the death of the process that set it, so a
    // previous instance killed hard enough to outrun its janitor can leave
    // the pointer fenced into one pixel. Clear anything stale before doing
    // anything else — this build normally holds no clip at all.
    ClipCursor(nullptr);

    g_cfg.Load(Config::DefaultPath());

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.lpszClassName = L"openMouse.Engine";
    RegisterClassExW(&wc);

    // A hidden NORMAL window, not a message-only one: the tray icon needs a
    // window that can receive shell callbacks, and message-only windows
    // cannot. Never shown; WS_EX_TOOLWINDOW keeps it out of Alt-Tab.
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"openMouse",
                             WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) { LogLine(L"[fatal] could not create engine window"); return 1; }

    ApplyBindings();

    // Seed every seat at the current cursor, then fan them out so they are not
    // stacked on top of each other at startup.
    POINT start{};
    GetCursorPos(&start);
    for (int i = 0; i < g_cfg.seatCount; ++i) {
        g_seats[i].index = i;
        g_seats[i].pos   = { start.x + i * 40, start.y + i * 24 };
        ClampToVirtualScreen(g_seats[i].pos);
        const COLORREF c = (i < (int)g_cfg.seatColors.size())
                         ? g_cfg.seatColors[i] : RGB(200, 200, 200);
        // Every seat gets an overlay, including seat 0. Seat 0 starts out
        // owning the hardware cursor so its overlay starts hidden, but it
        // needs to exist: the moment another seat clicks, seat 0 loses the
        // cursor and SetActiveSeat asks for that overlay. Creating it only for
        // i != 0 made seat 0's pointer vanish permanently on the first
        // handover.
        g_overlayOk[i] = g_overlay[i].Create(inst, i, c);
        g_overlay[i].MoveTo(g_seats[i].pos);
        LogLine(L"[seat %d] colour %02X%02X%02X  overlay %s", i,
                GetRValue(c), GetGValue(c), GetBValue(c),
                g_overlayOk[i] ? L"created" : L"FAILED TO CREATE");
    }
    g_activeSeat = 0;

    // Decide this before showing the overlays: if the hardware cursor can be
    // hidden, every seat draws itself; if not, seat 0 keeps the system cursor
    // and we fall back to snapping it back after stray movement.
    const bool hidden = HideSystemCursor();
    if (!hidden)
        LogLine(L"[cursor] could not hide — falling back to snap-back correction (expect jitter)");
    for (int i = 0; i < g_cfg.seatCount; ++i)
        g_overlay[i].Show(hidden || i != 0);

    if (!RegisterCapture(g_hwnd, g_cfg.captureKeyboards)) return 1;

    // The gate is what stops physical clicks reaching apps alongside our
    // injections. Without it every click is delivered twice, so its failure
    // is fatal, not degraded.
    std::thread gate(GateThreadProc);
    Sleep(100);
    if (!g_mouseGate) {
        LogLine(L"[fatal] mouse gate failed — refusing to run with doubled clicks");
        UnregisterCapture();
        if (g_gateThreadId) PostThreadMessageW(g_gateThreadId, WM_QUIT, 0, 0);
        gate.join();
        return 1;
    }

    TrayAdd(g_hwnd);
    SpawnJanitor();

    LogLine(L"openMouse running — %d seats, keyboard capture %s",
            g_cfg.seatCount, g_cfg.captureKeyboards ? L"ON" : L"off");
    LogLine(L"Panic key: Ctrl+Alt+Shift+Q");
    for (int i = 0; i < g_cfg.seatCount; ++i)
        LogLine(L"  seat %d start pos %ld,%ld", i, g_seats[i].pos.x, g_seats[i].pos.y);

    SetTimer(g_hwnd, 1, 250, nullptr);

    if (g_runSeconds > 0) {
        LogLine(L"Auto-exit in %d seconds.", g_runSeconds);
        SetTimer(g_hwnd, 2, (UINT)g_runSeconds * 1000, nullptr);
    }

    std::thread watchdog(WatchdogThread);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_running = false;
    watchdog.join();
    StopGateThread();
    gate.join();
    TrayRemove(g_hwnd);
    UnregisterCapture();
    RestoreSystemCursor();
    for (int i = 0; i < kMaxSeats; ++i) g_overlay[i].Destroy();
    return 0;
}

// Recovery hatch. If the engine is ever killed hard enough to skip its exit
// path, the desktop is left with an invisible pointer and this is the way back.
int RunRestoreCursor() {
    LogLine(L"openMouse — restoring the system cursor");
    if (!g_mag.Load())       { LogLine(L"  magnification.dll unavailable"); return 1; }
    if (!g_mag.Initialize()) { LogLine(L"  MagInitialize failed: %lu", GetLastError()); return 1; }
    g_magReady = true;
    const BOOL ok = g_mag.ShowSystemCursor(TRUE);
    if (g_mag.Uninitialize) g_mag.Uninitialize();
    LogLine(ok ? L"  cursor restored." : L"  MagShowSystemCursor(TRUE) failed.");
    return ok ? 0 : 1;
}

} // namespace
} // namespace om

// ---------------------------------------------------------------- entry

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    // Per-monitor DPI awareness keeps pointer coordinates honest on mixed-DPI
    // setups. Resolved dynamically so the binary still runs on older builds.
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using SetCtxFn = BOOL (WINAPI*)(HANDLE);
        if (auto fn = (SetCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext"))
            fn((HANDLE)-4);  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    }

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring mode;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--list" || a == L"--probe" || a == L"--probe-echo" ||
            a == L"--identify" || a == L"--watch" || a == L"--restore-cursor" ||
            a == L"--receiver" || a == L"--monitor" || a == L"--help") mode = a;
        else if (a == L"--verbose")
            om::g_verbose = true;
        else if (a == L"--cage")
            om::g_eatMoves = false;   // force the old ClipCursor behaviour
        else if (a.rfind(L"--seconds=", 0) == 0)
            om::g_runSeconds = _wtoi(a.c_str() + 10);
        else if (a.rfind(L"--janitor=", 0) == 0)
            return om::RunJanitor((DWORD)_wtoi(a.c_str() + 10));
    }
    LocalFree(argv);

    om::OpenLogFile();

    if (mode == L"--identify" || mode == L"--watch") {
        // Needs its own visible window: it is a dialogue with the person at
        // the keyboard, not something to be captured through a pipe.
        AllocConsole();
        SetConsoleTitleW(mode == L"--watch" ? L"openMouse — device watch"
                                            : L"openMouse — device identification");
        if (HWND c = GetConsoleWindow()) { ShowWindow(c, SW_SHOW); SetForegroundWindow(c); }
    } else if (!mode.empty()) {
        // Only steal a console when stdout is not already going somewhere. If
        // the caller redirected our output to a pipe or file, AttachConsole
        // would replace that handle and the captured output would vanish.
        const HANDLE existing = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!existing || existing == INVALID_HANDLE_VALUE) {
            if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        }
    }

    if (mode == L"--list")       return om::RunList();
    if (mode == L"--probe")      return om::RunProbe();
    if (mode == L"--probe-echo") return om::RunProbeEcho();
    if (mode == L"--identify")   return om::RunIdentify();
    if (mode == L"--watch")      return om::RunWatch();
    if (mode == L"--restore-cursor") return om::RunRestoreCursor();
    if (mode == L"--receiver")   return om::RunReceiver();
    if (mode == L"--monitor")    return om::RunMonitor();
    if (mode == L"--help") {
        om::LogLine(L"openMouse — multi-pointer input for Windows");
        om::LogLine(L"");
        om::LogLine(L"  openmouse.exe               run the engine");
        om::LogLine(L"  openmouse.exe --list        list input devices and their paths");
        om::LogLine(L"  openmouse.exe --identify    bind each seat by moving its mouse");
        om::LogLine(L"  openmouse.exe --probe       check SendInput works under NOLEGACY");
        om::LogLine(L"  openmouse.exe --probe-echo  check whether injection feeds back");
        om::LogLine(L"  openmouse.exe --watch       show which interface a device emits on");
        om::LogLine(L"  openmouse.exe --receiver    window that logs the mouse messages it gets");
        om::LogLine(L"  openmouse.exe --monitor     system-wide button log with provenance");
        om::LogLine(L"  openmouse.exe --restore-cursor  bring back a cursor left hidden");
        om::LogLine(L"  openmouse.exe --help        this text");
        om::LogLine(L"");
        om::LogLine(L"  Run flags:  --verbose        per-event tracing to the log");
        om::LogLine(L"              --seconds=N     auto-exit after N seconds");
        om::LogLine(L"");
        om::LogLine(L"  Panic key while running: Ctrl+Alt+Shift+Q");
        om::LogLine(L"  Tray icon: right-click to stop, re-identify, or open the log folder");
        // %% — this string goes through a printf-style formatter, where a bare
        // %A is a hex-float specifier and eats the path.
        om::LogLine(L"  Log file: %%APPDATA%%\\openMouse\\openmouse.log");
        return 0;
    }
    return om::RunMain(inst);
}
