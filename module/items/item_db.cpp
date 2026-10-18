#include "item_db.hpp"
#include "../../common/json.hpp"

#include <fstream>

namespace cheat::items {

LoadResult Load(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) return { false, "items.json not found: " + path.string(), {} };
    nlohmann::json j;
    try { f >> j; }
    catch (const std::exception& e) { return { false, e.what(), {} }; }

    Db db;
    db.schemaVersion = j.value("schema_version", 0);

    if (j.contains("categories")) {
        for (auto it = j["categories"].begin(); it != j["categories"].end(); ++it) {
            CategoryBucket bucket;
            const auto& catNode = it.value();
            for (auto w = catNode.begin(); w != catNode.end(); ++w) {
                Weapon weap;
                weap.name     = w.key();
                weap.defIndex = w.value().value("def_index", std::uint16_t{0});
                if (w.value().contains("paints")) {
                    for (const auto& p : w.value()["paints"]) {
                        Paint paint;
                        paint.name      = p.value("name", "");
                        paint.kit       = p.value("kit", 0);
                        paint.rarity    = p.value("rarity", "common");
                        paint.finishMin = p.value("finish_quality_min", 0.f);
                        paint.finishMax = p.value("finish_quality_max", p.value("max", 1.f));
                        weap.paints.push_back(std::move(paint));
                    }
                }
                bucket.weapons.emplace(w.key(), std::move(weap));
            }
            db.categories.emplace(it.key(), std::move(bucket));
        }
    }

    auto scoop = [&](const char* key, auto& bucket, auto parse) {
        if (!j.contains(key)) return;
        for (auto it = j[key].begin(); it != j[key].end(); ++it) {
            bucket.emplace(it.key(), parse(it.key(), it.value()));
        }
    };
    scoop("Stickers", db.stickers,
          [](const std::string& k, const nlohmann::json& v) {
              return Sticker{ k, v.value("sticker_id", 0),
                              v.value("rarity", std::string("common")) };
          });
    scoop("Charms", db.charms,
          [](const std::string& k, const nlohmann::json& v) {
              return Charm{ k, v.value("charm_id", 0) };
          });
    scoop("Agents", db.agents,
          [](const std::string& k, const nlohmann::json& v) {
              return Agent{ k, v.value("def_index", std::uint16_t{0}),
                            v.value("loadout_slot", 0) };
          });
    scoop("Cases", db.cases,
          [](const std::string& k, const nlohmann::json& v) {
              return Keylike{ k, v.value("def_index", std::uint16_t{0}) };
          });
    scoop("Keys", db.keys,
          [](const std::string& k, const nlohmann::json& v) {
              return Keylike{ k, v.value("def_index", std::uint16_t{0}) };
          });
    // Label-only cosmetics (music kits attach to profile, graffiti to a slot,
    // coins/pins/patches/pets have no per-weapon attribute path). Surfaced
    // in the DB so the UI can list them; backend ignores them.
    auto klike = [](const std::string& k, const nlohmann::json& v) {
        return Keylike{ k, v.value("def_index", std::uint16_t{0}) };
    };
    scoop("MusicKits",        db.musicKits,        klike);
    scoop("CoinsPinsPatches", db.coinsPinsPatches, klike);
    scoop("Graffiti",         db.graffiti,         klike);

    return { true, "ok", std::move(db) };
}

unsigned int RarityColor(const std::string& r) {
    // ImGui 0xAABBGGRR literal
    if (r == "contraband")        return 0xFF2E22E8; // red-orange
    if (r == "covert")            return 0xFF1D43EA; // red
    if (r == "classified")        return 0xFFB83DC2; // pink
    if (r == "restricted")        return 0xFFC73689; // purple
    if (r == "milspec" ||
        r == "mil-spec" ||
        r == "mil_spec")          return 0xFFCE6E4B; // blue
    if (r == "industrial")        return 0xFFC9ED5E; // light blue
    if (r == "common" ||
        r == "consumer")          return 0xFFB4B4B4; // grey
    if (r == "exceedingly_rare")  return 0xFF24E0FF; // yellow (knives/gloves)
    return 0xFFCCCCCC;
}

} // namespace cheat::items
