// module/hooks/framestage_hook.cpp — inline detour on CHLClient::FrameStageNotify.
//
// This function's prologue is KNOWN (cs2-sdk.com sig
//   48 89 5C 24 ? 48 89 6C 24 ? 57 48 83 EC ? 48 8B F9 33 ED
// ) so the first 5 bytes always form a complete `mov [rsp+??], rbx` — the
// stolen-byte path is safe here even though it bit us for DXGI Present.
// We validate the known 4-byte head before copying and bail cleanly on a
// mismatch (sig stale after a CS2 update).
#include "framestage_hook.hpp"
#include "hook_registry.hpp"
#include "../../common/offsets.hpp"
#include "../inventory/backend.hpp"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace cheat::hooks {

namespace {

// FrameStageNotify sig (cs2-sdk.com, RVA 0xB6D390 on the dumped build).
constexpr const char* kFrameStageSig =
    "48 89 5C 24 ? 48 89 6C 24 ? 57 48 83 EC ? 48 8B F9 33 ED";

// ClientFrameStage_t stages (Source2). "After the network update" = 4.
constexpr int kFrameNetUpdateEnd = 4;

using FrameStageFn = void(__fastcall*)(void* /*this*/, int /*stage*/);
FrameStageFn       g_orig = nullptr;
std::atomic<bool>  g_inFlight{ false };

// Minimal sig scanner — scoped to this TU, mirrors the one in
// backend_phase_a.cpp but reads from a target HMODULE (client.dll).
std::uint8_t* Scan(HMODULE mod, const char* pattern) {
    if (!mod || !pattern) return nullptr;
    std::vector<int> bytes;
    for (const char* p = pattern; *p; ) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (*p == '?') { bytes.push_back(-1); ++p; if (*p == '?') ++p; continue; }
        auto hex = [](char c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = hex(*p++); if (hi < 0) continue;
        int lo = hex(*p++); if (lo < 0) continue;
        bytes.push_back((hi << 4) | lo);
    }
    const std::size_t n = bytes.size();
    if (n == 0) return nullptr;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS64*>(
                    reinterpret_cast<std::uint8_t*>(mod) + dos->e_lfanew);
    auto* base = reinterpret_cast<std::uint8_t*>(mod);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    constexpr DWORD kReadX =
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

    for (WORD si = 0; si < nt->FileHeader.NumberOfSections; ++si) {
        if (!(sec[si].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        auto* p = base + sec[si].VirtualAddress;
        const std::size_t sz = sec[si].Misc.VirtualSize;
        for (std::size_t i = 0; i + n <= sz; ) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(p + i, &mbi, sizeof(mbi))) break;
            const bool ok_prot = (mbi.State == MEM_COMMIT) && (mbi.Protect & kReadX) &&
                                 !(mbi.Protect & PAGE_GUARD);
            if (!ok_prot) {
                const auto skip = reinterpret_cast<std::uint8_t*>(mbi.BaseAddress) +
                                  mbi.RegionSize - p;
                i = (skip > 0) ? static_cast<std::size_t>(skip) : (i + 0x1000);
                continue;
            }
            bool match = true;
            for (std::size_t k = 0; k < n; ++k) {
                if (bytes[k] < 0) continue;
                if (p[i + k] != static_cast<std::uint8_t>(bytes[k])) { match = false; break; }
            }
            if (match) return p + i;
            ++i;
        }
    }
    return nullptr;
}

// 5-byte E9 rel32 to our detour; trampoline is 5 stolen bytes + 14-byte
// absolute jmp back. Prologue head MUST be `48 89 5C 24` (one complete
// 5-byte `mov [rsp+??], rbx`) — enforced by the sig head match.
constexpr std::size_t kPatchLen  = 5;
constexpr std::size_t kFarJmpLen = 14;

void MakeRel32(std::uint8_t out[kPatchLen], void* from, void* to) {
    const std::intptr_t rel =
        reinterpret_cast<std::uint8_t*>(to) -
        (reinterpret_cast<std::uint8_t*>(from) + kPatchLen);
    out[0] = 0xE9;
    std::memcpy(out + 1, &rel, 4);
}

void MakeFarJmp(std::uint8_t out[kFarJmpLen], std::uint64_t target) {
    out[0] = 0xFF; out[1] = 0x25;
    out[2] = out[3] = out[4] = out[5] = 0x00;
    std::memcpy(out + 6, &target, 8);
}

void* BuildTrampoline(void* target) {
    void* mem = VirtualAlloc(nullptr, 0x40, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) return nullptr;
    std::memcpy(mem, target, kPatchLen);
    std::uint8_t jmp[kFarJmpLen];
    MakeFarJmp(jmp,
        reinterpret_cast<std::uint64_t>(static_cast<std::uint8_t*>(target) + kPatchLen));
    std::memcpy(static_cast<std::uint8_t*>(mem) + kPatchLen, jmp, kFarJmpLen);
    DWORD old;
    VirtualProtect(mem, 0x40, PAGE_EXECUTE_READ, &old);
    return mem;
}

void __fastcall FrameStageDetour(void* self, int stage) {
    g_inFlight.store(true);
    if (stage == kFrameNetUpdateEnd) {
        // Writes to item views land here, before the game begins rendering
        // this frame — no race with the game thread's render-side reads.
        cheat::inventory::OnFrame();
    }
    g_inFlight.store(false);
    if (g_orig) g_orig(self, stage);
}

} // namespace

bool InstallFrameStageHook() {
    HMODULE client = GetModuleHandleA(offsets::MODULE_CLIENT);
    if (!client) return false;
    auto* target = Scan(client, kFrameStageSig);
    if (!target) return false;

    // Confirm the stolen head is `48 89 5C 24 ??` — one complete 5-byte mov.
    if (target[0] != 0x48 || target[1] != 0x89 ||
        target[2] != 0x5C || target[3] != 0x24) {
        return false;
    }

    // rel32 range check — detour must sit within ±2GB of target.
    const auto delta = reinterpret_cast<std::intptr_t>(&FrameStageDetour) -
                       reinterpret_cast<std::intptr_t>(target);
    if (delta < -0x80000000LL || delta >= 0x80000000LL) return false;

    void* tramp = BuildTrampoline(target);
    if (!tramp) return false;
    g_orig = reinterpret_cast<FrameStageFn>(tramp);

    std::uint8_t patch[kPatchLen];
    MakeRel32(patch, target, reinterpret_cast<void*>(&FrameStageDetour));

    if (!Install("FrameStageNotify", target,
                 std::vector<std::uint8_t>(patch, patch + kPatchLen),
                 tramp)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        g_orig = nullptr;
        return false;
    }
    return true;
}

void UninstallFrameStageHook() {
    // hook_registry::UninstallAll in Teardown handles the restore. Keep
    // g_orig set so a thread already past the entry can still tail-call.
}

bool FrameStageInFlight() { return g_inFlight.load(); }

} // namespace cheat::hooks
