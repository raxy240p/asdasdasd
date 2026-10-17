// module/items/preview.hpp — §4.4.
// Phase-1 impl renders cached PNGs. Phase-2 drives the game's own weapon
// preview; interface is identical so the swap is a factory flip.
#pragma once
#include "item_config.hpp"
#include <imgui.h>
#include <filesystem>
#include <memory>

namespace cheat::items {

class IItemPreview {
public:
    virtual ~IItemPreview() = default;
    virtual void DrawIcon    (const ItemConfig& item, ImVec2 size) = 0;
    virtual void DrawPanorama(const ItemConfig& item, ImVec2 size) = 0;
};

// Static icon-cache implementation. Icons loaded on first use from
//   <iconDir>/<def_index>_<paintKit>.png
// Missing icons render as a labelled placeholder tile.
std::unique_ptr<IItemPreview> MakeIconPreview(const std::filesystem::path& iconDir);

} // namespace cheat::items
