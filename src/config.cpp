// openMouse — configuration
// SPDX-License-Identifier: MIT
//
// A hand-rolled INI reader rather than GetPrivateProfileString, because device
// interface paths are long and contain '#', '&' and '{}' — characters the Win32
// profile APIs handle poorly. Paths never contain '=', so splitting on the
// first '=' is unambiguous.
#include "openmouse.h"
#include <cstdio>
#include <cstdarg>
#include <shlobj.h>

namespace om {
namespace {

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::wstring ReadFileUtf16(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    std::string raw((size_t)size.QuadPart, '\0');
    DWORD read = 0;
    ReadFile(h, raw.data(), (DWORD)raw.size(), &read, nullptr);
    CloseHandle(h);
    raw.resize(read);

    // Accept UTF-8 with or without BOM; that is what an editor will produce.
    const char* p = raw.c_str();
    int len = (int)raw.size();
    if (len >= 3 && (BYTE)p[0] == 0xEF && (BYTE)p[1] == 0xBB && (BYTE)p[2] == 0xBF) {
        p += 3; len -= 3;
    }
    if (len <= 0) return L"";
    int need = MultiByteToWideChar(CP_UTF8, 0, p, len, nullptr, 0);
    std::wstring out(need, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p, len, &out[0], need);
    return out;
}

bool WriteFileUtf8(const std::wstring& path, const std::wstring& text) {
    int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                   nullptr, 0, nullptr, nullptr);
    std::string raw(need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                        &raw[0], need, nullptr, nullptr);

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(h, raw.data(), (DWORD)raw.size(), &written, nullptr);
    CloseHandle(h);
    return written == raw.size();
}

} // namespace

std::wstring Config::DefaultPath() {
    wchar_t* appdata = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
        const std::wstring roaming = appdata;
        CoTaskMemFree(appdata);
        dir = roaming + L"\\openMouse";
        CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring path = dir + L"\\openmouse.ini";

        // Carry over a configuration written under the project's former name.
        // Device bindings are measured by hand with --identify; silently
        // losing them on an upgrade would be a poor trade for a rename.
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            const std::wstring legacy = roaming + L"\\OpenMPX\\openmpx.ini";
            if (GetFileAttributesW(legacy.c_str()) != INVALID_FILE_ATTRIBUTES)
                CopyFileW(legacy.c_str(), path.c_str(), TRUE);
        }
        return path;
    }
    return L"openmouse.ini";
}

bool Config::Load(const std::wstring& path) {
    const std::wstring text = ReadFileUtf16(path);
    if (text.empty()) return false;

    std::wstring section;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = text.size();
        std::wstring line = Trim(text.substr(pos, eol - pos));
        pos = eol + 1;

        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line.front() == L'[' && line.back() == L']') {
            section = Trim(line.substr(1, line.size() - 2));
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = Trim(line.substr(0, eq));
        std::wstring val = Trim(line.substr(eq + 1));
        if (key.empty()) continue;

        if (section == L"general") {
            if      (key == L"seats")             seatCount        = _wtoi(val.c_str());
            else if (key == L"capture_keyboards") captureKeyboards = (val == L"1" || val == L"true");
            else if (key == L"mark_active_seat")  markActiveSeat   = (val == L"1" || val == L"true");
        } else if (section == L"mouse") {
            mouseBinding[key] = _wtoi(val.c_str());
        } else if (section == L"keyboard") {
            keyboardBinding[key] = _wtoi(val.c_str());
        } else if (section.rfind(L"seat", 0) == 0) {
            const int idx = _wtoi(section.substr(4).c_str());
            if (idx >= 0 && idx < kMaxSeats) {
                if ((int)sensitivity.size() <= idx) sensitivity.resize(idx + 1, 1.0);
                if ((int)seatColors.size()  <= idx) seatColors.resize(idx + 1, RGB(200,200,200));
                if (key == L"sensitivity") sensitivity[idx] = _wtof(val.c_str());
                else if (key == L"color") {
                    const unsigned long v = wcstoul(val.c_str(), nullptr, 16);
                    seatColors[idx] = RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
                }
            }
        }
    }
    if (seatCount < 1) seatCount = 1;
    if (seatCount > kMaxSeats) seatCount = kMaxSeats;
    return true;
}

