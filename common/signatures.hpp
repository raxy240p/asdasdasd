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

// Client-local econ view refresh. Called after we overwrite the item view
// to force a same-frame rebuild instead of waiting on OnDataChanged. See
// cs2-sdk.com knife-changer flow. If the sig misses on a build, Phase-A
// falls back to the tick-driven refresh (slightly stale first frame).
inline constexpr const char* SIG_EconItemView_Update    = "40 53 48 83 EC ? 48 8B D9 48 8B 49 ? 48 85 C9 74 ? E8 ? ? ? ?";

// Local-player resolve helper (fallback if dwLocalPlayerController breaks).
inline constexpr const char* SIG_GetLocalPlayerCtrl     = "48 8B 05 ? ? ? ? 48 85 C0 74 ? 48 8B 40 ? 48 85 C0 74 ? C3";

// ---- knife model swap flow (cs2-sdk.com documented) ----
// All resolved at init; a miss on any of these downgrades the Knife path
// to "paint only" (model stays as the server-spawned weapon). Patterns
// come from the published cs2-sdk.com knife-changer page.
inline constexpr const char* SIG_ApplyEconCustomization =
    "48 89 5C 24 08 57 48 83 EC 20 8B FA 48 8B D9 E8 ? ? ? ? 48 8B CB E8 ? ? ? ? 48 85 C0";
inline constexpr const char* SIG_UpdateCompositeMaterial =
    "E8 ? ? ? ? 48 8D 8B ? ? ? ? 48 89 BC 24";
inline constexpr const char* SIG_UpdateCompositeMaterialSet =
    "40 55 53 41 57 48 8D AC 24 00 FE ? ?";
inline constexpr const char* SIG_SetMeshGroupMask =
    "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 8D 99 ? ? ? ? 48 8B 71";
inline constexpr const char* SIG_InvalidateDescription =
    "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8D B9";
inline constexpr const char* SIG_GetViewModel =
    "40 53 48 83 EC 20 48 8B D9 E8 ? ? ? ? 48 83 BB ? ? ? ? 00 74 ? 48 8B 03";
inline constexpr const char* SIG_GetModelPath =
    "48 89 5C 24 10 56 48 83 EC 20 48 8B 1D";
inline constexpr const char* SIG_ChangeModel =
    "40 53 48 83 EC ? 48 8B D9 4C 8B C2 48 8B 0D ? ? ? ? 48 8D 54 24";

// Attribute definition resolver — maps a schema attribute name ("set item
// keychain slot 0 id" etc.) to its runtime def-index. Lets Phase-A resolve
// charm/keychain def-indexes at boot instead of hard-coding them per build.
// RVA 0x1127C70 on the dumped build; sig is a direct prologue match.
inline constexpr const char* SIG_GetAttributeDefinitionByName =
    "48 89 5C 24 10 48 89 6C 24 18 57 41 56 41 57 48 83 EC 60 48 8D 05";

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
