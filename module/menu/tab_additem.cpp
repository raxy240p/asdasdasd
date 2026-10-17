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
    // Horizontally-scrollable child so 15 categories at 90px each don't
    // clip or wrap awkwardly on narrow windows.
    ImGui::BeginChild("##catstrip", ImVec2(0, 36), false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (const char* cat : kCategoryOrder) {
        const bool have =
            S.db.categories.count(cat) != 0 ||
            (std::strcmp(cat, "Stickers") == 0 && !S.db.stickers.empty()) ||
            (std::strcmp(cat, "Charms")   == 0 && !S.db.charms.empty())   ||
            (std::strcmp(cat, "Agents")   == 0 && !S.db.agents.empty())   ||
            (std::strcmp(cat, "Cases")    == 0 && !S.db.cases.empty())    ||
            (std::strcmp(cat, "Keys")     == 0 && !S.db.keys.empty());
        const bool sel  = S.selCategory == cat;

        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (!have) ImGui::BeginDisabled();
        if (ImGui::Button(cat, {90, 24})) {
            S.selCategory = cat;
            S.selWeapon.clear();
            S.selPaintKit = -1;
        }
        if (!have) ImGui::EndDisabled();
        if (sel) ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    ImGui::EndChild();
    ImGui::Separator();
}

void DrawWeaponList(State& S, float width) {
    ImGui::BeginChild("weap", {width, 0}, true);

    // Non-weapon categories live in their own buckets, not db.categories.
    // Selecting a row commits an ItemConfig directly (no paint-kit step).
    auto commit = [&](const std::string& name, cheat::items::Category cat) {
        S.selWeapon = name;
        S.editing = {};
        S.editing.category = cat;
        for (auto& s : S.editing.stickers) s.scale = 1.f;
        S.editorOpen = true;
    };

    if (S.selCategory == "Stickers") {
        for (const auto& [n, s] : S.db.stickers)
            if (ImGui::Selectable(n.c_str(), S.selWeapon == n)) {
                commit(n, cheat::items::Category::Sticker);
                S.editing.stickers[0].stickerId = s.id;
            }
    } else if (S.selCategory == "Charms") {
        for (const auto& [n, c] : S.db.charms)
            if (ImGui::Selectable(n.c_str(), S.selWeapon == n)) {
                commit(n, cheat::items::Category::Charm);
                S.editing.charm.charmId = c.id;
            }
    } else if (S.selCategory == "Agents") {
        for (const auto& [n, a] : S.db.agents)
            if (ImGui::Selectable(n.c_str(), S.selWeapon == n)) {
                commit(n, cheat::items::Category::Agent);
                S.editing.definitionIndex = a.defIndex;
            }
    } else if (S.selCategory == "Cases") {
        for (const auto& [n, k] : S.db.cases)
            if (ImGui::Selectable(n.c_str(), S.selWeapon == n)) {
                commit(n, cheat::items::Category::Case);
                S.editing.definitionIndex = k.defIndex;
            }
    } else if (S.selCategory == "Keys") {
        for (const auto& [n, k] : S.db.keys)
            if (ImGui::Selectable(n.c_str(), S.selWeapon == n)) {
                commit(n, cheat::items::Category::Key);
                S.editing.definitionIndex = k.defIndex;
            }
    } else {
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

            ImGui::SameLine(ImGui::GetContentRegionMax().x - 70);
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

    const float side = (width > 48.0f) ? std::min(width - 24.0f, 260.0f) : 48.0f;
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
            // One slider per line so a narrow preview pane doesn't overflow.
            ImGui::SliderFloat("wear",  &s.wear,     0.f,    1.f);
            ImGui::SliderFloat("scl",   &s.scale,    0.1f,   5.f);
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
