// module/dllmain.cpp — minimal entry per §4.1 / §4.5.
//
// The injector hijacks a game thread into sk_init(). We export both sk_init
// AND DllMain so a conventional LoadLibrary path (dev-only, SK_DEBUG_LOAD)
// still boots. In the shipping path DllMain is NOT the entry — the injector
// calls sk_init directly (§3.4 step 8).
#include "../common/ipc.hpp"
#include "hooks/present_hook.hpp"
#include "menu/menu.hpp"
#include "render/dx11_setup.hpp"
#include "opsec/mem_hygiene.hpp"

#include <windows.h>
#include <filesystem>

extern "C" __declspec(dllexport) ipc::Block g_ipc = {
    ipc::kMagic, ipc::kVersion,
    static_cast<uint32_t>(ipc::State::Boot),
    0, 0, {0,0,0},
};

namespace {

std::filesystem::path ConfigDir() {
    wchar_t buf[MAX_PATH]; buf[0] = 0;
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH) == 0) {
        GetTempPathW(MAX_PATH, buf);
    }
    std::filesystem::path p(buf);
    // Random folder is applied by the injector before launch — the module
    // uses whatever folder the injector handed over via a env var. Fall back
    // to a stable name for first bring-up so dev builds work.
    wchar_t tag[128] = L"sk_cfg";
    GetEnvironmentVariableW(L"SK_CFG_DIR", tag, 128);
    return p / tag;
}

void Boot() {
    g_ipc.state = static_cast<uint32_t>(ipc::State::LoadingDb);
    cheat::menu::Init(ConfigDir());

    g_ipc.state = static_cast<uint32_t>(ipc::State::HookingDX);
    cheat::hooks::InstallPresent(); // Present + ResizeBuffers + first-frame WndProc

    // sk_init returns; the Present hook will flip state to Ready/Degraded on the
    // first frame it runs (after the game device is adopted).
}

void Teardown() {
    cheat::hooks::UninstallPresent();
    cheat::menu::Shutdown();
    cheat::render::Shutdown();
}

} // namespace

extern "C" __declspec(dllexport) void sk_init() {
    static bool booted = false;
    if (booted) return;
    booted = true;
    Boot();
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: sk_init();                 break;
        case DLL_PROCESS_DETACH: Teardown();                break;
        default: break;
    }
    return TRUE;
}
