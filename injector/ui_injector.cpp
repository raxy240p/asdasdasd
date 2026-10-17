// injector/ui_injector.cpp — ImGui window per §3.5.
// Three status rows + one INJECT button + log pane. No console.
#include "ui_injector.hpp"
#include "anticheat_gate.hpp"
#include "target_find.hpp"
#include "manual_map.hpp"
#include "randomize.hpp"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <d3d11.h>
#include <tchar.h>

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#pragma comment(lib, "d3d11.lib")

namespace injector::ui {

namespace {

// ---------- DX11 bootstrap (minimal, no SDK deps) ----------
struct Dx {
    ID3D11Device*          dev = nullptr;
    ID3D11DeviceContext*   ctx = nullptr;
    IDXGISwapChain*        sc  = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
};
Dx                g_dx;
HWND              g_hwnd = nullptr;
std::deque<std::string> g_log;
std::mutex        g_logMx;

void LogLine(const char* s) {
    std::lock_guard<std::mutex> _(g_logMx);
    g_log.emplace_back(s);
    while (g_log.size() > 128) g_log.pop_front();
}

bool CreateDevice(HWND h) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = h;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        fls, 2, D3D11_SDK_VERSION, &sd,
        &g_dx.sc, &g_dx.dev, &got, &g_dx.ctx);
    if (FAILED(hr)) return false;

    ID3D11Texture2D* back = nullptr;
    g_dx.sc->GetBuffer(0, IID_PPV_ARGS(&back));
    g_dx.dev->CreateRenderTargetView(back, nullptr, &g_dx.rtv);
    back->Release();
    return true;
}

void CleanupDevice() {
    if (g_dx.rtv) g_dx.rtv->Release();
    if (g_dx.sc)  g_dx.sc->Release();
    if (g_dx.ctx) g_dx.ctx->Release();
    if (g_dx.dev) g_dx.dev->Release();
    g_dx = {};
}

extern "C" LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
LRESULT WINAPI WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
    if (m == WM_SIZE && g_dx.dev && w != SIZE_MINIMIZED) {
        if (g_dx.rtv) { g_dx.rtv->Release(); g_dx.rtv = nullptr; }
        g_dx.sc->ResizeBuffers(0, LOWORD(l), HIWORD(l), DXGI_FORMAT_UNKNOWN, 0);
        ID3D11Texture2D* back = nullptr;
        g_dx.sc->GetBuffer(0, IID_PPV_ARGS(&back));
        g_dx.dev->CreateRenderTargetView(back, nullptr, &g_dx.rtv);
        back->Release();
        return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(h, m, w, l);
}

std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
}

// Finds cheat.dll next to the running exe.
std::filesystem::path DefaultDllPath() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::filesystem::path p(self);
    return p.parent_path() / L"cheat.dll";
}

enum class Stage { Scanning, AcBlocked, WaitingCS2, Injecting, Done, Fail };

struct State {
    Stage                    stage = Stage::Scanning;
    bool                     steamCheckedOnce = false;
    std::vector<ac::Status>  ac;
    std::optional<target::CS2Window> cs2;
    std::string              failMsg;
};

const char* StageText(Stage s) {
    switch (s) {
        case Stage::Scanning:    return "SCANNING";
        case Stage::AcBlocked:   return "BLOCKED";
        case Stage::WaitingCS2:  return "WAITING";
        case Stage::Injecting:   return "WORKING";
        case Stage::Done:        return "INJECTED";
        case Stage::Fail:        return "FAILED";
    }
    return "?";
}

} // namespace

