// module/menu/tab_inventory.cpp — Inventory tab per §4.2.
#include "menu.hpp"
#include "../inventory/backend.hpp"

#include <imgui.h>
#include <cstdio>

namespace cheat::menu {

void DrawInventoryTab() {
    auto& S = Get();
    if (ImGui::Button("Apply Changes")) {
        S.dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert to Real")) {
        S.inventory.clear();
        cheat::inventory::RequestRevert();
    }
    // Count badge right-aligned in the toolbar row, textDim via TextDisabled.
    char badge[32];
    std::snprintf(badge, sizeof(badge), "%zu items", S.inventory.size());
    const float bw = ImGui::CalcTextSize(badge).x;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - bw);
    ImGui::TextDisabled("%s", badge);

    ImGui::Separator();

    if (ImGui::BeginTable("inv", 6,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("icon",   ImGuiTableColumnFlags_WidthFixed, 48);
        ImGui::TableSetupColumn("def",    ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("skin",   ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("wear",   ImGuiTableColumnFlags_WidthFixed, 55);
        ImGui::TableSetupColumn("stat/name",ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##act",  ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableHeadersRow();

        int removeIdx = -1;
        for (int i = 0; i < static_cast<int>(S.inventory.size()); ++i) {
            auto& it = S.inventory[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (S.preview) S.preview->DrawIcon(it, {44, 44});

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%u", it.definitionIndex);

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d", it.paintKit);

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.3f", it.wear);

            ImGui::TableSetColumnIndex(4);
            if (it.statTrak >= 0) ImGui::Text("ST[%d] %s", it.statTrak, it.customName.data());
            else                   ImGui::TextUnformatted(it.customName.data());

            ImGui::TableSetColumnIndex(5);
            ImGui::PushID(i);
            if (ImGui::SmallButton("edit")) {
                S.editing      = it;
                S.editingIdx   = i;        // commit updates in place
                S.editorOpen   = true;
                S.requestedTab = "Add Item";
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("remove")) removeIdx = i;
            ImGui::PopID();
        }
        if (removeIdx >= 0) {
            S.inventory.erase(S.inventory.begin() + removeIdx);
            // Restore the server-truth values on all live views so the
            // removed entry's attributes stop being overridden, then re-
            // apply what's left. Order matters: ClearAll first wipes our
            // override marker (m_iItemIDHigh=0 lets CS2 reload), then
            // dirty=true repaints the remaining inventory on next frame.
            cheat::inventory::RequestRevert();
            S.dirty = true;
            // If the row being removed was the one in the editor, clear
            // the edit cursor so the Add tab's commit goes back to append.
            if (S.editingIdx == removeIdx) {
                S.editingIdx = -1;
                S.editorOpen = false;
            } else if (S.editingIdx > removeIdx) {
                --S.editingIdx;
            }
        }
        ImGui::EndTable();
    }
}

} // namespace cheat::menu
