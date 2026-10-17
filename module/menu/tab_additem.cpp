// module/menu/tab_additem.cpp — category bar → weapon list → paint list → editor.
// Layout per §4.2 and B.3.
#include "menu.hpp"

#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace cheat::menu {

namespace {

const char* kCategoryOrder[] = {
    "Rifles", "SMGs", "Pistols", "Heavy", "Knives", "Gloves", "Agents",
    "Stickers", "Cases", "Keys", "Charms", "Music Kits",
    "Coins/Pins/Patches", "Graffiti", "Pets",
};

void DrawPaintEditor() {
    auto& S = Get();
    if (!S.editorOpen) return;

    ImGui::SetNextWindowSize({360, 480}, ImGuiCond_Once);
    if (!ImGui::Begin("Item Editor", &S.editorOpen)) { ImGui::End(); return; }
    auto& it = S.editing;

    ImGui::Text("def_index: %u  paint: %d", it.definitionIndex, it.paintKit);
    ImGui::Separator();

    ImGui::SliderFloat("wear",  &it.wear, 0.f, 1.f, "%.3f");
    ImGui::SliderInt  ("seed",  &it.seed, 0, 1024);

    bool st = it.statTrak >= 0;
    if (ImGui::Checkbox("StatTrak", &st)) it.statTrak = st ? 0 : -1;
    if (st) { ImGui::SameLine(); ImGui::SliderInt("##stv", &it.statTrak, 0, 100000); }
    ImGui::Checkbox("Souvenir", &it.souvenir);
    ImGui::InputText("nametag", it.customName.data(), it.customName.size());

    ImGui::Separator();
    ImGui::TextDisabled("Stickers");
    for (int i = 0; i < 5; ++i) {
        ImGui::PushID(i);
        auto& s = it.stickers[i];
        ImGui::Text("Slot %d", i);
        ImGui::SameLine(); ImGui::SetNextItemWidth(80);
        ImGui::InputInt   ("id",   &s.stickerId, 0, 0);
        ImGui::SameLine(); ImGui::SetNextItemWidth(60);
        ImGui::SliderFloat("wear", &s.wear, 0.f, 1.f);
        ImGui::SameLine(); ImGui::SetNextItemWidth(60);
        ImGui::SliderFloat("scl",  &s.scale, 0.1f, 5.f);
        ImGui::SameLine(); ImGui::SetNextItemWidth(60);
        ImGui::SliderFloat("rot",  &s.rotation, -180.f, 180.f);
        ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::TextDisabled("Charm (CS2 keychain)");
    ImGui::InputInt("charm id",  &it.charm.charmId, 0, 0);
    ImGui::InputInt("pattern",   &it.charm.pattern, 0, 0);

    ImGui::Separator();
    if (ImGui::Button("Add to Inventory", {160, 28})) {
        S.inventory.push_back(it);
        S.dirty = true;
        S.editorOpen = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", {120, 28})) S.editorOpen = false;

    ImGui::End();
}

} // namespace

void DrawAddItemTab() {
    auto& S = Get();

    // Category bar across the top.
    for (const char* cat : kCategoryOrder) {
        const bool have = S.db.categories.count(cat) != 0 || std::strcmp(cat, "Pets") == 0;
        if (!have) ImGui::BeginDisabled();
        if (ImGui::Selectable(cat, S.selCategory == cat, 0, ImVec2(90, 0))) {
            S.selCategory = cat;
            S.selWeapon.clear();
            S.selPaintKit = -1;
        }
        if (!have) ImGui::EndDisabled();
        ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::Separator();

    // Weapon list (left) + paints (right).
    const float splitW = ImGui::GetContentRegionAvail().x * 0.28f;
    ImGui::BeginChild("weapList", {splitW, 0}, true);
    {
        auto cit = S.db.categories.find(S.selCategory);
        if (cit == S.db.categories.end()) {
            ImGui::TextDisabled("pick a category");
        } else {
            for (auto& [name, w] : cit->second.weapons) {
                if (ImGui::Selectable(name.c_str(), S.selWeapon == name)) {
                    S.selWeapon   = name;
                    S.selPaintKit = -1;
                }
            }
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("paintList", {0, 0}, true);
    {
        auto cit = S.db.categories.find(S.selCategory);
        if (cit == S.db.categories.end()) { /* empty */ }
        else {
            auto wit = cit->second.weapons.find(S.selWeapon);
            if (wit == cit->second.weapons.end()) ImGui::TextDisabled("pick a weapon");
            else {
                auto& w = wit->second;
                for (const auto& p : w.paints) {
                    ImGui::PushID(p.kit);
                    const unsigned rc = cheat::items::RarityColor(p.rarity);
                    const ImVec2 rowStart = ImGui::GetCursorScreenPos();

                    // Rarity stripe (left-edge) via DrawList.
                    ImGui::GetWindowDrawList()->AddRectFilled(
                        rowStart, {rowStart.x + 4, rowStart.y + 54}, rc);
                    ImGui::Dummy({4, 54});
                    ImGui::SameLine();

                    cheat::items::ItemConfig stub{};
                    stub.definitionIndex = w.defIndex;
                    stub.paintKit        = p.kit;
                    stub.wear            = (p.finishMin + p.finishMax) * 0.5f;
                    if (S.preview) S.preview->DrawIcon(stub, {48, 48});
                    ImGui::SameLine();

                    ImGui::BeginGroup();
                    ImGui::Text("%s", p.name.c_str());
                    ImGui::TextDisabled("kit %d   %.2f-%.2f   %s",
                        p.kit, p.finishMin, p.finishMax, p.rarity.c_str());
                    ImGui::EndGroup();
                    ImGui::SameLine();

                    if (ImGui::Button("Select", {80, 48})) {
                        S.editing = {};
                        S.editing.definitionIndex = w.defIndex;
                        S.editing.paintKit        = p.kit;
                        S.editing.wear            = (p.finishMin + p.finishMax) * 0.5f;
                        S.editorOpen              = true;
                    }
                    ImGui::PopID();
                    ImGui::Separator();
                }
            }
        }
    }
    ImGui::EndChild();

    DrawPaintEditor();
}

} // namespace cheat::menu