int Run(HINSTANCE hInst) {
    const wchar_t* wndClass = randomize::RandomWindowClass();
    const wchar_t* wndTitle = randomize::RandomWindowTitle();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = wndClass;
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, wndClass, wndTitle, WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 420, 360,
                             nullptr, nullptr, hInst, nullptr);
    if (!CreateDevice(g_hwnd)) { UnregisterClassW(wndClass, hInst); return 1; }

    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dx.dev, g_dx.ctx);

    State S;
    auto lastPoll = std::chrono::steady_clock::now();
    S.ac = ac::ScanProcesses(true);
    S.steamCheckedOnce = true;
    S.stage = ac::AllClear(S.ac) ? Stage::WaitingCS2 : Stage::AcBlocked;

    bool quit = false;
    MSG msg{};
    while (!quit) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg);
            if (msg.message == WM_QUIT) quit = true;
        }
        if (quit) break;

        const auto now = std::chrono::steady_clock::now();
        if (now - lastPoll > std::chrono::milliseconds(500)) {
            lastPoll = now;
            if (S.stage == Stage::AcBlocked || S.stage == Stage::WaitingCS2) {
                S.ac = ac::ScanProcesses(/*checkSteam*/ !S.steamCheckedOnce);
                // Steam row is checked only once; drop it on subsequent polls.
                if (S.steamCheckedOnce) {
                    S.ac.erase(std::remove_if(S.ac.begin(), S.ac.end(),
                        [](const ac::Status& s){ return s.family == ac::Family::Steam; }),
                        S.ac.end());
                }
                if (!ac::AllClear(S.ac)) S.stage = Stage::AcBlocked;
                else if (S.stage == Stage::AcBlocked) S.stage = Stage::WaitingCS2;
            }
            if (S.stage == Stage::WaitingCS2) {
                S.cs2 = target::Find();
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin(randomize::RandomImGuiTitle(), nullptr,
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

        ImGui::Text("Status");
        ImGui::Separator();
        for (const auto& row : S.ac) {
            const ImVec4 col = row.running ? ImVec4(1,0.3f,0.3f,1) : ImVec4(0.3f,1,0.4f,1);
            ImGui::TextColored(col, "  [%s] %-14ls %s",
                row.running ? "X" : "OK",
                row.label, row.running ? "RUNNING" : "clear");
        }
        {
            const ImVec4 col = S.cs2 ? ImVec4(0.3f,1,0.4f,1) : ImVec4(0.9f,0.9f,0.4f,1);
            ImGui::TextColored(col, "  [%s] CS2 window     %s",
                S.cs2 ? "OK" : "..", S.cs2 ? "running" : "waiting");
        }

        ImGui::Separator();
        if (ImGui::Button("RECHECK", {120, 28})) {
            S.ac = ac::ScanProcesses(!S.steamCheckedOnce);
            if (S.steamCheckedOnce) {
                S.ac.erase(std::remove_if(S.ac.begin(), S.ac.end(),
                    [](const ac::Status& s){ return s.family == ac::Family::Steam; }),
                    S.ac.end());
            }
            S.stage = ac::AllClear(S.ac) ? Stage::WaitingCS2 : Stage::AcBlocked;
        }
        ImGui::SameLine();

        const bool canInject =
            ac::AllClear(S.ac) && S.cs2.has_value() &&
            (S.stage == Stage::WaitingCS2 || S.stage == Stage::Fail);
        if (!canInject) ImGui::BeginDisabled();
        if (ImGui::Button("INJECT", {160, 28})) {
            S.stage = Stage::Injecting;
            LogLine("reading cheat.dll...");
            auto bytes = ReadFileBytes(DefaultDllPath());
            if (bytes.empty()) {
                S.stage = Stage::Fail; S.failMsg = "cheat.dll missing next to loader";
                LogLine(S.failMsg.c_str());
            } else {
                LogLine("manual mapping...");
                mapper::Options opts;
                // ipc::Block RVA is export-resolved post-build; 0 = skip wait.
                opts.readyFlagRva = 0;
                auto r = mapper::Inject(S.cs2->pid, bytes, opts, &LogLine);
                if (r.ok) { S.stage = Stage::Done; LogLine("injected ok"); }
                else      { S.stage = Stage::Fail; S.failMsg = r.stage + ": " + r.message;
                            LogLine(S.failMsg.c_str()); }
                // zero local DLL buffer per §6.6
                std::fill(bytes.begin(), bytes.end(), 0);
            }
        }
        if (!canInject) ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Text("[%s]", StageText(S.stage));

        if (S.stage == Stage::Done)
            ImGui::TextColored({0.3f,1,0.5f,1}, "Injected — you may close this.");
        if (S.stage == Stage::Fail)
            ImGui::TextColored({1,0.4f,0.4f,1}, "Fail: %s", S.failMsg.c_str());

        ImGui::Separator();
        ImGui::BeginChild("log", {0,0}, true);
        {
            std::lock_guard<std::mutex> _(g_logMx);
            for (const auto& l : g_log) ImGui::TextUnformatted(l.c_str());
        }
        ImGui::EndChild();
        ImGui::End();

        ImGui::Render();
        const float clear[4] = { 0.06f, 0.06f, 0.08f, 1.0f };
        g_dx.ctx->OMSetRenderTargets(1, &g_dx.rtv, nullptr);
        g_dx.ctx->ClearRenderTargetView(g_dx.rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_dx.sc->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDevice();
    DestroyWindow(g_hwnd);
    UnregisterClassW(wndClass, hInst);
    return 0;
}

} // namespace injector::ui
