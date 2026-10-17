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
} // namespace

void Init(const std::filesystem::path& configDir) {
    g_s.configDir = configDir;
    std::filesystem::create_directories(configDir);

    // DB expected at <configDir>/../data/items.json or packaged next to it.
    auto dbPath = configDir / "items.json";
    if (!std::filesystem::exists(dbPath)) {
        // Second-chance: look next to the loader binary (dev builds).
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

    ImGui::SetNextWindowSize({720, 520}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("##cheat", &g_open,
                     ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::BeginTabBar("##tabs")) {
            if (ImGui::BeginTabItem("Inventory")) { DrawInventoryTab();  ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Add Item"))  { DrawAddItemTab();    ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Settings"))  { DrawSettingsTab();   ImGui::EndTabItem(); }
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
