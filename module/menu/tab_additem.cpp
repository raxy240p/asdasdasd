// module/menu/tab_additem.cpp — three-column Add Item view.
//   top row     : horizontal category selector
//   left (28%)  : weapon list in the selected category
//   middle (42%): paint-kit list, grouped by rarity header
//   right (30%) : panorama preview + inline editor + "Add to Inventory"
// Resolution-independent — all widths are fractions of the content area.
#include "menu.hpp"

#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace cheat::menu {

namespace {

const char* kCategoryOrder[] = {
    "Rifles", "SMGs", "Pistols", "Heavy", "Knives", "Gloves", "Agents",
    "Stickers", "Cases", "Keys", "Charms", "Music Kits",
    "Coins/Pins/Patches", "Graffiti", "Pets",
};

// Human-readable rarity header, ordered from most common to rarest.
struct RarityDef { const char* key; const char* label; int order; };
constexpr RarityDef kRarityOrder[] = {
    { "common",            "Consumer Grade",   0 },
    { "industrial",        "Industrial Grade", 1 },
    { "milspec",           "Mil-Spec",         2 },
    { "restricted",        "Restricted",       3 },
    { "classified",        "Classified",       4 },
    { "covert",            "Covert",           5 },
    { "exceedingly_rare",  "Rare Special",     6 },
    { "contraband",        "Contraband",       7 },
};
int RarityOrder(const std::string& r) {
    for (const auto& e : kRarityOrder) if (r == e.key) return e.order;
    return 100;
}
const char* RarityLabel(const std::string& r) {
    for (const auto& e : kRarityOrder) if (r == e.key) return e.label;
    return r.c_str();
}

void DrawCategoryStrip(State& S) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float minBtn = 90.0f;
    float x = 0.0f;
    for (const char* cat : kCategoryOrder) {
        const bool have = S.db.categories.count(cat) != 0 ||
                          std::strcmp(cat, "Pets") == 0;
        const bool sel  = S.selCategory == cat;

        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (!have) ImGui::BeginDisabled();
        if (ImGui::Button(cat, {minBtn, 24})) {
            S.selCategory = cat;
            S.selWeapon.clear();
            S.selPaintKit = -1;
        }
        if (!have) ImGui::EndDisabled();
        if (sel) ImGui::PopStyleColor();

        x += minBtn + ImGui::GetStyle().ItemSpacing.x;
        if (x + minBtn < avail) ImGui::SameLine();
        else                     { ImGui::NewLine(); x = 0.0f; }
    }
    if (x > 0.0f) ImGui::NewLine();
    ImGui::Separator();
}

void DrawWeaponList(State& S, float width) {
    ImGui::BeginChild("weap", {width, 0}, true);
    auto cit = S.db.categories.find(S.selCategory);
    if (cit == S.db.categories.end()) {
        ImGui::TextDisabled("pick a category above");
    } else {
        for (auto& [name, w] : cit->second.weapons) {
            if (ImGui::Selectable(name.c_str(), S.selWeapon == name)) {
                S.selWeapon = name;
                S.selPaintKit = -1;
            }
        }
    }
    ImGui::EndChild();
}

// Paint rows + rarity headers. Clicking a row loads the editor on the right.
void DrawPaintList(State& S, float width) {
    ImGui::BeginChild("paints", {width, 0}, true);

    auto cit = S.db.categories.find(S.selCategory);
    if (cit == S.db.categories.end()) { ImGui::EndChild(); return; }
    auto wit = cit->second.weapons.find(S.selWeapon);
    if (wit == cit->second.weapons.end()) {
        ImGui::TextDisabled("pick a weapon on the left");
        ImGui::EndChild();
        return;
    }
    auto& weap = wit->second;

    // Bucket paints by rarity for grouped headers.
    std::map<int, std::vector<const cheat::items::Paint*>> buckets;
    for (const auto& p : weap.paints) buckets[RarityOrder(p.rarity)].push_back(&p);

    for (auto& [ord, paints] : buckets) {
        if (paints.empty()) continue;
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImColor(cheat::items::RarityColor(paints.front()->rarity)).Value);
        ImGui::TextUnformatted(RarityLabel(paints.front()->rarity));
        ImGui::PopStyleColor();
        ImGui::Separator();

        for (const auto* p : paints) {
            ImGui::PushID(p->kit);
            const ImVec2 rowStart = ImGui::GetCursorScreenPos();
            const auto stripe = cheat::items::RarityColor(p->rarity);
            ImGui::GetWindowDrawList()->AddRectFilled(
                rowStart, {rowStart.x + 4, rowStart.y + 50}, stripe);
            ImGui::Dummy({4, 50}); ImGui::SameLine();

            cheat::items::ItemConfig stub{};
            stub.definitionIndex = weap.defIndex;
            stub.paintKit        = p->kit;
            stub.wear            = (p->finishMin + p->finishMax) * 0.5f;
            if (S.preview) S.preview->DrawIcon(stub, {44, 44});
            ImGui::SameLine();

            ImGui::BeginGroup();
            ImGui::TextUnformatted(p->name.c_str());
            ImGui::TextDisabled("kit %d · %.2f–%.2f", p->kit, p->finishMin, p->finishMax);
            ImGui::EndGroup();

            ImGui::SameLine(ImGui::GetContentRegionAvail().x > 70
                            ? ImGui::GetWindowWidth() - 70 : 0);
            if (ImGui::Button("Select", {60, 44})) {
                S.editing = {};
                S.editing.category        = cheat::items::Category::Custom;
                S.editing.definitionIndex = weap.defIndex;
                S.editing.paintKit        = p->kit;
                S.editing.wear            = (p->finishMin + p->finishMax) * 0.5f;
                for (auto& s : S.editing.stickers) s.scale = 1.f;
                S.editorOpen              = true;
                S.selPaintKit             = p->kit;
            }
            ImGui::PopID();
            ImGui::Separator();
        }
    }

    ImGui::EndChild();
}

