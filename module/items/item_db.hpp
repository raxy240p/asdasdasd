#pragma once
#include "item_config.hpp"
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace cheat::items {

struct Paint {
    std::string name;
    int         kit            = 0;
    std::string rarity;        // "common" | "industrial" | ... | "covert" | "exceedingly_rare"
    float       finishMin      = 0.f;
    float       finishMax      = 1.f;
};

struct Weapon {
    std::string         name;
    std::uint16_t       defIndex = 0;
    std::vector<Paint>  paints;
};

struct CategoryBucket {
    // ordered: UI relies on insertion order from the JSON.
    std::map<std::string, Weapon> weapons;
};

struct Sticker {
    std::string   name;
    int           id = 0;
    std::string   rarity;
};
struct Charm   { std::string name; int id = 0; };
struct Agent   { std::string name; std::uint16_t defIndex = 0; int loadoutSlot = 0; };
struct Keylike { std::string name; std::uint16_t defIndex = 0; }; // Case / Key / etc.

struct Db {
    int schemaVersion = 0;
    std::map<std::string, CategoryBucket> categories; // "Rifles", "SMGs", ...
    std::map<std::string, Sticker>        stickers;
    std::map<std::string, Charm>          charms;
    std::map<std::string, Agent>          agents;
    std::map<std::string, Keylike>        cases;
    std::map<std::string, Keylike>        keys;
};

// Loads data/items.json. Returns an empty Db + sets !ok on failure.
struct LoadResult { bool ok; std::string message; Db db; };
LoadResult Load(const std::filesystem::path& path);

// Rarity -> ImGui color stripe. Matches CS2 web rarity convention.
unsigned int RarityColor(const std::string& r);

} // namespace cheat::items
