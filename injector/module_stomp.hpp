// injector/module_stomp.hpp — §6.7 fallback mapper.
//
// Instead of VirtualAllocEx'ing a fresh private region (whose RX pages are
// "anonymous" from VAC's point of view), map the image into unused padding
// inside a legit, LDR-tracked, signed, loaded module. Outbound return
// addresses then fall inside a trusted module, immune to the module-range-
// cache check (Appendix A.3).
//
// Trade-off: the stomped module becomes part of our image's byte-level
// identity. Picking a module that is NOT part of VAC's PE-hash set keeps
// A.2 silent.
//
// Compiled only when the project is configured with -DSK_MODULE_STOMP=1.
#pragma once

#ifdef SK_MODULE_STOMP

#include "manual_map.hpp"
#include <string>

namespace injector::stomp {

struct Options {
    // Candidate modules, tried in order. First one whose RX padding
    // exceeds the DLL's SizeOfImage wins. Lower-signal picks come first.
    std::vector<std::string> candidates = {
        "rendersystemdx11.dll",
        "schemasystem.dll",
        "resourcesystem.dll",
        "scenesystem.dll",
        "steamaudio.dll",
    };
    DWORD readyTimeoutMs = 5000;
    std::uint32_t readyFlagRva = 0;
};

mapper::Result InjectStomp(DWORD pid,
                           const std::vector<std::uint8_t>& dllBytes,
                           const Options& opts,
                           std::function<void(const char*)> onLog);

} // namespace injector::stomp

#endif // SK_MODULE_STOMP
