// injector/target_find.cpp — EnumWindows per §3.3.
#include "target_find.hpp"

#include <psapi.h>
#include <algorithm>
#include <cwctype>
#include <string>

#pragma comment(lib, "psapi.lib")

namespace injector::target {

namespace {

constexpr const wchar_t* kExpectedTitle = L"Counter-Strike 2";
constexpr const wchar_t* kExpectedImage = L"cs2.exe";

std::wstring BaseNameLower(const std::wstring& path) {
    auto pos = path.find_last_of(L"\\/");
    std::wstring name = (pos == std::wstring::npos) ? path : path.substr(pos + 1);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return name;
}

struct EnumCtx { std::optional<CS2Window> hit; };

BOOL CALLBACK OnWindow(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lp);

    wchar_t title[128]{};
    if (GetWindowTextW(hwnd, title, 127) <= 0) return TRUE;
    if (std::wstring_view(title) != kExpectedTitle) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return TRUE;

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return TRUE;

    wchar_t path[MAX_PATH]{};
    DWORD n = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(h, 0, path, &n) != 0;
    CloseHandle(h);
    if (!ok) return TRUE;

    if (BaseNameLower(path) == kExpectedImage) {
        ctx->hit = CS2Window{ hwnd, pid };
        return FALSE; // stop
    }
    return TRUE;
}

} // namespace

std::optional<CS2Window> Find() {
    EnumCtx ctx{};
    EnumWindows(&OnWindow, reinterpret_cast<LPARAM>(&ctx));
    return ctx.hit;
}

} // namespace injector::target
