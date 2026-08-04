// openMouse — raw input device enumeration
// SPDX-License-Identifier: MIT
#include "openmouse.h"
#include <algorithm>

namespace om {

std::wstring DevicePathFromHandle(HANDLE h) {
    if (!h) return L"";
    UINT len = 0;
    if (GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, nullptr, &len) != 0 || len == 0)
        return L"";
    std::wstring buf(len + 1, L'\0');
    UINT written = len + 1;
    if (GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, &buf[0], &written) == (UINT)-1)
        return L"";
    buf.resize(wcslen(buf.c_str()));
    return buf;
}

// Raw input hands us an interface path of the form
//     \\?\HID#VID_046D&PID_C52B&MI_01#7&2f8e7b0d&0&0000#{4d1e55b2-...}
// The corresponding registry key holding a human-readable description is
//     HKLM\SYSTEM\CurrentControlSet\Enum\HID\VID_046D&PID_C52B&MI_01\7&2f8e7b0d&0&0000
// so we strip the \\?\ prefix, drop the trailing interface GUID, and swap
// '#' separators for backslashes.
static std::wstring PathToEnumKey(const std::wstring& devPath) {
    std::wstring s = devPath;
    if (s.rfind(L"\\\\?\\", 0) == 0) s = s.substr(4);

    // Drop the trailing "#{guid}" component if present.
    size_t brace = s.find(L'{');
    if (brace != std::wstring::npos) {
        size_t hash = s.rfind(L'#', brace);
        if (hash != std::wstring::npos) s = s.substr(0, hash);
    }
    std::replace(s.begin(), s.end(), L'#', L'\\');
    return L"SYSTEM\\CurrentControlSet\\Enum\\" + s;
}

std::wstring FriendlyNameFromPath(const std::wstring& path) {
    const std::wstring key = PathToEnumKey(path);
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return L"";

    auto readValue = [&](const wchar_t* name) -> std::wstring {
        DWORD type = 0, cb = 0;
        if (RegQueryValueExW(hk, name, nullptr, &type, nullptr, &cb) != ERROR_SUCCESS ||
            type != REG_SZ || cb == 0)
            return L"";
        std::wstring out(cb / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(hk, name, nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(&out[0]), &cb) != ERROR_SUCCESS)
            return L"";
        out.resize(wcslen(out.c_str()));
        return out;
    };

    // FriendlyName is nicer when present; DeviceDesc always is, but is
    // prefixed with a resource path we have to trim.
    std::wstring name = readValue(L"FriendlyName");
    if (name.empty()) {
        name = readValue(L"DeviceDesc");
        size_t semi = name.rfind(L';');
        if (semi != std::wstring::npos) name = name.substr(semi + 1);
    }
    RegCloseKey(hk);
    return name;
}

std::vector<DeviceInfo> EnumerateDevices() {
    std::vector<DeviceInfo> result;

    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0)
        return result;

    std::vector<RAWINPUTDEVICELIST> list(count);
    UINT got = GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST));
    if (got == (UINT)-1) return result;
    list.resize(got);

    for (const auto& d : list) {
        if (d.dwType != RIM_TYPEMOUSE && d.dwType != RIM_TYPEKEYBOARD)
            continue;

        DeviceInfo info;
        info.handle = d.hDevice;
        info.type   = d.dwType;
        info.path   = DevicePathFromHandle(d.hDevice);
        if (info.path.empty()) continue;

        // Terminal Services and injected-input pseudo devices show up here and
        // must not be bound to a seat — they are not physical hardware.
        if (info.path.find(L"RDP_MOU") != std::wstring::npos ||
            info.path.find(L"RDP_KBD") != std::wstring::npos)
            continue;

        info.friendlyName = FriendlyNameFromPath(info.path);
        if (info.friendlyName.empty())
            info.friendlyName = (d.dwType == RIM_TYPEMOUSE) ? L"Mouse" : L"Keyboard";

        result.push_back(std::move(info));
    }
    return result;
}

} // namespace om
