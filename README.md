# openMouse

Multi-pointer input for Windows. Every connected mouse gets its own on-screen pointer on
one shared desktop, so two people can work at one PC at the same time — one per monitor,
each with their own mouse.

MIT licensed. No kernel driver, no second Windows session, no paid tier.

Named after Linux's Multi-Pointer X, which has offered this natively since 2008. Windows
never got an equivalent.

## Contents

- [Status](#status)
- [What it does](#what-it-does)
- [What it cannot do](#what-it-cannot-do)
- [Requirements](#requirements)
- [Installation](#installation)
- [Setup](#setup)
- [Configuration](#configuration)
- [Command reference](#command-reference)
- [Safety](#safety)
- [How it works](#how-it-works)
- [Findings](#findings)
- [Known limitations](#known-limitations)
- [Building](#building)
- [Licence](#licence)

## Status

Working alpha, in daily use on the machine it was developed on (Windows 11 Pro 25H2,
build 26200; two Logitech mice and keyboards; two monitors).

The mouse half is complete and confirmed in real two-person use: independent pointers,
click-to-own, cross-window focus, per-seat scrolling and dragging. Keyboard support is
limited by an operating-system constraint described below, and is off by default.

It has been tested on exactly one machine. Several behaviours it depends on are
undocumented (see [Findings](#findings)), so treat other Windows versions as unverified
until you run `--probe` and `--probe-echo` there.

## What it does

- Each connected mouse drives its own pointer, drawn with the machine's real cursor
  artwork. Seat 0 gets the standard white arrow, seat 1 the black scheme; further seats
  are tinted to their configured colour.
- Exactly one seat owns the click stream at a time. Clicking transfers ownership, so
  clicks land where the clicker is pointing and hover states follow them. Clicking into
  a window that is not focused activates it and delivers the click in one press.
- Scrolling is per seat and requires no click: each wheel scrolls the window under its
  own pointer, and does not steal ownership.
- Dragging is protected. While one seat holds a button, the other seats' buttons are
  held off so the drag cannot be broken or hijacked; their pointers keep moving freely.
- Mouse back/forward side buttons work per seat.
- Runs from the system tray. Right-click to stop, re-identify devices, or open the log
  folder.
- Device bindings are stored by device interface path, so they survive reboots and
  replugging.

## What it cannot do

This section is the honest limit of a user-mode approach. Read it before relying on the
tool.

**One foreground window per desktop.** Windows delivers keystrokes only to the focused
window. There is no supported way to type into a control that does not have focus.
`PostMessage(WM_CHAR)` works for classic Win32 controls, where each control is a real
window, but Chromium (Edge, Chrome, Electron), WPF and UWP render every control inside a
single window handle. There is no handle to address.

**Per-seat keyboards cannot be built the way per-seat mice can.** The mouse works by
suppressing physical input in a low-level hook and re-injecting it per seat. That is
impossible for keyboards: measured on Windows 11 26200, blocking a key in a
`WH_KEYBOARD_LL` hook also suppresses that key's raw input delivery — and raw input is
the only place device identity exists, since `KBDLLHOOKSTRUCT` carries none. Suppress
the key and you go blind; stay sighted and you cannot suppress. The mouse is not subject
to this: blocking a button in `WH_MOUSE_LL` leaves its `WM_INPUT` intact.

Keyboard support is therefore **steering only**. Keystrokes are never suppressed and
never re-injected; the engine simply brings the typing seat's last-clicked window to the
front first. Consequences: no per-seat keyboard layouts, one shared modifier state, and
the first keystroke after a seat loses focus may still land in the previous window. Two
people typing simultaneously interleave into one window.

**One dragger at a time.** There is a single click stream, so while one seat drags, the
other seats' clicks are held until the drag ends.

**Shell surfaces.** The taskbar, Start menu and tray flyouts draw above every normal
window, including the pointer overlays. When the owning seat's pointer moves over shell
UI, the real cursor is shown there instead so those surfaces stay usable; non-owning
pointers are not visible over them.

Out of scope entirely: per-seat audio, per-seat clipboard, per-seat window z-order, and
anything needing real isolation. Isolation requires separate Windows sessions, which is
a different and much larger project.

## Requirements

| | |
|---|---|
| Operating system | Windows 10 or 11, x64. Windows 8 is the theoretical minimum (Magnification API) but is untested. |
| Architecture | x64 only. The Magnification API is not supported under WOW64, so a 32-bit build will not work correctly. |
| Privileges | Runs as a normal user. No administrator rights, no driver, no signing requirements. |
| Hardware | Two or more mice. Any HID mouse should work; wireless receivers that expose several HID collections are handled by `--identify`. |
| Displays | Multi-monitor supported, including negative coordinates. Mixed-DPI setups are handled but lightly tested. |

Not portable to macOS or Linux, and not intended to be: the implementation is built on
Win32 raw input, low-level hooks and the Magnification API. Linux already has this
natively through X11's Multi-Pointer X.

## Installation

Download or build `openmouse.exe` (see [Building](#building)) and run it. There is no
installer and nothing is written outside `%APPDATA%\openMouse`.

## Setup

Run these in order. Do not skip step 1.

**1. Check the machine is viable**

```
openmouse.exe --probe
openmouse.exe --probe-echo
```

`--probe` verifies that injected input still reaches the system while a raw-input
registration is active. `--probe-echo` reports whether injected input feeds back into the
raw input stream, which the engine must filter. If `--probe` fails, no user-mode
multi-pointer implementation is possible on that machine.

**2. Identify the devices**

```
openmouse.exe --identify
```

Wireless receivers routinely expose more HID interfaces than there are physical devices,
and enumeration order does not match physical order. On the development machine, two
mice and two keyboards enumerate as four and five interfaces respectively. `--identify`
binds each seat by observation: move only that seat's mouse, then type only on its
keyboard, when prompted. Interfaces it cannot attribute are left unbound rather than
guessed.

Nothing is suppressed during this step; the machine stays fully usable.

**3. Run**

```
openmouse.exe
```

A tray icon appears. You should see two pointers — a white arrow for seat 0 and a black
one for seat 1. Move both mice, click with each, scroll with each, drag with each.

Add `--seconds=60` for a first run that stops by itself.

**4. Optional: keyboard steering**

Set `capture_keyboards=1` in the configuration file and restart. This never suppresses or
injects keystrokes, so it cannot break typing; see
[What it cannot do](#what-it-cannot-do) for what it does and does not achieve.

## Configuration

`%APPDATA%\openMouse\openmouse.ini`

```ini
[general]
seats=2
capture_keyboards=0
mark_active_seat=1

[seat0]
color=FFFFFF          ; FFFFFF renders the stock white arrow, untinted
sensitivity=1.00

[seat1]
color=000000          ; 000000 renders the black pointer scheme
sensitivity=1.00

[mouse]
\\?\HID#VID_046D&PID_C534&MI_01&Col01#7&393c14a9&0&0000#{...}=0
\\?\HID#VID_046D&PID_C534&MI_01&Col01#7&a9440b5&0&0000#{...}=1

[keyboard]
\\?\HID#VID_046D&PID_C534&MI_00#7&2bb71987&0&0000#{...}=0
\\?\HID#VID_046D&PID_C534&MI_00#7&dde68fc&0&0000#{...}=1
```

Any other `color` value tints the arrow body and keeps the dark outline. `sensitivity`
is a per-seat pointer speed multiplier. Up to eight seats are supported by the code;
only two have been tested.

Once measured bindings exist, unmatched interfaces are dropped as ghost collections of
already-bound hardware. On a first run with no bindings at all, devices are auto-assigned
round-robin so nothing is dead out of the box, but run `--identify` before trusting the
assignment.

## Command reference

| Command | Purpose |
|---|---|
| `openmouse.exe` | Run the engine |
| `--identify` | Bind each seat by observing its devices |
| `--list` | List every input device and its interface path |
| `--probe` | Check that injected input reaches the system |
| `--probe-echo` | Check whether injected input feeds back into raw input |
| `--watch` | Show which interface a given physical device emits on |
| `--receiver` | Open a window that logs the mouse messages it receives |
| `--monitor` | Log system-wide button events with provenance |
| `--restore-cursor` | Restore a system cursor left hidden |
| `--help` | Usage summary |

Run flags: `--verbose` enables per-event tracing to the log, `--seconds=N` exits after N
seconds, `--cage` forces the legacy cursor-clipping strategy.

Logs are written to `%APPDATA%\openMouse\openmouse.log` and rotate at 4 MB.

## Safety

While openMouse runs it owns all mouse input on the machine: a low-level mouse hook
swallows every physical button, wheel and move event, and the engine re-injects them per
seat. It also hides the hardware cursor through the Magnification API and positions it
itself.

| Risk | Mitigation |
|---|---|
| Process hangs, leaving no working input | A watchdog thread removes the hook, releases capture and restores the cursor after roughly three seconds of a stalled message loop |
| Need to stop immediately | `Ctrl+Alt+Shift+Q` releases everything and exits. It works from any keyboard, whether or not keyboard steering is on |
| Runaway injection loop | Injections are tagged and self-echoes discarded; a rate limiter of 300 injections per second releases everything and exits |
| Process crashes | An unhandled-exception filter releases everything. The hook and raw-input registration die with the process regardless |
| Process killed outright | A companion janitor process waits on the engine's handle and restores the cursor, and clears any cursor clip, however the engine died |
| Cursor left hidden | `openmouse.exe --restore-cursor` |
| Move suppression stops working after an OS update | Watched at runtime. If physical moves are being swallowed while no raw input arrives, the engine reverts to the cursor-cage strategy by itself within a second |

`Ctrl+Alt+Del` is handled by the kernel and is never captured.

## How it works

Windows has one hardware cursor and one foreground window per desktop. openMouse does
not try to change that. One seat owns the real cursor at any moment; every seat's pointer
is drawn as a layered, click-through, topmost window.

```
 physical mice and keyboards
          |
          v
   WH_MOUSE_LL gate ------> physical buttons, wheel and moves are blocked here
          |                 (injected events pass; this is the real suppression)
          v
   WM_INPUT (INPUTSINK | DEVNOTIFY)   <- used only to tell devices apart
          |   self-injected echoes (hDevice == NULL, or our tag) discarded
          |   ghost HID collections discarded
          v
   device handle ---> seat lookup
          |
          +-- mouse move  --> seat.pos += delta * sensitivity
          |                   owning seat -> SetCursorPos; the real cursor is
          |                   hidden and moves only where the engine puts it
          |                   every seat  -> layered-window overlay arrow
          |
          +-- mouse button -> drag guard; take ownership; if the target window
          |                   is not focused, activate it and let it settle;
          |                   preserve multi-click rhythm; inject the click
          |
          +-- wheel        -> park the cursor at this seat's position, then
          |                   inject (hover-scroll routes it to that window)
          |
          +-- key          -> panic-combo check; if steering is on, bring the
                              seat's last-clicked window to the front
```

| File | Responsibility |
|---|---|
| `src/main.cpp` | Raw input capture, routing, injection, suppression gate, watchdog, tray, janitor, command modes |
| `src/devices.cpp` | Device enumeration and stable identity via the registry Enum key |
| `src/overlay.cpp` | Per-seat pointer rendering derived from the real system cursor |
| `src/config.cpp` | Configuration load and save, logging |

## Findings

These behaviours were measured on Windows 11 Pro build 26200 and are recorded because
they are either undocumented or contradict a reasonable reading of the documentation.
They shaped the design.

**`RIDEV_NOLEGACY` is per-application.** Its suppression applies to the registering
application only, so registering it from a background process does not stop physical
clicks reaching other applications. An early version relied on it, and every click was
consequently delivered twice — once physically and once injected — which appeared as
single clicks selecting text and double clicks selecting paragraphs. Suppression is now
performed by a low-level mouse hook. Raw input is used purely for device attribution.

**Blocking in a low-level mouse hook leaves raw input intact; the keyboard equivalent
does not.** In one measured session the engine still received 242 physical button events
while its hook blocked them, but zero keyboard events while its keyboard hook blocked
those. This asymmetry is the reason per-seat mice are possible and per-seat keyboards are
not.

**Injected input returns through raw input.** `SendInput` events arrive as `WM_INPUT`
with `hDevice == NULL`, carrying the `dwExtraInfo` tag intact. Unfiltered, the first
click becomes an infinite loop, and a watchdog cannot detect it because the storm keeps
the heartbeat healthy. Injections are tagged and discarded on arrival.

**`SetCursorPos` generates no low-level hook event at all.** This is what makes blocking
physical moves safe: the engine cannot block its own cursor positioning. Because it is
undocumented, it is verified at runtime and the engine falls back automatically.

**A `ClipCursor` rectangle survives the death of the process that set it.** A hard kill
can leave the pointer confined to a single pixel, with no way to click out of it. The
janitor process exists for this.

**`MagShowSystemCursor` has no documented restoration on process exit.** Same exposure,
same mitigation.

## Known limitations

- Keyboard support is steering only, and off by default.
- A drag begun as the very first action into an unfocused window is lost to the window
  activation delay. Click to focus, then drag.
- Simultaneous scrolling by two seats can occasionally cross-route a wheel tick, because
  one hardware cursor arbitrates hover-scroll routing.
- Applications that reposition the cursor themselves, such as the "snap to default
  button" accessibility setting, will move the owning seat's pointer.
- Non-owning pointers are not visible over the taskbar, Start menu or tray flyouts.
- No IME support. Non-Latin input under keyboard steering is untested and likely broken.
- Absolute-positioning devices such as tablets and touchscreens are handled in code but
  untested.
- Seat counts above two are supported in code but untested.
- The tray icon uses the generic application icon. There is no settings interface.

## Building

Requires the Visual Studio Build Tools with the C++ workload, or any MSVC-compatible
toolchain, plus the Windows SDK.

```
build.cmd
```

The script locates the toolchain with `vswhere` and produces `openmouse.exe` in the
repository root.

With CMake:

```
cmake -B build -A x64
cmake --build build --config Release
```

With MinGW, including cross-compiling from Linux:

```
x86_64-w64-mingw32-g++ -std=c++17 -municode -mwindows -O2 -Wall -static \
    src/main.cpp src/devices.cpp src/overlay.cpp src/config.cpp \
    -o openmouse.exe -luser32 -lgdi32 -lshell32 -lole32 -luuid -ladvapi32 \
    -static-libgcc -static-libstdc++
```

`/utf-8` is required under MSVC. The sources are UTF-8 without a byte-order mark, and
without that flag MSVC decodes them as the system ANSI codepage, corrupting non-ASCII
characters inside wide string literals as well as narrow ones.

## Licence

MIT. See [LICENSE](LICENSE).
