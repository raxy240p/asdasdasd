#include "menu.hpp"
#include "../../common/json.hpp"
#include "../render/dx11_setup.hpp"
#include "../inventory/backend.hpp"

#include <imgui.h>
#include <windows.h>
#include <cstdlib>
#include <fstream>
#include <system_error>

namespace cheat::menu {

// Sibling tab files define these.
void DrawInventoryTab();
void DrawAddItemTab();
void DrawSettingsTab();

namespace {
State g_s;
bool  g_open = false;
bool  g_togglePrev = false;
bool  g_styleApplied = false;

// Clean dark palette — flat, no window chrome, subtle accent.
// Mirrors the "clean imgui" look (vayn1337/Clean-imgui-menu reference,
// which turns out to ship upstream ImGui — the look is pure styling).
void ApplyCleanStyle() {
    auto& s  = ImGui::GetStyle();
    auto& co = s.Colors;

    s.WindowRounding    = 8.0f;
    s.ChildRounding     = 4.0f;
    s.FrameRounding     = 4.0f;
    s.PopupRounding     = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.GrabRounding      = 4.0f;
    s.TabRounding       = 4.0f;
    s.WindowBorderSize  = 0.0f;
    s.FrameBorderSize   = 0.0f;
    s.WindowPadding     = ImVec2(10, 10);
    s.FramePadding      = ImVec2(8, 4);
    s.ItemSpacing       = ImVec2(7, 5);
    s.ItemInnerSpacing  = ImVec2(6, 4);
    s.ScrollbarSize     = 12.0f;

    const ImVec4 bg       = { 0.08f, 0.09f, 0.11f, 0.96f };
    const ImVec4 panel    = { 0.11f, 0.12f, 0.15f, 1.00f };
    const ImVec4 panelHi  = { 0.14f, 0.16f, 0.19f, 1.00f };
    const ImVec4 border   = { 0.20f, 0.22f, 0.26f, 0.60f };
    const ImVec4 text     = { 0.90f, 0.92f, 0.94f, 1.00f };
    const ImVec4 textDim  = { 0.55f, 0.58f, 0.62f, 1.00f };
    const ImVec4 accent   = { 0.33f, 0.65f, 1.00f, 1.00f };
    const ImVec4 accentHi = { 0.40f, 0.72f, 1.00f, 1.00f };
    const ImVec4 accentLo = { 0.22f, 0.48f, 0.80f, 1.00f };

    co[ImGuiCol_WindowBg]           = bg;
    co[ImGuiCol_ChildBg]            = panel;
    co[ImGuiCol_PopupBg]            = panel;
    co[ImGuiCol_Border]             = border;
    co[ImGuiCol_FrameBg]            = panel;
    co[ImGuiCol_FrameBgHovered]     = panelHi;
    co[ImGuiCol_FrameBgActive]      = panelHi;
    co[ImGuiCol_TitleBg]            = bg;
    co[ImGuiCol_TitleBgActive]      = bg;
    co[ImGuiCol_TitleBgCollapsed]   = bg;
    co[ImGuiCol_MenuBarBg]          = panel;
    co[ImGuiCol_ScrollbarBg]        = bg;
    co[ImGuiCol_ScrollbarGrab]      = panelHi;
    co[ImGuiCol_ScrollbarGrabHovered]= accentLo;
    co[ImGuiCol_ScrollbarGrabActive]= accent;
    co[ImGuiCol_CheckMark]          = accent;
    co[ImGuiCol_SliderGrab]         = accent;
    co[ImGuiCol_SliderGrabActive]   = accentHi;
    co[ImGuiCol_Button]             = panelHi;
    co[ImGuiCol_ButtonHovered]      = accentLo;
    co[ImGuiCol_ButtonActive]       = accent;
    co[ImGuiCol_Header]             = panelHi;
    co[ImGuiCol_HeaderHovered]      = accentLo;
    co[ImGuiCol_HeaderActive]       = accent;
    co[ImGuiCol_Separator]          = border;
    co[ImGuiCol_SeparatorHovered]   = accentLo;
    co[ImGuiCol_SeparatorActive]    = accent;
    co[ImGuiCol_ResizeGrip]         = border;
    co[ImGuiCol_ResizeGripHovered]  = accentLo;
    co[ImGuiCol_ResizeGripActive]   = accent;
    co[ImGuiCol_Tab]                = panel;
    co[ImGuiCol_TabHovered]         = accentLo;
    co[ImGuiCol_TabActive]          = accent;
    co[ImGuiCol_TabUnfocused]       = panel;
    co[ImGuiCol_TabUnfocusedActive] = accentLo;
    co[ImGuiCol_Text]               = text;
    co[ImGuiCol_TextDisabled]       = textDim;
    co[ImGuiCol_TextSelectedBg]     = accent;
    co[ImGuiCol_TableHeaderBg]      = panelHi;
    co[ImGuiCol_TableBorderStrong]  = border;
    co[ImGuiCol_TableBorderLight]   = border;
}

} // namespace

void Init(const std::filesystem::path& configDir) {
    g_s.configDir = configDir;
    // Non-throwing overload — an unhandled filesystem_error here would
    // unwind out of sk_init on a thread the engine hijacked, and without
    // RtlAddFunctionTable in our manual-map path the unwind can corrupt
    // the game. A failed mkdir is tolerable; later writes will error.
    std::error_code mkec;
    std::filesystem::create_directories(configDir, mkec);

    auto dbPath = configDir / "items.json";
    if (!std::filesystem::exists(dbPath)) {
        // Prefer the injector-provided staged location, then fall back to the
        // path beside cs2.exe.
        if (const char* env = std::getenv("SK_ITEMS_DB")) {
            std::filesystem::path staged{env};
            if (std::filesystem::exists(staged)) dbPath = staged;
        }
        if (!std::filesystem::exists(dbPath)) {
            wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
            auto fallback = std::filesystem::path(self).parent_path() / "data" / "items.json";
            if (std::filesystem::exists(fallback)) dbPath = fallback;
        }
    }
    auto r = cheat::items::Load(dbPath);
    if (r.ok) g_s.db = std::move(r.db);

    g_s.preview = cheat::items::MakeIconPreview(configDir / "icons");

    LoadConfig();
}

void Shutdown() {
    SaveConfig();
    // State has an atomic<bool>, so copy-assign is deleted. Reset in place.
    g_s.inventory.clear();
    g_s.db = cheat::items::Db{};
    g_s.dirty.store(false);
    g_s.preview.reset();
    g_s.selCategory.clear();
    g_s.selWeapon.clear();
    g_s.selPaintKit = -1;
    g_s.editing = {};
    g_s.editorOpen = false;
    g_s.editingIdx = -1;
    g_s.toggleVk = VK_INSERT;
    g_s.showOffsetsStatus = false;
    g_s.requestedTab.clear();
}

bool IsOpen() { return g_open; }
State& Get()  { return g_s; }

void Tick() {
    // Only sample the toggle when CS2 is the foreground window — otherwise
    // INSERT typed in a chat window or editor would flip us.
    HWND cs2 = FindWindowW(nullptr, L"Counter-Strike 2");
    const bool fg = (cs2 != nullptr && GetForegroundWindow() == cs2);
    const bool nowDown = fg && (GetAsyncKeyState(g_s.toggleVk) & 0x8000) != 0;
    if (nowDown && !g_togglePrev) g_open = !g_open;
    g_togglePrev = nowDown;

    if (!g_open) return;

    if (!g_styleApplied) { ApplyCleanStyle(); g_styleApplied = true; }

    // Centred default position, bounded default size — never full-screen,
    // never off-screen. INSERT is the only close path.
    const ImVec2 defSize{ 760, 520 };
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 defPos{ (display.x - defSize.x) * 0.5f,
                         (display.y - defSize.y) * 0.5f };

    ImGui::SetNextWindowSize(defSize, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos (defPos,  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints({ 480, 320 }, { 2000, 1400 });

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |     // no Windows chrome
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;

    if (ImGui::Begin("##cheat_main", nullptr, flags)) {
        // Top strip: title only. INSERT toggles the menu — no in-window X,
        // the only close chrome belongs to the loader (native Win32).
        ImFont* hf = cheat::render::GetHeaderFont();
        if (hf) ImGui::PushFont(hf);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.92f, 0.94f, 1.0f));
        ImGui::Text("skinloader   [INSERT to close]");
        ImGui::PopStyleColor();
        if (hf) ImGui::PopFont();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::BeginTabBar("##tabs")) {
            auto tabFlags = [](const char* name) -> ImGuiTabItemFlags {
                if (g_s.requestedTab == name) {
                    g_s.requestedTab.clear();
                    return ImGuiTabItemFlags_SetSelected;
                }
                return 0;
            };
            if (ImGui::BeginTabItem("Inventory", nullptr, tabFlags("Inventory"))) { DrawInventoryTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Add Item",  nullptr, tabFlags("Add Item")))  { DrawAddItemTab();   ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Settings",  nullptr, tabFlags("Settings")))  { DrawSettingsTab();  ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

bool SaveConfig() {
    // Serialize to a buffer first so a dump() exception (e.g. invalid UTF-8
    // from a truncated multibyte customName) can't leave a half-written
    // file behind. Then write via rename-over-temp so an interrupted write
    // never corrupts the live config either.
    std::string body;
    try {
        nlohmann::json j;
        j["items"] = g_s.inventory;
        j["settings"]["toggleVk"]        = g_s.toggleVk;
        j["settings"]["enableKnifeSwap"] = g_s.enableKnifeSwap;
        body = j.dump(2);
    } catch (...) { return false; }

    const auto cfg = g_s.configDir / "config.json";
    const auto tmp = g_s.configDir / "config.json.tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
        f.flush();
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, cfg, ec);
    if (ec) {
        std::filesystem::remove(cfg, ec);
        std::filesystem::rename(tmp, cfg, ec);
    }
    return !ec;
}

bool LoadConfig() {
    std::ifstream f(g_s.configDir / "config.json");
    if (!f) return false;
    try {
        nlohmann::json j; f >> j;
        if (j.contains("items")) g_s.inventory = j["items"].get<decltype(g_s.inventory)>();
        if (j.contains("settings")) {
            g_s.toggleVk        = j["settings"].value("toggleVk", int{VK_INSERT});
            g_s.enableKnifeSwap = j["settings"].value("enableKnifeSwap", false);
        }
    } catch (...) { return false; }
    // Loaded entries must auto-apply to the live weapon views; previously
    // we populated inventory without touching dirty, so saved skins only
    // took effect after the user clicked Apply.
    g_s.dirty.store(true);
    return true;
}

} // namespace cheat::menu
