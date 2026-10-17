// common/ipc.hpp — the ready-flag / heartbeat the injector reads via RPM.
// Lives at a known export in the module (g_ipc). The injector waits for
// state == IpcState::Ready with a watchdog on the heartbeat counter.
#pragma once

#include <cstdint>

namespace ipc {

enum class State : uint32_t {
    Boot        = 0,
    HookingDX   = 1,
    HookingWnd  = 2,
    LoadingDb   = 3,
    Ready       = 4,
    Degraded    = 5, // partial boot: offsets missing. menu still works.
    Fatal       = 6,
};

#pragma pack(push, 1)
struct Block {
    uint32_t magic;       // 'SK' | 'IPC' banner — see kMagic
    uint32_t version;     // ipc protocol version
    uint32_t state;       // State
    uint32_t heartbeat;   // monotonic, bumped once per Present
    uint32_t degraded_mask; // bitmap of missing-offset groups when state==Degraded
    uint32_t _pad[3];
};
#pragma pack(pop)

inline constexpr uint32_t kMagic   = 0x53'4B'49'43u; // "SKIC"
inline constexpr uint32_t kVersion = 1;

} // namespace ipc
