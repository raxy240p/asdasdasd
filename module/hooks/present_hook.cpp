// module/hooks/present_hook.cpp — inline detour on Present + ResizeBuffers.
// Trampolines reconstructed via a small length-disassembly table. Dependency-
// free so no MinHook/polyhook — fewer footprints, fewer imports.
//
// Rule: Present is the ONLY frame-time driver. Menu render, inventory apply,
// heartbeat, offset verify, header-erase sanity — all run here (§4.1 / §6.1).
#include "present_hook.hpp"
#include "hook_registry.hpp"
#include "wndproc_hook.hpp"
#include "../render/dx11_setup.hpp"
#include "../menu/menu.hpp"
#include "../inventory/backend.hpp"
#include "../opsec/mem_hygiene.hpp"
#include "../../common/ipc.hpp"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <windows.h>
#include <atomic>
#include <cstring>

extern "C" ipc::Block g_ipc; // defined in dllmain.cpp

namespace cheat::hooks {

namespace {

PresentFn       g_origPresent  = nullptr;
ResizeBuffersFn g_origResize   = nullptr;

std::atomic<bool> g_inPresent{ false };

// Minimal LDE for a 14-byte absolute JMP overwrite. We copy that many bytes
// into the trampoline, append the far jump back. For a single build target
// this is enough; a per-build sig audit catches instruction overlap.
constexpr std::size_t kPatchLen = 14;

// 14-byte absolute JMP [RIP+0]; dq target
void MakeFarJmp(std::uint8_t out[kPatchLen], std::uint64_t target) {
    out[0] = 0xFF; out[1] = 0x25;
    out[2] = out[3] = out[4] = out[5] = 0x00;
    std::memcpy(out + 6, &target, 8);
}

// Allocate an executable trampoline: original 14 bytes + 14-byte jmp back.
void* BuildTrampoline(void* target) {
    void* mem = VirtualAlloc(nullptr, 0x40, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) return nullptr;
    std::memcpy(mem, target, kPatchLen);
    std::uint8_t jmp[kPatchLen];
    MakeFarJmp(jmp,
        reinterpret_cast<std::uint64_t>(static_cast<std::uint8_t*>(target) + kPatchLen));
    std::memcpy(static_cast<std::uint8_t*>(mem) + kPatchLen, jmp, kPatchLen);
    DWORD old;
    VirtualProtect(mem, 0x40, PAGE_EXECUTE_READ, &old);
    return mem;
}

HRESULT WINAPI PresentDetour(IDXGISwapChain* sc, UINT sync, UINT flags) {
    g_inPresent.store(true);

    cheat::render::OnPresent_AdoptDevice(sc);
    cheat::hooks::EnsureWndProcSubclass();

    if (cheat::render::IsBound()) {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        cheat::menu::Tick();        // handles INSERT toggle + draws menu

        ImGui::Render();
        auto* ctx = cheat::render::GameContext();
        auto* rtv = cheat::render::MainRtv();
        if (ctx && rtv) {
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
    }

    // Heartbeat + state. See common/ipc.hpp.
    g_ipc.heartbeat++;
    if (g_ipc.state == static_cast<uint32_t>(ipc::State::HookingDX)) {
        g_ipc.state = static_cast<uint32_t>(
            cheat::opsec::OffsetsVerified() ? ipc::State::Ready : ipc::State::Degraded);
    }

    // Dirty-apply contract — game thread only (§4.5).
    cheat::inventory::OnFrame();

    g_inPresent.store(false);
    return g_origPresent(sc, sync, flags);
}

HRESULT WINAPI ResizeDetour(IDXGISwapChain* sc, UINT bufCount, UINT w, UINT h,
                            DXGI_FORMAT fmt, UINT flags)
{
    cheat::render::OnResize(w, h);
    return g_origResize(sc, bufCount, w, h, fmt, flags);
}

} // namespace

bool InstallPresent() {
    cheat::render::VTable vt;
    if (!cheat::render::ResolveSwapChainVTable(vt)) return false;

    std::uint8_t presentPatch[kPatchLen];
    MakeFarJmp(presentPatch, reinterpret_cast<std::uint64_t>(&PresentDetour));
    void* presentTramp = BuildTrampoline(vt.Present);
    if (!presentTramp) return false;
    g_origPresent = reinterpret_cast<PresentFn>(presentTramp);

    std::uint8_t resizePatch[kPatchLen];
    MakeFarJmp(resizePatch, reinterpret_cast<std::uint64_t>(&ResizeDetour));
    void* resizeTramp = BuildTrampoline(vt.ResizeBuffers);
    if (!resizeTramp) return false;
    g_origResize = reinterpret_cast<ResizeBuffersFn>(resizeTramp);

    Install("Present", vt.Present,
            std::vector<std::uint8_t>(presentPatch, presentPatch + kPatchLen),
            presentTramp);
    Install("ResizeBuffers", vt.ResizeBuffers,
            std::vector<std::uint8_t>(resizePatch, resizePatch + kPatchLen),
            resizeTramp);
    return true;
}

void UninstallPresent() { UninstallAll(); }

} // namespace cheat::hooks
