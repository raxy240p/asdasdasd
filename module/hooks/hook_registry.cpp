#include "hook_registry.hpp"

#include <windows.h>
#include <cstring>

namespace cheat::hooks {

namespace {
std::mutex          g_mx;
std::vector<Entry>  g_entries;

bool WritePatch(void* dst, const std::uint8_t* src, size_t n) {
    DWORD old = 0;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(dst, src, n);
    DWORD tmp = 0;
    VirtualProtect(dst, n, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return true;
}
} // namespace

bool Install(const std::string& name, void* target,
             const std::vector<std::uint8_t>& patch, void* trampoline)
{
    std::lock_guard<std::mutex> _(g_mx);
    Entry e{ name, target, std::vector<std::uint8_t>(patch.size()), trampoline, true };
    std::memcpy(e.original.data(), target, patch.size());
    // If VirtualProtect fails here, the detour never gets installed — don't
    // record a bogus entry.
    if (!WritePatch(target, patch.data(), patch.size())) return false;
    g_entries.push_back(std::move(e));
    return true;
}

void UninstallAll() {
    std::lock_guard<std::mutex> _(g_mx);
    for (auto& e : g_entries) {
        if (e.active) WritePatch(e.target, e.original.data(), e.original.size());
        e.active = false;
    }
    g_entries.clear();
}

void SuspendAll() {
    std::lock_guard<std::mutex> _(g_mx);
    for (auto& e : g_entries) {
        if (!e.active) continue;
        std::vector<std::uint8_t> patched(e.original.size());
        std::memcpy(patched.data(), e.target, patched.size());
        WritePatch(e.target, e.original.data(), e.original.size());
        // Reuse `original` slot via swap — original now holds the patched bytes.
        // ponytail: single vector per entry; two allocs would just restate "two states".
        e.original.swap(patched);
        e.active = false;
    }
}

void ResumeAll() {
    std::lock_guard<std::mutex> _(g_mx);
    for (auto& e : g_entries) {
        if (e.active) continue;
        std::vector<std::uint8_t> real(e.original.size());
        std::memcpy(real.data(), e.target, real.size());
        WritePatch(e.target, e.original.data(), e.original.size());
        e.original.swap(real);
        e.active = true;
    }
}

std::vector<Entry> Snapshot() {
    std::lock_guard<std::mutex> _(g_mx);
    return g_entries;
}

} // namespace cheat::hooks
