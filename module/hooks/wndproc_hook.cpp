#include "wndproc_hook.hpp"
#include "../menu/menu.hpp"
#include "../render/dx11_setup.hpp"

#include <imgui.h>

namespace cheat::hooks {

namespace {

WNDPROC g_orig = nullptr;
HWND    g_hwnd = nullptr;

extern "C" LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

LRESULT CALLBACK Subclassed(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (cheat::render::IsBound()) {
        ImGui_ImplWin32_WndProcHandler(h, m, w, l);
        if (cheat::menu::IsOpen()) {
            auto& io = ImGui::GetIO();
            // Swallow input the menu wants, let game see the rest.
            if ((io.WantCaptureMouse    && (m == WM_LBUTTONDOWN || m == WM_LBUTTONUP ||
                                            m == WM_RBUTTONDOWN || m == WM_RBUTTONUP ||
                                            m == WM_MBUTTONDOWN || m == WM_MBUTTONUP ||
                                            m == WM_MOUSEWHEEL  || m == WM_MOUSEMOVE)) ||
                (io.WantCaptureKeyboard && (m == WM_KEYDOWN || m == WM_KEYUP ||
                                            m == WM_CHAR    || m == WM_SYSKEYDOWN ||
                                            m == WM_SYSKEYUP))) {
                return 1;
            }
        }
    }
    return CallWindowProcW(g_orig, h, m, w, l);
}

} // namespace

void EnsureWndProcSubclass() {
    if (g_orig) return;
    // Pick the window the game is rendering to.
    g_hwnd = FindWindowW(nullptr, L"Counter-Strike 2");
    if (!g_hwnd) return;
    g_orig = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(&Subclassed)));
}

void UninstallWndProcSubclass() {
    if (!g_orig || !g_hwnd) return;
    SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_orig));
    g_orig = nullptr;
    g_hwnd = nullptr;
}

bool IsMenuSwallowingInput() {
    return cheat::render::IsBound() && cheat::menu::IsOpen();
}

} // namespace cheat::hooks
