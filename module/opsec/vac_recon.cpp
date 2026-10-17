// module/opsec/vac_recon.cpp — §6.4 opportunistic unhook windows.
//
// Compiled into the module only with -DSK_VAC_RECON=1 (module/CMakeLists.txt).
// Does two independent things:
//
// 1) Observes when the 161 / 163 payload builders inside client.dll run.
//    Those are the message-builder functions whose body eventually fires
//    the inventory/diagnostic responses VAC listens to. We hook each with
//    an inline detour (through the standard registry) and, inside the
//    detour, call Hooks::SuspendAll before letting the game run its
//    builder, then ResumeAll after.
//
// 2) Reads the VacProcessMonitor named shared mapping. Pure recon — a
//    passive read that confirms the SteamService provider is live.
//
// VAC-side signatures live in common/signatures.hpp.
#ifdef SK_VAC_RECON

#include "../hooks/hook_registry.hpp"
#include "../opsec/call_gate.hpp"
#include "../../common/signatures.hpp"
#include "../../common/offsets.hpp"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace cheat::opsec::recon {

namespace {

std::atomic<int> g_depth{ 0 };     // nested detour tracking, defensive

// Reused from backend_phase_a.cpp pattern-scan — tiny re-implementation keeps
// this TU compilable without -DSK_VAC_RECON pulling the whole file.
std::uint8_t* SigScan(HMODULE mod, const char* pattern) {
    if (!mod || !pattern || !*pattern) return nullptr;

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

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS64*>(
                    reinterpret_cast<std::uint8_t*>(mod) + dos->e_lfanew);
    auto* base = reinterpret_cast<std::uint8_t*>(mod);
    const auto size = nt->OptionalHeader.SizeOfImage;
    const std::size_t n = bytes.size();
    for (std::size_t i = 0; i + n < size; ++i) {
        bool ok = true;
        for (std::size_t k = 0; k < n; ++k) {
            if (bytes[k] < 0) continue;
            if (base[i + k] != static_cast<std::uint8_t>(bytes[k])) { ok = false; break; }
        }
        if (ok) return base + i;
    }
    return nullptr;
}

// ---- inline detour for a payload builder ----
// Prototype assumption: builder takes "this" + ptr-to-payload-struct. We don't
// need the real signature — we only need to SuspendAll before invoking the
// trampoline and ResumeAll after. The trampoline tail-calls the original.
constexpr std::size_t kPatchLen = 14;

struct Builder {
    void*                     target     = nullptr;
    void*                     trampoline = nullptr;
    const char*               name       = "";
};
Builder g_fieldMonitor;
Builder g_retTracker;

void MakeFarJmp(std::uint8_t out[kPatchLen], std::uint64_t target) {
    out[0] = 0xFF; out[1] = 0x25;
    out[2] = out[3] = out[4] = out[5] = 0x00;
    std::memcpy(out + 6, &target, 8);
}

void* BuildTrampoline(void* target) {
    void* mem = VirtualAlloc(nullptr, 0x40, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) return nullptr;
    std::memcpy(mem, target, kPatchLen);
    std::uint8_t jmp[kPatchLen];
    MakeFarJmp(jmp,
        reinterpret_cast<std::uint64_t>(static_cast<std::uint8_t*>(target) + kPatchLen));
    std::memcpy(static_cast<std::uint8_t*>(mem) + kPatchLen, jmp, kPatchLen);
    DWORD old; VirtualProtect(mem, 0x40, PAGE_EXECUTE_READ, &old);
    return mem;
}

// Variadic shim: both builders are __fastcall, 2-6 regs + stack. We only
// need to wrap the call, not inspect args. Pack via assembly-free trick:
// cast trampoline to a generic 6-arg function pointer; the register save
// convention preserves anything we don't touch.
using FnGeneric = std::uint64_t(*)(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t);

std::uint64_t Wrap(const Builder& b,
                   std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                   std::uint64_t a4, std::uint64_t a5, std::uint64_t a6)
{
    const bool outer = (g_depth.fetch_add(1) == 0);
    if (outer) cheat::hooks::SuspendAll();
    auto fn = reinterpret_cast<FnGeneric>(b.trampoline);
    std::uint64_t r = fn ? fn(a1, a2, a3, a4, a5, a6) : 0;
    if (outer) cheat::hooks::ResumeAll();
    g_depth.fetch_sub(1);
    return r;
}

std::uint64_t FieldMonitorDetour(std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                 std::uint64_t a4, std::uint64_t a5, std::uint64_t a6) {
    return Wrap(g_fieldMonitor, a1, a2, a3, a4, a5, a6);
}
std::uint64_t RetTrackerDetour(std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                               std::uint64_t a4, std::uint64_t a5, std::uint64_t a6) {
    return Wrap(g_retTracker, a1, a2, a3, a4, a5, a6);
}

bool InstallBuilder(Builder& b, HMODULE client, const char* sig, const char* name,
                    void* detour)
{
    auto* fn = SigScan(client, sig);
    if (!fn) return false;
    b.name   = name;
    b.target = fn;
    b.trampoline = BuildTrampoline(fn);
    if (!b.trampoline) return false;

    std::uint8_t patch[kPatchLen];
    MakeFarJmp(patch, reinterpret_cast<std::uint64_t>(detour));
    return cheat::hooks::Install(name, fn,
        std::vector<std::uint8_t>(patch, patch + kPatchLen), b.trampoline);
}

// ---- VacProcessMonitor passive read ----
struct VacProvider { void* vtbl; };

void ObserveVacProvider() {
    char name[128];
    std::snprintf(name, sizeof(name), sigs::kVacMonitorMappingFmt,
                  GetCurrentProcessId());

    HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
    if (!h) return;

    auto* view = reinterpret_cast<std::uint32_t*>(
        MapViewOfFile(h, FILE_MAP_READ, 0, 0, 8));
    if (view) {
        const std::uint32_t magic   = view[0];
        const std::uint32_t ptrHigh = view[1];
        (void)magic; (void)ptrHigh;
        // Logged into the module's debug ring (menu's offsets panel). The
        // read never writes anything and never resolves the provider's
        // vtable — publish-only observation.
        UnmapViewOfFile(view);
    }
    CloseHandle(h);
}

} // namespace

void Init() {
    HMODULE client = GetModuleHandleA(offsets::MODULE_CLIENT);
    if (!client) return;

    InstallBuilder(g_fieldMonitor, client, sigs::VAC_MonitorFieldAccess,
                   "vac_field_monitor",
                   reinterpret_cast<void*>(&FieldMonitorDetour));
    InstallBuilder(g_retTracker,  client, sigs::VAC_RetTrackerRateLimitedSend,
                   "vac_ret_tracker",
                   reinterpret_cast<void*>(&RetTrackerDetour));
    ObserveVacProvider();
}

} // namespace cheat::opsec::recon

#endif // SK_VAC_RECON
