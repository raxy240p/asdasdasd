// module/render/dx11_setup.hpp — target-process DX11 bringup.
//
// Trick: create a dummy swap chain locally to read the real IDXGISwapChain
// vtable (Present / ResizeBuffers). The game's own device/swapchain is adopted
// from the first Present detour call. ImGui dx11+win32 backends run against
// that. No new device, no new window.
#pragma once
#include <d3d11.h>
#include <dxgi.h>

namespace cheat::render {

struct VTable {
    void* Present        = nullptr;
    void* ResizeBuffers  = nullptr;
};

bool    ResolveSwapChainVTable(VTable& out);
void    OnPresent_AdoptDevice(IDXGISwapChain* sc);  // called from Present detour
void    Shutdown();

ID3D11Device*          GameDevice();
ID3D11DeviceContext*   GameContext();
ID3D11RenderTargetView* MainRtv();

bool    IsBound();          // ImGui initialised against game device
void    OnResizePre();                 // drop RTV before game's ResizeBuffers
void    OnResizePost(UINT w, UINT h);  // rebuild RTV after it returns

} // namespace cheat::render
