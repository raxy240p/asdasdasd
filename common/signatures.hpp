// common/signatures.hpp — AOB patterns. '?' / "??" are nibble/byte wildcards.
//
// Sigs for the active cheat live at the top. VAC-internal sigs are reference-
// only intel for the optional §6.4 utilities (SK_VAC_RECON build flag).
#pragma once

namespace sigs {

// ---- active cheat ----
// Present / ResizeBuffers are resolved by vtable-of-dummy-swapchain at init,
// so no AOBs are shipped by default — kept here as a known fallback.
inline constexpr const char* PRESENT_FALLBACK           = "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 40 48 8B D9 41 8B F8";
inline constexpr const char* RESIZE_BUFFERS_FALLBACK    = "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 40 41 8B F0 44 8B C2";

// Client-local econ view refresh. Shape of the thunk is stable across builds;
// re-verify first bytes after each schema change in GameTracking-CS2.
inline constexpr const char* SIG_EconItemView_Update    = "40 53 48 83 EC ? 48 8B D9 48 8B 49 ? 48 85 C9 74 ? E8 ? ? ? ?"; // TODO verify

// Local-player resolve helper (fallback if dwLocalPlayerController breaks).
inline constexpr const char* SIG_GetLocalPlayerCtrl     = "48 8B 05 ? ? ? ? 48 85 C0 74 ? 48 8B 40 ? 48 85 C0 74 ? C3";

// ---- VAC intel (reference-only, SK_VAC_RECON) ----
// All from Appendix A.3 / A.5 of the design doc.
inline constexpr const char* VAC_MonitorFieldAccess             =
    "40 53 41 57 48 83 EC 38 48 89 74 24";
inline constexpr const char* VAC_RetTrackerRateLimitedSend      =
    "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 41 56 48 83 EC 20 41 8B F1 48 63 F9";
inline constexpr const char* VAC_ModuleRangeCacheCheck          =
    "48 89 5C 24 ? 48 89 74 24 ? 57 41 56 41 57 48 83 EC 20 48 8B F9";
inline constexpr const char* VAC_MonitorFieldRemove             = "?"; // derive near wrappers 0x133FBA0 / 0x133FBB0
inline constexpr const char* SteamService_CProviderCtor         =
    "55 8B EC 83 EC 3C 56 8B F1 C7 06 ? ? ? ?";

// Shared mapping name for the VacProcessMonitor provider (A.5).
inline constexpr const char* kVacMonitorMappingFmt =
    "Steam_{E9FD3C51-9B58-4DA0-962C-734882B19273}_Pid:%08X";
inline constexpr unsigned     kVacMonitorMagic     = 0x00030004;

} // namespace sigs
