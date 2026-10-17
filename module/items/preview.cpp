// module/items/preview.cpp — icon cache + static panorama placeholder.
#include "preview.hpp"
#include "../render/icon_cache.hpp"

#include <cstdio>

namespace cheat::items {

namespace {

class IconPreviewImpl final : public IItemPreview {
public:
    explicit IconPreviewImpl(const std::filesystem::path& dir)
        : cache_(dir) {}

    void DrawIcon(const ItemConfig& item, ImVec2 size) override {
        char key[64];
        std::snprintf(key, sizeof(key), "%u_%d", item.definitionIndex, item.paintKit);
        if (auto* tex = cache_.Get(key)) {
            ImGui::Image(tex, size);
        } else {
            // Placeholder tile.
            ImGui::PushStyleColor(ImGuiCol_ChildBg, 0xFF333333);
            ImGui::BeginChild(key, size, true, ImGuiWindowFlags_NoScrollbar);
            ImGui::TextDisabled("no icon\n%u/%d", item.definitionIndex, item.paintKit);
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
    }

    void DrawPanorama(const ItemConfig& item, ImVec2 size) override {
        // Phase 2 TODO: drive game's own weapon preview. Until then: static icon
        // scaled up, labelled.
        DrawIcon(item, size);
        ImGui::TextDisabled("(static preview — phase 2: live panorama)");
    }

private:
    cheat::render::IconCache cache_;
};

} // namespace

std::unique_ptr<IItemPreview> MakeIconPreview(const std::filesystem::path& iconDir) {
    return std::make_unique<IconPreviewImpl>(iconDir);
}

} // namespace cheat::items
