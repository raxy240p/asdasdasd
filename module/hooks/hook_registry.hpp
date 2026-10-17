// module/hooks/hook_registry.hpp — inline detour tracker.
// Every patch keeps original bytes + target + patched size. SuspendAll
// restores all; ResumeAll re-applies — byte-for-byte round-trippable.
// No VMT swaps.
#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace cheat::hooks {

struct Entry {
    std::string            name;
    void*                  target;         // function patched
    std::vector<std::uint8_t> original;    // saved bytes
    void*                  trampoline;     // optional, for restore-free call-through
    bool                   active;
};

// Register and ARM a detour. `patch` is the little-endian jmp/mov+jmp stub we
// write into `target`. The registry stashes the original bytes before writing.
// Trampoline construction is caller-side (keeps this module tiny — the Present
// detour is the only one in the default build).
bool Install(const std::string& name, void* target,
             const std::vector<std::uint8_t>& patch,
             void* trampoline);

void UninstallAll();

// §6.3 / §6.4 — opportunistic unhook windows.
void SuspendAll();
void ResumeAll();

// Debug: for the Settings tab "offsets status" panel.
std::vector<Entry> Snapshot();

} // namespace cheat::hooks
