#include "mem_hygiene.hpp"
#include "../../common/offsets.hpp"

#include <windows.h>
#include <atomic>
#include <cstring>

namespace cheat::opsec {

namespace {
std::atomic<unsigned> g_mask{ 0 };

// Address of our own module. Resolved via a cheap trick: use the address of
// this translation-unit's function and query the backing region.
HMODULE SelfModule() {
    HMODULE h = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&SelfModule), &h);
    return h;
}
} // namespace

void EraseHeaders() {
    HMODULE me = SelfModule();
    if (!me) return;
    auto* p = reinterpret_cast<std::uint8_t*>(me);
    DWORD old;
    if (VirtualProtect(p, 0x40, PAGE_READWRITE, &old)) {
        std::memset(p, 0, 0x40);
        VirtualProtect(p, 0x40, old, &old);
    }
}

bool OffsetsVerified() {
    unsigned mask = 0;
    for (std::size_t i = 0; i < std::size(offsets::kRequired); ++i) {
        if (offsets::kRequired[i].value == 0) mask |= (1u << (i & 31));
    }
    g_mask.store(mask);
    return mask == 0;
}

void     SetDegradedMask(unsigned m) { g_mask.store(m); }
unsigned DegradedMask()               { return g_mask.load(); }

} // namespace cheat::opsec
