#include "dx11_setup.hpp"
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <filesystem>

#pragma comment(lib, "d3d11.lib")

namespace cheat::render {

namespace {

ID3D11Device*           g_dev = nullptr;
ID3D11DeviceContext*    g_ctx = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
IDXGISwapChain*         g_sc  = nullptr;
bool                    g_bound = false;
ImFont*                 g_headerFont = nullptr;

void MakeRTV() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_sc->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

} // namespace

bool ResolveSwapChainVTable(VTable& out) {
    // Build a throwaway swap chain against a hidden window to steal vtable slots.
    HWND tmp = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 1, 1,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!tmp) return false;

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = tmp;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device*    dv = nullptr;
    ID3D11DeviceContext* cx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        fls, 1, D3D11_SDK_VERSION, &sd, &sc, &dv, &got, &cx);
    if (FAILED(hr)) { DestroyWindow(tmp); return false; }

    void** vt = *reinterpret_cast<void***>(sc);
    out.Present       = vt[8];  // IDXGISwapChain::Present
    out.ResizeBuffers = vt[13]; // IDXGISwapChain::ResizeBuffers
    out.table         = vt;     // base — used by the vtable hook installer

    sc->Release(); cx->Release(); dv->Release();
    DestroyWindow(tmp);
    return true;
}

void OnPresent_AdoptDevice(IDXGISwapChain* sc) {
    if (g_bound) return;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_dev)))) {
        g_dev = nullptr;
        return;
    }
    // AddRef so our stored g_sc doesn't dangle if the game ever drops its
    // reference. Released alongside the device in Shutdown().
    sc->AddRef();
    g_sc = sc;
    g_dev->GetImmediateContext(&g_ctx);
    MakeRTV();

    DXGI_SWAP_CHAIN_DESC desc{};
    sc->GetDesc(&desc);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    // Load Segoe UI as the default; fall back to ImGui's built-in font if
    // the system TTF isn't where we expect it (sandboxed, Wine, etc.).
    ImGuiIO& io = ImGui::GetIO();
    const char* kSegoe     = "C:\\Windows\\Fonts\\segoeui.ttf";
    const char* kSegoeBold = "C:\\Windows\\Fonts\\segoeuib.ttf";
    if (std::filesystem::exists(kSegoe)) io.Fonts->AddFontFromFileTTF(kSegoe, 16.0f);
    else                                 io.Fonts->AddFontDefault();
    if (std::filesystem::exists(kSegoeBold))
        g_headerFont = io.Fonts->AddFontFromFileTTF(kSegoeBold, 15.0f);

    ImGui_ImplWin32_Init(desc.OutputWindow);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    g_bound = true;
}

ImFont* GetHeaderFont() { return g_headerFont; }

void OnResizePre() {
    if (!g_bound) return;
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}
void OnResizePost(UINT /*w*/, UINT /*h*/) {
    if (!g_bound) return;
    MakeRTV();
}

void Shutdown() {
    if (!g_bound) return;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_rtv) g_rtv->Release();
    if (g_sc)  g_sc->Release();   // released the AddRef from OnPresent_AdoptDevice
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    g_rtv = nullptr; g_ctx = nullptr; g_dev = nullptr; g_sc = nullptr;
    g_bound = false;
}

ID3D11Device*          GameDevice()  { return g_dev; }
ID3D11DeviceContext*   GameContext() { return g_ctx; }
ID3D11RenderTargetView* MainRtv()    { return g_rtv; }
bool                   IsBound()     { return g_bound; }

} // namespace cheat::render
