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

// 5-byte E9 rel32 hotpatch. Known DXGI Present/ResizeBuffers prologues
// start with a complete 5-byte instruction (`48 89 5C 24 08` = mov [rsp+8],rbx),
// so the stolen window never straddles. If the target lies beyond ±2GB of
// our detour (rare for in-process DXGI), install fails cleanly instead of
// relocating anything.
//
// The trampoline is still a stolen-prologue + 14-byte abs jmp back, which
// IS allowed to live in private RX memory (it is called BY the game via our
// rel32, so return addresses land in game code — no 164 trip on the game
// side; our own code never calls the trampoline directly).
constexpr std::size_t kPatchLen = 5;

// Sanity check only — we accept any x64 function prologue whose first byte
// is a REX prefix (0x40-0x4F) or a common push (0x55, 0x53), covering every
// MSVC prologue DXGI methods have used across Win10/Win11/23H2 builds. The
// hotpatch guarantee means the first 5 bytes are always a complete
// instruction regardless of exact opcode, so the stolen window never
// straddles. A pre-flight that demanded one specific pattern was the
// cause of the "module heartbeat timeout" on Win11 — the match would
// fail, InstallPresent returned false, and the module boot went Fatal.
bool LooksLikeFunctionStart(const std::uint8_t* b) {
    const std::uint8_t x = b[0];
    return (x >= 0x40 && x <= 0x4F) || x == 0x55 || x == 0x53;
}

bool Fits32(std::intptr_t delta) {
    return delta >= -0x80000000LL && delta < 0x80000000LL;
}

void MakeRel32Jmp(std::uint8_t out[kPatchLen], void* from, void* to) {
    const std::intptr_t rel =
        reinterpret_cast<std::uint8_t*>(to) -
        (reinterpret_cast<std::uint8_t*>(from) + kPatchLen);
    out[0] = 0xE9;
    std::memcpy(out + 1, &rel, 4);
}

// 14-byte absolute JMP [RIP+0]; dq target
// 14-byte absolute far-jmp used ONLY in the trampoline tail.
constexpr std::size_t kFarJmpLen = 14;
void MakeFarJmp14(std::uint8_t out[kFarJmpLen], std::uint64_t target) {
    out[0] = 0xFF; out[1] = 0x25;
    out[2] = out[3] = out[4] = out[5] = 0x00;
    std::memcpy(out + 6, &target, 8);
}

// Trampoline = 5 stolen bytes + 14-byte abs-jmp back to (target + 5).
void* BuildTrampoline(void* target) {
    void* mem = VirtualAlloc(nullptr, 0x40, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) return nullptr;
    std::memcpy(mem, target, kPatchLen);
    std::uint8_t jmp[kFarJmpLen];
    MakeFarJmp14(jmp,
        reinterpret_cast<std::uint64_t>(static_cast<std::uint8_t*>(target) + kPatchLen));
    std::memcpy(static_cast<std::uint8_t*>(mem) + kPatchLen, jmp, kFarJmpLen);
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
            // Save game's prior RTV + DSV so our OMSet doesn't clobber the
            // binding ImGui will restore (ImGui's state backup inside
            // RenderDrawData only sees whatever is bound at call time).
            ID3D11RenderTargetView* prevRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
            ID3D11DepthStencilView* prevDsv = nullptr;
            ctx->OMGetRenderTargets(
                D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prevRtv, &prevDsv);
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            ctx->OMSetRenderTargets(
                D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prevRtv, prevDsv);
            for (auto* v : prevRtv) if (v) v->Release();
            if (prevDsv) prevDsv->Release();
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
    // Must release the RTV before calling the original; ResizeBuffers refuses
    // if any back-buffer reference is outstanding. Rebuild after resize.
    cheat::render::OnResizePre();
    HRESULT hr = g_origResize(sc, bufCount, w, h, fmt, flags);
    cheat::render::OnResizePost(w, h);
    return hr;
}

} // namespace

bool InstallPresent() {
    cheat::render::VTable vt;
    if (!cheat::render::ResolveSwapChainVTable(vt)) return false;

    // 5-byte rel32 requires target to be within ±2GB of the detour. For
    // in-process DXGI patched by code in our mapped image, this is almost
    // always true. If it isn't, bail — we don't quietly fall back to the
    // 14-byte abs-jmp that straddles prologues.
    const auto presentDelta =
        reinterpret_cast<std::intptr_t>(&PresentDetour) -
        reinterpret_cast<std::intptr_t>(vt.Present);
    const auto resizeDelta =
        reinterpret_cast<std::intptr_t>(&ResizeDetour) -
        reinterpret_cast<std::intptr_t>(vt.ResizeBuffers);
    if (!Fits32(presentDelta) || !Fits32(resizeDelta)) return false;

    std::uint8_t presentPatch[kPatchLen];
    MakeRel32Jmp(presentPatch, vt.Present, reinterpret_cast<void*>(&PresentDetour));
    void* presentTramp = BuildTrampoline(vt.Present);
    if (!presentTramp) return false;

    std::uint8_t resizePatch[kPatchLen];
    MakeRel32Jmp(resizePatch, vt.ResizeBuffers, reinterpret_cast<void*>(&ResizeDetour));
    void* resizeTramp = BuildTrampoline(vt.ResizeBuffers);
    if (!resizeTramp) {
        VirtualFree(presentTramp, 0, MEM_RELEASE);
        return false;
    }

    // Loose sanity check: first byte must be a plausible function-prologue
    // opcode. Any demanding pattern match would reject newer DXGI builds
    // whose prologue is just as hotpatch-safe but shaped differently.
    auto prologueOk = [](void* target) {
        return LooksLikeFunctionStart(static_cast<const std::uint8_t*>(target));
    };
    if (!prologueOk(vt.Present) || !prologueOk(vt.ResizeBuffers)) {
        VirtualFree(presentTramp, 0, MEM_RELEASE);
        VirtualFree(resizeTramp,  0, MEM_RELEASE);
        return false;
    }

    g_origPresent = reinterpret_cast<PresentFn>(presentTramp);
    g_origResize  = reinterpret_cast<ResizeBuffersFn>(resizeTramp);

    const bool okP = Install("Present", vt.Present,
            std::vector<std::uint8_t>(presentPatch, presentPatch + kPatchLen),
            presentTramp);
    const bool okR = Install("ResizeBuffers", vt.ResizeBuffers,
            std::vector<std::uint8_t>(resizePatch, resizePatch + kPatchLen),
            resizeTramp);
    if (!okP || !okR) {
        UninstallAll();              // restore anything that did arm
        g_origPresent = nullptr;
        g_origResize  = nullptr;
        return false;
    }
    return true;
}

void UninstallPresent() { UninstallAll(); }

bool PresentInFlight() { return g_inPresent.load(); }

} // namespace cheat::hooks
