// injector/ui_injector.cpp — ImGui window per §3.5.
// Three status rows + one INJECT button + log pane. No console.
#include "ui_injector.hpp"
#include "anticheat_gate.hpp"
#include "target_find.hpp"
#include "manual_map.hpp"
#include "randomize.hpp"
#ifdef SK_MODULE_STOMP
#include "module_stomp.hpp"
#endif

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

// ImGui v1.91 keeps its WndProc handler declaration behind `#if 0`.
// MUST live at GLOBAL scope (not inside anon ns) or the call site below
// binds to a local internal-linkage name and the link fails.
IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

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
    if (FAILED(g_dx.sc->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) {
        return false;
    }
    g_dx.dev->CreateRenderTargetView(back, nullptr, &g_dx.rtv);
    back->Release();
    return g_dx.rtv != nullptr;
}

void CleanupDevice() {
    if (g_dx.rtv) g_dx.rtv->Release();
    if (g_dx.sc)  g_dx.sc->Release();
    if (g_dx.ctx) g_dx.ctx->Release();
    if (g_dx.dev) g_dx.dev->Release();
    g_dx = {};
}

LRESULT WINAPI WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (::ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
    if (m == WM_SIZE && g_dx.dev && w != SIZE_MINIMIZED) {
        // ResizeBuffers rejects outstanding back-buffer refs AND bound views.
        // Unbind the ctx RTV before dropping ours.
        if (g_dx.ctx) g_dx.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        if (g_dx.rtv) { g_dx.rtv->Release(); g_dx.rtv = nullptr; }
        if (FAILED(g_dx.sc->ResizeBuffers(0, LOWORD(l), HIWORD(l),
                                         DXGI_FORMAT_UNKNOWN, 0))) {
            return 0; // leave rtv null; drawing path checks for it
        }
        ID3D11Texture2D* back = nullptr;
        if (SUCCEEDED(g_dx.sc->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
            g_dx.dev->CreateRenderTargetView(back, nullptr, &g_dx.rtv);
            back->Release();
        }
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

std::filesystem::path DefaultDllPath() {
    return randomize::CheatDllPathForThisRun();
}

enum class Stage { Scanning, AcBlocked, WaitingCS2, Injecting,
                   Watching, GameClosed, Fail };

struct State {
    Stage                    stage = Stage::Scanning;
    bool                     steamCheckedOnce = false;
    std::vector<ac::Status>  ac;
    std::optional<target::CS2Window> cs2;
    std::string              failMsg;

    // Self-destruct state (post-inject).
    HANDLE                   watchHandle = nullptr;         // cs2 process, SYNCHRONIZE
    std::filesystem::path    cheatDllPath;                  // deleted on cs2 exit
};

const char* StageText(Stage s) {
    switch (s) {
        case Stage::Scanning:    return "SCANNING";
        case Stage::AcBlocked:   return "BLOCKED";
        case Stage::WaitingCS2:  return "WAITING";
        case Stage::Injecting:   return "WORKING";
        case Stage::Watching:    return "WATCHING";
        case Stage::GameClosed:  return "CLEANED";
        case Stage::Fail:        return "FAILED";
    }
    return "?";
}

// Delete the file at `p`. If a direct delete fails (lock), retry briefly.
bool DeleteWithRetry(const std::filesystem::path& p) {
    for (int i = 0; i < 20; ++i) {
        if (DeleteFileW(p.c_str())) return true;
        Sleep(50);
    }
    return false;
}

// POSIX-delete the running staged exe. Opens the exe with DELETE share, flags
// it for POSIX-semantics delete, and lets ExitProcess finish the job. Falls
// back to MoveFileEx(DELAY_UNTIL_REBOOT) when the volume isn't NTFS or the
// SDK is too old to know the flag.
void SelfDestructOnExit() {
    wchar_t self[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return;

    HANDLE h = CreateFileW(self, DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        FILE_DISPOSITION_INFO_EX info{};
        info.Flags = 0x1 /* DELETE */ | 0x2 /* POSIX_SEMANTICS */;
        if (SetFileInformationByHandle(h, FileDispositionInfoEx,
                                       &info, sizeof(info))) {
            CloseHandle(h);
            return; // deletes on process exit
        }
        CloseHandle(h);
    }
    // Fallback: reboot-time delete. Not immediate but guaranteed.
    MoveFileExW(self, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
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

    // Small, fixed-size launcher window — no resize, no maximize so the UI
    // can't be stretched to full-screen. The cheat menu NEVER lives here;
    // it lives in-game via INSERT.
    const DWORD style =
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const int   wndW = 460;
    const int   wndH = 420;
    // Center on the primary monitor's work area so it never spawns under
    // the taskbar on 1440x1080 setups.
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int cx = work.left + ((work.right  - work.left) - wndW) / 2;
    const int cy = work.top  + ((work.bottom - work.top ) - wndH) / 2;
    g_hwnd = CreateWindowExW(0, wndClass, wndTitle, style,
                             cx, cy, wndW, wndH,
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
    // Only mark the Steam gate as satisfied once Steam was actually clear.
    // Audit catch: previously set unconditionally, meaning a user who
    // launched the loader with Steam running had the Steam row silently
    // dropped on the next poll and INJECT re-enabled.
    S.steamCheckedOnce = ac::AllClear(S.ac);
    S.stage = ac::AllClear(S.ac) ? Stage::WaitingCS2 : Stage::AcBlocked;

    bool quit = false;
    MSG msg{};
    while (!quit) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg);
            if (msg.message == WM_QUIT) quit = true;
        }
        if (quit) break;

        // Watch for CS2 exit (non-blocking). Fires once, then cleans up and
        // schedules our own exit on the next frame.
        if (S.stage == Stage::Watching && S.watchHandle) {
            if (WaitForSingleObject(S.watchHandle, 0) == WAIT_OBJECT_0) {
                LogLine("cs2 exited — deleting cheat.dll");
                DeleteWithRetry(S.cheatDllPath);
                SelfDestructOnExit();
                CloseHandle(S.watchHandle);
                S.watchHandle = nullptr;
                S.stage = Stage::GameClosed;
            }
        } else if (S.stage == Stage::GameClosed) {
            // One frame of "Cleaning up..." visible, then out.
            quit = true;
        }

        const auto now = std::chrono::steady_clock::now();
        // AC scans are toolhelp snapshots — EDR-loud. Fast cadence only
        // while blocked; stretch to ~3s once we're just waiting for CS2.
        const auto acPeriod = (S.stage == Stage::AcBlocked)
                                ? std::chrono::milliseconds(500)
                                : std::chrono::milliseconds(3000);
        if (now - lastPoll > acPeriod) {
            lastPoll = now;
            if (S.stage == Stage::AcBlocked || S.stage == Stage::WaitingCS2) {
                S.ac = ac::ScanProcesses(/*checkSteam*/ !S.steamCheckedOnce);
                if (S.steamCheckedOnce) {
                    S.ac.erase(std::remove_if(S.ac.begin(), S.ac.end(),
                        [](const ac::Status& s){ return s.family == ac::Family::Steam; }),
                        S.ac.end());
                } else if (ac::AllClear(S.ac)) {
                    S.steamCheckedOnce = true;
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
        // RECHECK is only meaningful before injection. During Watching we must
        // not reset the stage without first closing the cs2 handle (handle
        // leak + orphaned watcher per audit).
        const bool canRecheck =
            S.stage == Stage::AcBlocked || S.stage == Stage::WaitingCS2 ||
            S.stage == Stage::Fail;
        if (!canRecheck) ImGui::BeginDisabled();
        if (ImGui::Button("RECHECK", {120, 28})) {
            S.ac = ac::ScanProcesses(!S.steamCheckedOnce);
            if (S.steamCheckedOnce) {
                S.ac.erase(std::remove_if(S.ac.begin(), S.ac.end(),
                    [](const ac::Status& s){ return s.family == ac::Family::Steam; }),
                    S.ac.end());
            } else if (ac::AllClear(S.ac)) {
                S.steamCheckedOnce = true;
            }
            S.stage = ac::AllClear(S.ac) ? Stage::WaitingCS2 : Stage::AcBlocked;
            S.failMsg.clear();
        }
        if (!canRecheck) ImGui::EndDisabled();
        ImGui::SameLine();

        const bool canInject =
            ac::AllClear(S.ac) && S.cs2.has_value() &&
            (S.stage == Stage::WaitingCS2 || S.stage == Stage::Fail);
        if (!canInject) ImGui::BeginDisabled();
        if (ImGui::Button("INJECT", {160, 28})) {
            // Fresh attempt — clear stale state. If an earlier INJECT reached
            // Watching and then went Fail (couldn't open SYNCHRONIZE), close
            // the dangling handle before overwriting it.
            if (S.watchHandle) { CloseHandle(S.watchHandle); S.watchHandle = nullptr; }
            S.failMsg.clear();
            S.stage = Stage::Injecting;
            S.cheatDllPath = DefaultDllPath();
            LogLine("reading cheat.dll...");
            auto bytes = ReadFileBytes(S.cheatDllPath);
            if (bytes.empty()) {
                S.stage = Stage::Fail; S.failMsg = "cheat.dll missing next to loader";
                LogLine(S.failMsg.c_str());
            } else {
                // Resolve g_ipc's RVA from the DLL bytes so the mapper can
                // wait for the module to flip to Ready/Degraded before we
                // report success. Previously set to 0, which meant the
                // launcher reported "injected ok" without any confirmation
                // the module had even reached the Present hook.
                // 32-bit matches Options::readyFlagRva and PE RVAs.
                std::uint32_t ipcRva = 0;
                {
                    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(bytes.data());
                    if (bytes.size() > sizeof(IMAGE_DOS_HEADER) &&
                        dos->e_magic == IMAGE_DOS_SIGNATURE) {
                        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(
                            bytes.data() + dos->e_lfanew);
                        if (nt->Signature == IMAGE_NT_SIGNATURE) {
                            auto& dir = nt->OptionalHeader
                                .DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
                            if (dir.VirtualAddress && dir.Size) {
                                // Walk exports locally for g_ipc.
                                auto* sec = IMAGE_FIRST_SECTION(nt);
                                auto rva2off = [&](DWORD rva) -> DWORD {
                                    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
                                        if (rva >= sec[i].VirtualAddress &&
                                            rva <  sec[i].VirtualAddress + sec[i].Misc.VirtualSize)
                                            return rva - sec[i].VirtualAddress + sec[i].PointerToRawData;
                                    return 0;
                                };
                                DWORD edOff = rva2off(dir.VirtualAddress);
                                if (edOff && edOff + sizeof(IMAGE_EXPORT_DIRECTORY) <= bytes.size()) {
                                    auto* ed = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(
                                        bytes.data() + edOff);
                                    DWORD namesOff = rva2off(ed->AddressOfNames);
                                    DWORD ordsOff  = rva2off(ed->AddressOfNameOrdinals);
                                    DWORD funcsOff = rva2off(ed->AddressOfFunctions);
                                    if (namesOff && ordsOff && funcsOff) {
                                        auto* names = reinterpret_cast<DWORD*>(bytes.data() + namesOff);
                                        auto* ords  = reinterpret_cast<WORD*>(bytes.data() + ordsOff);
                                        auto* funcs = reinterpret_cast<DWORD*>(bytes.data() + funcsOff);
                                        for (DWORD i = 0; i < ed->NumberOfNames; ++i) {
                                            DWORD nOff = rva2off(names[i]);
                                            if (!nOff) continue;
                                            auto* n = reinterpret_cast<const char*>(bytes.data() + nOff);
                                            if (std::strcmp(n, "g_ipc") == 0) {
                                                ipcRva = funcs[ords[i]];
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

#ifdef SK_MODULE_STOMP
                LogLine("stomp mapping...");
                injector::stomp::Options opts;
                opts.readyFlagRva = ipcRva;
                auto r = injector::stomp::InjectStomp(S.cs2->pid, bytes, opts, &LogLine);
#else
                LogLine("manual mapping...");
                mapper::Options opts;
                opts.readyFlagRva = ipcRva;
                auto r = mapper::Inject(S.cs2->pid, bytes, opts, &LogLine);
#endif
                if (r.ok) {
                    LogLine("injected ok");
                    // SYNCHRONIZE handle for exit-watch. If OpenProcess fails,
                    // the inject still succeeded — stay in Watching, surface
                    // a note instead of flipping to Fail (previously a Fail
                    // here would re-enable INJECT and attempt a second map
                    // over the live module).
                    S.watchHandle = OpenProcess(SYNCHRONIZE, FALSE, S.cs2->pid);
                    if (S.watchHandle) {
                        S.stage = Stage::Watching;
                    } else {
                        S.stage = Stage::Watching;
                        S.failMsg = "injected ok (no SYNCHRONIZE: no auto-clean)";
                        LogLine(S.failMsg.c_str());
                    }
                } else {
                    S.stage = Stage::Fail; S.failMsg = r.stage + ": " + r.message;
                    LogLine(S.failMsg.c_str());
                }
                std::fill(bytes.begin(), bytes.end(), std::uint8_t{0});
            }
        }
        if (!canInject) ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Text("[%s]", StageText(S.stage));

        if (S.stage == Stage::Watching)
            ImGui::TextColored({0.3f,1,0.5f,1},
                "Injected. Loader will self-destruct when CS2 closes.");
        if (S.stage == Stage::GameClosed)
            ImGui::TextColored({0.3f,1,0.5f,1},
                "CS2 closed. Cleaning up and exiting...");
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

    // If the user closed the loader WHILE we were watching cs2, do the same
    // cleanup the watch-fire path does — otherwise cheat.dll + the staged
    // exe both sit in %TEMP%\<rand>\ as breadcrumbs forever.
    if (S.watchHandle && S.stage == Stage::Watching) {
        DeleteWithRetry(S.cheatDllPath);
        SelfDestructOnExit();
    }
    if (S.watchHandle) { CloseHandle(S.watchHandle); S.watchHandle = nullptr; }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDevice();
    DestroyWindow(g_hwnd);
    UnregisterClassW(wndClass, hInst);
    return 0;
}

} // namespace injector::ui
