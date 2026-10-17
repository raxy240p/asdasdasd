// module/inventory/backend.hpp — §4.5 contract.
#pragma once
#include "../items/item_config.hpp"
#include <memory>
#include <vector>

namespace cheat::inventory {

class IInventoryBackend {
public:
    virtual ~IInventoryBackend() = default;
    // GAME THREAD ONLY — called from Present hook.
    virtual bool ApplyAll(const std::vector<cheat::items::ItemConfig>& items) = 0;
    virtual bool ClearAll() = 0;
};

// Factory: phase A today, phase B stub flipped in by build flag.
std::unique_ptr<IInventoryBackend> MakeBackend();

// Called from Present hook. Observes menu dirty flag and routes to backend.
void OnFrame();
void RequestRevert();

} // namespace cheat::inventory
