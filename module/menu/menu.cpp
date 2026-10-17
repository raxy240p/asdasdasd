#include "menu.hpp"
#include "../../common/json.hpp"

#include <imgui.h>
#include <windows.h>
#include <fstream>

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

    s.WindowRounding    = 6.0f;
    s.ChildRounding     = 4.0f;
    s.FrameRounding     = 4.0f;
    s.PopupRounding     = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.GrabRounding      = 4.0f;
    s.TabRounding       = 4.0f;
    s.WindowBorderSize  = 0.0f;
    s.FrameBorderSize   = 0.0f;
    s.WindowPadding     = ImVec2(12, 12);
    s.FramePadding      = ImVec2(8, 4);
    s.ItemSpacing       = ImVec2(8, 6);
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

// Custom close X in the top-right of the window. Returns true if clicked.
// Preserves the layout cursor so the caller can keep stacking widgets after.
bool DrawCloseX() {
    const float r = 10.0f;
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    const ImVec2 center = { wp.x + ws.x - r - 10.0f, wp.y + r + 8.0f };
    auto* dl = ImGui::GetWindowDrawList();

    const ImVec2 saveCursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos({ center.x - r, center.y - r });
    ImGui::InvisibleButton("##closeX", { r * 2, r * 2 });
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();
    ImGui::SetCursorScreenPos(saveCursor);

    const ImU32 col = hovered ? IM_COL32(240, 90, 90, 255)
                              : IM_COL32(170, 70, 70, 220);
    dl->AddCircleFilled(center, r, col);
    const float d = r * 0.5f;
    const ImU32 glyph = IM_COL32(240, 240, 240, 255);
    dl->AddLine({ center.x - d, center.y - d }, { center.x + d, center.y + d }, glyph, 1.6f);
    dl->AddLine({ center.x - d, center.y + d }, { center.x + d, center.y - d }, glyph, 1.6f);
    return clicked;
}

} // namespace

void Init(const std::filesystem::path& configDir) {
    g_s.configDir = configDir;
    std::filesystem::create_directories(configDir);

    auto dbPath = configDir / "items.json";
    if (!std::filesystem::exists(dbPath)) {
        wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
        auto fallback = std::filesystem::path(self).parent_path() / "data" / "items.json";
        if (std::filesystem::exists(fallback)) dbPath = fallback;
    }
    auto r = cheat::items::Load(dbPath);
    if (r.ok) g_s.db = std::move(r.db);

    g_s.preview = cheat::items::MakeIconPreview(configDir / "icons");

    LoadConfig();
}

void Shutdown() {
    SaveConfig();
    g_s = {};
}

bool IsOpen() { return g_open; }
State& Get()  { return g_s; }

void Tick() {
    const bool nowDown = (GetAsyncKeyState(g_s.toggleVk) & 0x8000) != 0;
    if (nowDown && !g_togglePrev) g_open = !g_open;
    g_togglePrev = nowDown;

    if (!g_open) return;

    if (!g_styleApplied) { ApplyCleanStyle(); g_styleApplied = true; }

    // Centred default position, bounded default size — never full-screen,
    // never off-screen, always resizable, always closable via our X.
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
        // Custom top bar: title left, X right.
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.92f, 0.94f, 1.0f));
        ImGui::Text("skinloader");
        ImGui::PopStyleColor();
        if (DrawCloseX()) g_open = false;

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::BeginTabBar("##tabs")) {
            if (ImGui::BeginTabItem("Inventory")) { DrawInventoryTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Add Item"))  { DrawAddItemTab();   ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Settings"))  { DrawSettingsTab();  ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

bool SaveConfig() {
    nlohmann::json j;
    j["items"] = g_s.inventory;
    j["settings"]["toggleVk"] = g_s.toggleVk;
    std::ofstream f(g_s.configDir / "config.json");
    if (!f) return false;
    f << j.dump(2);
    return true;
}

bool LoadConfig() {
    std::ifstream f(g_s.configDir / "config.json");
    if (!f) return false;
    try {
        nlohmann::json j; f >> j;
        if (j.contains("items")) g_s.inventory = j["items"].get<decltype(g_s.inventory)>();
        if (j.contains("settings"))
            g_s.toggleVk = j["settings"].value("toggleVk", int{VK_INSERT});
    } catch (...) { return false; }
    return true;
}

} // namespace cheat::menu
