#pragma once
#include "../items/item_config.hpp"
#include "../items/item_db.hpp"
#include "../items/preview.hpp"
#include <atomic>
#include <filesystem>
#include <memory>
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

    // Settings
    int         toggleVk = VK_INSERT;  // §4.2 Settings — rebindable
    bool        showOffsetsStatus = false;

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
