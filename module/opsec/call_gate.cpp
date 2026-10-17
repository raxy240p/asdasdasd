// module/opsec/call_gate.cpp — RX cave finder + stub emitter.
//
// Scans client.dll's .text for runs of 0xCC/0x90 at least 12 bytes long,
// aligned past 16, and claims one per gate. Byte stub (12 bytes):
//   48 B8 <imm64 fn>   mov rax, imm64
//   FF E0              jmp rax
// Caller sees a function pointer; the first instruction executed is inside
// a signed, loaded, catalog-trusted module — ret-tracker (msg 164) stays
// silent for that outbound.
#include "call_gate.hpp"
#include "../../common/offsets.hpp"

#include <windows.h>
#include <mutex>
#include <vector>

namespace cheat::opsec {

namespace {

struct CavePool {
    std::vector<std::uint8_t*> free;
    std::vector<std::uint8_t*> used;      // stubs that need restore on unload
    std::size_t                capacity = 0;
};
CavePool  g_pool;
std::mutex g_mx;
bool       g_scanned = false;

constexpr std::size_t kStubSize = 12;

void ScanOnce() {
    if (g_scanned) return;
    g_scanned = true;

    auto h = reinterpret_cast<std::uint8_t*>(GetModuleHandleA(offsets::MODULE_CLIENT));
    if (!h) return;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(h);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS64*>(h + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);

    constexpr std::size_t kStub = 12;
    constexpr std::size_t kAlign = 16;

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        auto* p = h + sec[i].VirtualAddress;
        auto  n = sec[i].Misc.VirtualSize;
        for (std::size_t off = 0; off + kStub <= n; ++off) {
            if ((reinterpret_cast<std::uintptr_t>(p + off) & (kAlign - 1)) != 0) continue;
            bool ok = true;
            for (std::size_t k = 0; k < kStub; ++k) {
                std::uint8_t b = p[off + k];
                if (b != 0xCC && b != 0x90) { ok = false; break; }
            }
            if (ok) {
                g_pool.free.push_back(p + off);
                off += kStub; // don't straddle
            }
        }
    }
    g_pool.capacity = g_pool.free.size();
}

} // namespace

void* MakeGateToClient(void* realFn) {
    std::lock_guard<std::mutex> _(g_mx);
    ScanOnce();
    if (g_pool.free.empty() || !realFn) return nullptr;

    std::uint8_t* dst = g_pool.free.back();
    g_pool.free.pop_back();
    g_pool.used.push_back(dst);

    std::uint8_t stub[kStubSize] = {
        0x48, 0xB8, 0,0,0,0,0,0,0,0,
        0xFF, 0xE0,
    };
    std::memcpy(stub + 2, &realFn, 8);

    DWORD old;
    VirtualProtect(dst, kStubSize, PAGE_EXECUTE_READWRITE, &old);
    std::memcpy(dst, stub, kStubSize);
    VirtualProtect(dst, kStubSize, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, kStubSize);
    return dst;
}

CaveStats PoolStats() {
    std::lock_guard<std::mutex> _(g_mx);
    return { g_pool.capacity - g_pool.free.size(), g_pool.capacity };
}

void UnloadAllGates() {
    std::lock_guard<std::mutex> _(g_mx);
    // Restore each used stub back to int3 padding, so client.dll's .text
    // tail has no "mov rax, imm64; jmp rax" literals left for a scanner.
    std::uint8_t cc[kStubSize];
    std::memset(cc, 0xCC, kStubSize);
    for (auto* dst : g_pool.used) {
        DWORD old;
        VirtualProtect(dst, kStubSize, PAGE_EXECUTE_READWRITE, &old);
        std::memcpy(dst, cc, kStubSize);
        VirtualProtect(dst, kStubSize, old, &old);
        FlushInstructionCache(GetCurrentProcess(), dst, kStubSize);
    }
    g_pool.used.clear();
    g_pool.free.clear();
    g_pool.capacity = 0;
    g_scanned = false;
}

} // namespace cheat::opsec
