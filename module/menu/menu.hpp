#pragma once
#include <windows.h>  // VK_INSERT for toggleVk default
#include "../items/item_config.hpp"
#include "../items/item_db.hpp"
#include "../items/preview.hpp"
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace cheat::menu {

struct State {
    cheat::items::Db                               db;
    std::vector<cheat::items::ItemConfig>          inventory;
    std::atomic<bool>                              dirty{ false };
    std::unique_ptr<cheat::items::IItemPreview>    preview;

    // AddItem navigation
    std::string selCategory;
    std::string selWeapon;
    int         selPaintKit = -1;

    // Editor state (when a paint is picked)
    cheat::items::ItemConfig  editing{};
    bool                      editorOpen = false;
    // -1 means "committing adds a new entry"; >=0 means "committing updates
    // this inventory index". Set when the user clicks Edit on an existing
    // row so the commit button overwrites instead of duplicating.
    int                       editingIdx = -1;

    // Settings
    int         toggleVk = VK_INSERT;  // §4.2 Settings — rebindable
    bool        showOffsetsStatus = false;

    // Tab the UI should jump to on next frame (e.g. Inventory edit → Add Item).
    std::string requestedTab;

    std::filesystem::path configDir;
};

void Init(const std::filesystem::path& configDir);
void Shutdown();

void Tick();           // called from Present. Handles toggle + draws when open.
bool IsOpen();

// Persistence (triggered by Settings tab).
bool SaveConfig();
bool LoadConfig();

State& Get();

} // namespace cheat::menu