bool Config::Save(const std::wstring& path) const {
    std::wstring t;
    t += L"; openMouse configuration\n";
    t += L"; Device keys are raw input interface paths. Run openmouse.exe --list\n";
    t += L"; to print the paths of every mouse and keyboard on this machine.\n\n";

    t += L"[general]\n";
    t += L"seats=" + std::to_wstring(seatCount) + L"\n";
    t += L"capture_keyboards=" + std::wstring(captureKeyboards ? L"1" : L"0") + L"\n";
    t += L"mark_active_seat="  + std::wstring(markActiveSeat  ? L"1" : L"0") + L"\n\n";

    for (int i = 0; i < seatCount; ++i) {
        wchar_t buf[128];
        const COLORREF c = (i < (int)seatColors.size()) ? seatColors[i] : RGB(200,200,200);
        const double s   = (i < (int)sensitivity.size()) ? sensitivity[i] : 1.0;
        swprintf(buf, 128, L"[seat%d]\ncolor=%02X%02X%02X\nsensitivity=%.2f\n\n",
                 i, GetRValue(c), GetGValue(c), GetBValue(c), s);
        t += buf;
    }

    t += L"[mouse]\n";
    for (const auto& kv : mouseBinding)
        t += kv.first + L"=" + std::to_wstring(kv.second) + L"\n";
    t += L"\n[keyboard]\n";
    for (const auto& kv : keyboardBinding)
        t += kv.first + L"=" + std::to_wstring(kv.second) + L"\n";

    return WriteFileUtf8(path, t);
}

// ---------------------------------------------------------------- logging

namespace {

// Opened by OpenLogFile(). The engine runs as a GUI process with no console,
// so without this the only trace is OutputDebugStringW, which needs a debugger
// attached to read. Diagnosing a system-wide input hook that way is not
// practical, hence the file.
HANDLE g_logFile = INVALID_HANDLE_VALUE;

// WriteConsoleW only works on a real console handle. When stdout is redirected
// to a pipe or file — which is what happens whenever the output is captured —
// it fails and the text is silently dropped. Fall back to a UTF-8 WriteFile.
void WriteWideTo(HANDLE h, const wchar_t* text, size_t len) {
    if (!h || h == INVALID_HANDLE_VALUE || len == 0) return;

    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        DWORD written = 0;
        WriteConsoleW(h, text, (DWORD)len, &written, nullptr);
        return;
    }

    const int need = WideCharToMultiByte(CP_UTF8, 0, text, (int)len,
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) return;
    std::string raw((size_t)need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, (int)len, &raw[0], need, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(h, raw.data(), (DWORD)raw.size(), &written, nullptr);
}

} // namespace

// Appends to %APPDATA%\openMouse\openmouse.log. Safe to call more than once.
void OpenLogFile() {
    if (g_logFile != INVALID_HANDLE_VALUE) return;
    std::wstring path = Config::DefaultPath();
    const size_t slash = path.rfind(L'\\');
    path = (slash == std::wstring::npos) ? std::wstring(L"openmouse.log")
                                         : path.substr(0, slash + 1) + L"openmouse.log";
    // Start fresh once the log passes a few megabytes. This runs for hours at
    // a time and appends on every session; without a cap it grows without
    // bound and the interesting lines are unfindable.
    constexpr LONGLONG kMaxLogBytes = 4LL * 1024 * 1024;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    DWORD disposition = OPEN_ALWAYS;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        const LONGLONG size = ((LONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        if (size > kMaxLogBytes) disposition = CREATE_ALWAYS;
    }

    // Both share flags: the engine and a diagnostic mode (--monitor) may have
    // this open at once, and appends interleave safely.
    g_logFile = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void CloseLogFile() {
    if (g_logFile != INVALID_HANDLE_VALUE) { CloseHandle(g_logFile); g_logFile = INVALID_HANDLE_VALUE; }
}

void LogLine(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1023, fmt, ap);
    va_end(ap);
    buf[1023] = 0;

    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");

    const size_t len = wcslen(buf);
    WriteWideTo(GetStdHandle(STD_OUTPUT_HANDLE), buf, len);
    WriteWideTo(GetStdHandle(STD_OUTPUT_HANDLE), L"\r\n", 2);

    if (g_logFile != INVALID_HANDLE_VALUE) {
        WriteWideTo(g_logFile, buf, len);
        WriteWideTo(g_logFile, L"\r\n", 2);
    }
}

} // namespace om
