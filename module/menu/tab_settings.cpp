// module/menu/tab_settings.cpp — Settings tab per §4.2.
#include "menu.hpp"
#include "../hooks/hook_registry.hpp"
#include "../opsec/mem_hygiene.hpp"
#include "../../common/offsets.hpp"

#include <imgui.h>
#include <windows.h>

namespace cheat::menu {

namespace {

const char* VkName(int vk) {
    static char buf[32];
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    GetKeyNameTextA(scan << 16, buf, sizeof(buf));
    if (!buf[0]) std::snprintf(buf, sizeof(buf), "VK_%02X", vk);
    return buf;
}

} // namespace

void DrawSettingsTab() {
    auto& S = Get();

    ImGui::Text("Menu key: %s", VkName(S.toggleVk));
    ImGui::SameLine();
    static bool rebinding = false;
    if (!rebinding) {
        if (ImGui::Button("Rebind", {80, 20})) rebinding = true;
    } else {
        ImGui::TextDisabled("press any key...");
        for (int vk = 1; vk < 256; ++vk) {
            if (vk == VK_LBUTTON || vk == VK_RBUTTON) continue;
            if (GetAsyncKeyState(vk) & 0x8000) {
                S.toggleVk = vk;
                rebinding = false;
                break;
            }
        }
    }

    ImGui::Separator();
    if (ImGui::Button("Save config")) SaveConfig();
    ImGui::SameLine();
    if (ImGui::Button("Reload config")) LoadConfig();

    ImGui::Separator();
    ImGui::Checkbox("Enable Knife MODEL swap (experimental)", &S.enableKnifeSwap);
    ImGui::TextDisabled("Off = paint only. On = full def+subclass+composite "
                        "rebuild. May freeze/crash on build mismatch.");

    ImGui::Separator();
    ImGui::Checkbox("Show offset/hook debug", &S.showOffsetsStatus);

    if (S.showOffsetsStatus) {
        ImGui::Separator();
        ImGui::TextDisabled("Offsets (0x0 = unverified)");
        for (const auto& r : offsets::kRequired) {
            const bool bad = (r.value == 0);
            ImGui::TextColored(bad ? ImVec4(1,0.4f,0.4f,1) : ImVec4(0.3f,1,0.4f,1),
                " %-30s 0x%llX", r.name,
                static_cast<unsigned long long>(r.value));
        }

        ImGui::Separator();
        ImGui::TextDisabled("Active hooks");
        for (const auto& h : cheat::hooks::Snapshot()) {
            ImGui::Text(" %-20s target=%p %s",
                h.name.c_str(), h.target, h.active ? "ARMED" : "suspended");
        }
    }
}

} // namespace cheat::menu
