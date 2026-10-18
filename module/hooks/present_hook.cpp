// module/hooks/present_hook.cpp — IDXGISwapChain vtable hook.
//
// Previous build used an inline detour with 5 stolen prologue bytes. That
// cost us on Win11 23H2+ DXGI where the Present prologue starts with
// `48 8B C4 ...` (3-byte `mov rax, rsp` then more), so stealing 5 bytes
// straddled the next instruction and the trampoline crashed on first call.
//
// Vtable swap: no prologue copy, no stolen bytes, no LDE. Just overwrite
// vtable[8] / vtable[13] with our detour pointers, keep the originals as
// our "trampoline" (plain function pointer calls). CS2 doesn't scan swap-
// chain vtables, so no detection cost over the inline detour.
//
// Rule stays: Present is the ONLY frame-time driver. Menu render, inventory
// apply, heartbeat, offset verify, header-erase sanity — all run here.
#include "present_hook.hpp"
#include "framestage_hook.hpp"
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

extern "C" ipc::Block g_ipc; // defined in dllmain.cpp

namespace cheat::hooks {

namespace {

PresentFn       g_origPresent  = nullptr;
ResizeBuffersFn g_origResize   = nullptr;

// Set true by Boot() when framestage_hook installed successfully; drives
// the fallback branch in PresentDetour.
bool            g_frameStageHooked = false;

// Vtable slot addresses we overwrote, kept so uninstall swaps the exact
// same slots back — resolving the vtable twice could race against a DXGI
// internal re-register and bind us to the wrong table on teardown.
void**          g_vtPresentSlot = nullptr;
void**          g_vtResizeSlot  = nullptr;

std::atomic<bool> g_inPresent{ false };

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

    g_ipc.heartbeat++;
    if (g_ipc.state == static_cast<uint32_t>(ipc::State::HookingDX)) {
        g_ipc.state = static_cast<uint32_t>(
            cheat::opsec::OffsetsVerified() ? ipc::State::Ready : ipc::State::Degraded);
    }

    // Inventory apply has moved to the FrameStageNotify hook (stage
    // FRAME_NET_UPDATE_END) per the cs2-sdk.com spec. Doing those writes
    // here raced the game's render-side reads of the same item-view
    // fields and was the freeze-then-crash on earlier builds. If the
    // FrameStageNotify sig misses, framestage_hook::InstallFrameStageHook
    // reports false and the fallback below runs apply inline — degraded
    // but still functional. Boot sets g_frameStageHooked accordingly.
    if (!cheat::hooks::FrameStageInFlight() && !g_frameStageHooked) {
        cheat::inventory::OnFrame();
    }

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

// Swap one vtable slot atomically. Returns the previous value, or nullptr
// on VirtualProtect failure. The vtable sits in dxgi.dll .rdata (RO); the
// WriteCopy page we land on is private per process, so writes don't leak.
void* SwapVtableSlot(void** slot, void* fn) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
    void* prev = InterlockedExchangePointer(slot, fn);
    DWORD tmp = 0;
    VirtualProtect(slot, sizeof(void*), old, &tmp);
    return prev;
}

} // namespace

bool InstallPresent() {
    cheat::render::VTable vt;
    if (!cheat::render::ResolveSwapChainVTable(vt) || !vt.table) return false;

    g_vtPresentSlot = &vt.table[8];
    g_vtResizeSlot  = &vt.table[13];

    void* prevPresent = SwapVtableSlot(g_vtPresentSlot,
                                       reinterpret_cast<void*>(&PresentDetour));
    if (!prevPresent) { g_vtPresentSlot = g_vtResizeSlot = nullptr; return false; }

    void* prevResize = SwapVtableSlot(g_vtResizeSlot,
                                      reinterpret_cast<void*>(&ResizeDetour));
    if (!prevResize) {
        // Roll back Present so we don't leave the game half-hooked.
        SwapVtableSlot(g_vtPresentSlot, prevPresent);
        g_vtPresentSlot = g_vtResizeSlot = nullptr;
        return false;
    }

    g_origPresent = reinterpret_cast<PresentFn>(prevPresent);
    g_origResize  = reinterpret_cast<ResizeBuffersFn>(prevResize);
    return true;
}

void UninstallPresent() {
    // Swap originals back, in reverse order. Keep the pointers until the
    // in-flight spin in dllmain::Teardown has cleared so a thread already
    // inside PresentDetour still has a valid g_origPresent to tail-call.
    if (g_vtResizeSlot && g_origResize)
        SwapVtableSlot(g_vtResizeSlot, reinterpret_cast<void*>(g_origResize));
    if (g_vtPresentSlot && g_origPresent)
        SwapVtableSlot(g_vtPresentSlot, reinterpret_cast<void*>(g_origPresent));
    g_vtPresentSlot = g_vtResizeSlot = nullptr;
    // Deliberately leave g_origPresent/Resize set for the tail-call path.
    // The DLL is about to be unloaded; nothing else races on them.
}

bool PresentInFlight() { return g_inPresent.load(); }

void SetFrameStageHooked(bool hooked) { g_frameStageHooked = hooked; }

} // namespace cheat::hooks
