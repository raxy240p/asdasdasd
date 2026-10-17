// module/items/item_config.hpp — data model per §4.3.
#pragma once

#include "../../common/json.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace cheat::items {

enum class Category : int {
    Rifle, SMG, Pistol, Heavy, Knife, Glove, Agent,
    Sticker, Case, Key, Charm, MusicKit, Graffiti,
    Coin, Patch, Custom,
};

struct StickerSlot {
    int   stickerId = 0;
    float wear      = 0.f;
    float scale     = 1.f;
    float rotation  = 0.f;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(StickerSlot, stickerId, wear, scale, rotation);

struct CharmSlot {
    int   charmId = 0;
    int   pattern = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CharmSlot, charmId, pattern);

struct Gloves {
    int glovePaint = -1;
    int gloveWear  = -1;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Gloves, glovePaint, gloveWear);

struct ItemConfig {
    Category                    category         = Category::Custom;
    std::uint16_t               definitionIndex  = 0;
    int                         paintKit         = 0;
    float                       wear             = 0.f;
    int                         seed             = 0;
    int                         statTrak         = -1;
    bool                        souvenir         = false;
    std::array<char, 33>        customName       {};
    std::array<StickerSlot, 5>  stickers         {};
    CharmSlot                   charm            {};
    Gloves                      gloves           {};
};

inline void to_json(nlohmann::json& j, const ItemConfig& i) {
    j = {
        {"category",        static_cast<int>(i.category)},
        {"definitionIndex", i.definitionIndex},
        {"paintKit",        i.paintKit},
        {"wear",            i.wear},
        {"seed",            i.seed},
        {"statTrak",        i.statTrak},
        {"souvenir",        i.souvenir},
        {"customName",      std::string(i.customName.data())},
        {"stickers",        i.stickers},
        {"charm",           i.charm},
        {"gloves",          i.gloves},
    };
}
inline void from_json(const nlohmann::json& j, ItemConfig& i) {
    i.category        = static_cast<Category>(j.value("category", 15));
    i.definitionIndex = j.value("definitionIndex", std::uint16_t{0});
    i.paintKit        = j.value("paintKit", 0);
    i.wear            = j.value("wear", 0.f);
    i.seed            = j.value("seed", 0);
    i.statTrak        = j.value("statTrak", -1);
    i.souvenir        = j.value("souvenir", false);
    const auto n = j.value("customName", std::string{});
    i.customName = {};
    std::snprintf(i.customName.data(), i.customName.size(), "%s", n.c_str());
    if (j.contains("stickers")) i.stickers = j["stickers"].get<decltype(i.stickers)>();
    if (j.contains("charm"))    i.charm    = j["charm"].get<CharmSlot>();
    if (j.contains("gloves"))   i.gloves   = j["gloves"].get<Gloves>();
}

} // namespace cheat::items
