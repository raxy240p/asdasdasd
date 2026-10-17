#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace injector::mapper {

struct Options {
    bool eraseHeaders = true;  // §3.4 step 10 — zero MZ/e_lfanew after init
    DWORD readyTimeoutMs = 5000;
    std::uint32_t readyFlagRva = 0; // populated at build-time against the module
};

struct Result {
    bool        ok;
    std::string stage;      // which step hit the error
    std::string message;    // human-readable
    std::uintptr_t remoteBase = 0;
};

// Everything runs in-injector; the only remote operations are:
//   VirtualAllocEx / VirtualProtectEx / WriteProcessMemory / ReadProcessMemory
//   OpenThread + Suspend/Get/Set/Resume ThreadContext (thread hijack entry).
// No CreateRemoteThread, no LoadLibrary — see §3.4 / §6.6.
Result Inject(DWORD pid,
              const std::vector<std::uint8_t>& dllBytes,
              const Options& opts,
              std::function<void(const char*)> onLog);

} // namespace injector::mapper
