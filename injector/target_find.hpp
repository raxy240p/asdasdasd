#pragma once
#include <windows.h>
#include <optional>

namespace injector::target {

struct CS2Window {
    HWND  hwnd;
    DWORD pid;
};

// EnumWindows -> title == L"Counter-Strike 2" AND image-name == cs2.exe.
// Class name is intentionally NOT checked (§3.3).
std::optional<CS2Window> Find();

} // namespace injector::target
