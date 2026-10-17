#include "mem_hygiene.hpp"
#include "../../common/offsets.hpp"

#include <windows.h>
#include <atomic>
#include <cstring>

namespace cheat::opsec {

namespace {
std::atomic<unsigned> g_mask{ 0 };

// Address of our own module. Manual-mapped images are NOT in the LDR list,
// so `GetModuleHandleEx(FROM_ADDRESS, &SelfModule)` returns null and the
// erase silently no-ops. Walk the VM instead: VirtualQuery around any symbol
// in this module reports the backing region's AllocationBase, which IS the
// mapped image base regardless of LDR state.
HMODULE SelfModule() {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(&SelfModule), &mbi, sizeof(mbi)))
        return nullptr;
    return reinterpret_cast<HMODULE>(mbi.AllocationBase);
}
} // namespace

void EraseHeaders() {
    HMODULE me = SelfModule();
    if (!me) return;
    auto* p = reinterpret_cast<std::uint8_t*>(me);
    // Wipe the whole headers region (DOS + NT + sections). Any scanner that
    // walks our image for section characteristics or import directory finds
    // nothing.
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(p);
    DWORD hdrSz = 0x400; // safe upper bound before we even know the real size
    if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(p + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE)
            hdrSz = nt->OptionalHeader.SizeOfHeaders;
    }
    DWORD old;
    if (VirtualProtect(p, hdrSz, PAGE_READWRITE, &old)) {
        std::memset(p, 0, hdrSz);
        VirtualProtect(p, hdrSz, old, &old);
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
