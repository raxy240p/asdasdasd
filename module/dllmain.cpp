// module/dllmain.cpp — minimal entry per §4.1 / §4.5.
//
// The injector hijacks a game thread into sk_init(). We export both sk_init
// AND DllMain so a conventional LoadLibrary path (dev-only, SK_DEBUG_LOAD)
// still boots. In the shipping path DllMain is NOT the entry — the injector
// calls sk_init directly (§3.4 step 8).
#include "../common/ipc.hpp"
#include "hooks/present_hook.hpp"
#include "hooks/framestage_hook.hpp"
#include "hooks/wndproc_hook.hpp"
#include "menu/menu.hpp"
#include "render/dx11_setup.hpp"
#include "opsec/mem_hygiene.hpp"
#include "opsec/call_gate.hpp"

#include <windows.h>
#include <filesystem>

#ifdef SK_VAC_RECON
namespace cheat::opsec::recon { void Init(); void Shutdown(); }
#endif

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
    const bool ok = cheat::hooks::InstallPresent();
    if (!ok) {
        g_ipc.state = static_cast<uint32_t>(ipc::State::Fatal);
        return; // no renderer host — the module stays silent, injector surfaces
    }
    // FrameStageNotify is the inventory-apply site per spec. If its sig
    // misses (CS2 update changed the function head), fall back to running
    // apply from inside PresentDetour — degraded but still functional.
    const bool fsOk = cheat::hooks::InstallFrameStageHook();
    cheat::hooks::SetFrameStageHooked(fsOk);
    if (!fsOk) {
        constexpr std::uint32_t kBitFrameStage = (1u << 15);
        g_ipc.degraded_mask |= kBitFrameStage;
    }
#ifdef SK_VAC_RECON
    cheat::opsec::recon::Init();
#endif
    // Present hook flips state to Ready/Degraded on the first frame.
}

void Teardown() {
    cheat::hooks::UninstallWndProcSubclass();
#ifdef SK_VAC_RECON
    cheat::opsec::recon::Shutdown();  // drain in-flight recon detours
#endif
    cheat::hooks::UninstallFrameStageHook();  // no-op; registry restores on UninstallAll
    cheat::hooks::UninstallPresent();
    // Spin until BOTH detours are quiescent. Each sets its own in-flight
    // flag as the first act of its body, so this catches threads past the
    // entry jmp but before the trailing stores.
    for (int i = 0; i < 50 && (cheat::hooks::PresentInFlight() ||
                               cheat::hooks::FrameStageInFlight()); ++i) {
        Sleep(2);
    }
    cheat::menu::Shutdown();
    cheat::render::Shutdown();
    cheat::opsec::UnloadAllGates();
}

} // namespace

// Dedicated boot thread. The injector hijacks a game thread and jumps it
// into sk_init via a shim; running Boot's heavy work (filesystem, dummy
// swap-chain creation, sig scans) on that hijacked thread kept it off
// the game loop for ~100ms+ and left any lock it held unreachable to the
// rest of the game. Freeze-while-booting was almost certainly this. The
// hijacked thread is now in-and-out — it spawns the worker and returns
// to its original RIP within a few hundred cycles.
DWORD WINAPI BootThread(LPVOID) {
    Boot();
    return 0;
}

extern "C" __declspec(dllexport) void sk_init() {
    static bool booted = false;
    if (booted) return;
    booted = true;
    HANDLE th = CreateThread(nullptr, 0, &BootThread, nullptr, 0, nullptr);
    if (th) CloseHandle(th);
    else    Boot();   // last-resort inline fallback — better slow than silent
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: sk_init();                 break;
        case DLL_PROCESS_DETACH: Teardown();                break;
        default: break;
    }
    return TRUE;
}