// Right pane: panorama on top, inline editor beneath, "Add to Inventory" at bottom.
void DrawPreviewPane(State& S, float width) {
    ImGui::BeginChild("preview", {width, 0}, true);

    if (!S.editorOpen) {
        ImGui::TextDisabled("select a skin to preview + edit");
        ImGui::EndChild();
        return;
    }
    auto& it = S.editing;

    const float side = std::min(width - 24.0f, 260.0f);
    if (S.preview) S.preview->DrawPanorama(it, {side, side});
    ImGui::Separator();

    ImGui::Text("def %u · kit %d", it.definitionIndex, it.paintKit);
    ImGui::SliderFloat("wear",  &it.wear, 0.f, 1.f, "%.3f");
    ImGui::SliderInt  ("seed",  &it.seed, 0, 1024);

    bool st = it.statTrak >= 0;
    if (ImGui::Checkbox("StatTrak", &st)) it.statTrak = st ? 0 : -1;
    if (st) { ImGui::SameLine(); ImGui::SetNextItemWidth(80);
              ImGui::DragInt("##stv", &it.statTrak, 1, 0, 1'000'000); }
    ImGui::Checkbox("Souvenir", &it.souvenir);
    ImGui::InputText("nametag", it.customName.data(), it.customName.size());

    if (ImGui::CollapsingHeader("Stickers")) {
        for (int i = 0; i < 5; ++i) {
            ImGui::PushID(i);
            auto& s = it.stickers[i];
            ImGui::Text("Slot %d", i);
            ImGui::SetNextItemWidth(80); ImGui::SameLine();
            ImGui::InputInt("id", &s.stickerId, 0, 0);
            ImGui::SetNextItemWidth(60);
            ImGui::SliderFloat("wear",  &s.wear, 0.f, 1.f);
            ImGui::SetNextItemWidth(60); ImGui::SameLine();
            ImGui::SliderFloat("scl",   &s.scale, 0.1f, 5.f);
            ImGui::SetNextItemWidth(60); ImGui::SameLine();
            ImGui::SliderFloat("rot",   &s.rotation, -180.f, 180.f);
            ImGui::PopID();
        }
    }
    if (ImGui::CollapsingHeader("Charm")) {
        ImGui::InputInt("charm id", &it.charm.charmId, 0, 0);
        ImGui::InputInt("pattern",  &it.charm.pattern, 0, 0);
    }

    ImGui::Spacing();
    const float bw = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button("Add to Inventory", {bw, 32})) {
        S.inventory.push_back(it);
        S.dirty = true;
        S.editorOpen = false;
    }

    ImGui::EndChild();
}

} // namespace

void DrawAddItemTab() {
    auto& S = Get();

    DrawCategoryStrip(S);

    const float avail = ImGui::GetContentRegionAvail().x;
    const float wLeft   = avail * 0.28f;
    const float wMiddle = avail * 0.42f;
    const float wRight  = avail - wLeft - wMiddle
                          - 2 * ImGui::GetStyle().ItemSpacing.x;

    DrawWeaponList(S,  wLeft);   ImGui::SameLine();
    DrawPaintList (S,  wMiddle); ImGui::SameLine();
    DrawPreviewPane(S, wRight);
}

} // namespace cheat::menu
