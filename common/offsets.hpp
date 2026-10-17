// common/offsets.hpp — single source of truth for CS2 offsets/schema fields.
//
// Values are refreshed by scripts/update_offsets.py from the public dumps at
//   https://github.com/SteamTracking/GameTracking-CS2
// (schema layouts) and the a2x-style cs2-dumper JSONs linked from there.
//
// 0x0 == unverified placeholder. Startup must assert on required ones and
// degrade (status banner in debug tab) rather than crash. See module/opsec
// for the runtime verifier.
#pragma once

#include <cstddef>
#include <cstdint>

namespace offsets {

// ---- module names (resolved at runtime via PEB walk) ----
inline constexpr const char* MODULE_CLIENT  = "client.dll";
inline constexpr const char* MODULE_ENGINE  = "engine2.dll";
inline constexpr const char* MODULE_SCHEMA  = "schemasystem.dll";
inline constexpr const char* MODULE_TIER0   = "tier0.dll";

// ---- client.dll absolute RVAs (offsets.json flavor) ----
inline constexpr std::ptrdiff_t dwEntityList               = 0x0; // TODO
inline constexpr std::ptrdiff_t dwLocalPlayerController    = 0x0; // TODO
inline constexpr std::ptrdiff_t dwLocalPlayerPawn          = 0x0; // TODO
inline constexpr std::ptrdiff_t dwViewMatrix               = 0x0; // TODO
inline constexpr std::ptrdiff_t dwViewAngles               = 0x0; // TODO
inline constexpr std::ptrdiff_t dwGlobalVars               = 0x0; // TODO

// ---- schema: CCSPlayerController ----
inline constexpr std::ptrdiff_t m_hPlayerPawn              = 0x0; // TODO
inline constexpr std::ptrdiff_t m_pInGameMoneyServices     = 0x0; // TODO
inline constexpr std::ptrdiff_t m_pInventoryServices       = 0x0; // CCSPlayer_InventoryServices TODO

// ---- schema: CCSPlayer_InventoryServices ----
inline constexpr std::ptrdiff_t m_pInventory               = 0x0; // CCSPlayerInventory* TODO
inline constexpr std::ptrdiff_t m_vecLoadoutItems          = 0x0; // TODO

// ---- schema: C_BasePlayerWeapon / C_EconEntity ----
inline constexpr std::ptrdiff_t m_AttributeManager         = 0x0; // C_AttributeContainer TODO
inline constexpr std::ptrdiff_t m_Item                     = 0x0; // CEconItemView within attr container TODO
inline constexpr std::ptrdiff_t m_hOwnerEntity             = 0x0; // TODO

// ---- schema: CEconItemView / C_AttributeContainer econ fields ----
// All offsets are from the start of CEconItemView unless noted.
inline constexpr std::ptrdiff_t m_iItemDefinitionIndex     = 0x0; // uint16_t TODO
inline constexpr std::ptrdiff_t m_iEntityQuality           = 0x0; // int TODO
inline constexpr std::ptrdiff_t m_iItemIDHigh              = 0x0; // uint32_t — -1 marks un-networked view
inline constexpr std::ptrdiff_t m_iItemIDLow               = 0x0; // uint32_t TODO
inline constexpr std::ptrdiff_t m_iAccountID               = 0x0; // uint32_t TODO
inline constexpr std::ptrdiff_t m_nFallbackPaintKit        = 0x0; // uint32_t TODO
inline constexpr std::ptrdiff_t m_nFallbackSeed            = 0x0; // uint32_t TODO
inline constexpr std::ptrdiff_t m_flFallbackWear           = 0x0; // float    TODO
inline constexpr std::ptrdiff_t m_nFallbackStatTrak        = 0x0; // uint32_t TODO
inline constexpr std::ptrdiff_t m_szCustomName             = 0x0; // char[33] TODO
inline constexpr std::ptrdiff_t m_bInitialized             = 0x0; // bool     TODO

// ---- stickers / charm (keychain) attachment layout ----
// CS2 renders stickers via NetworkedQuantizedFloat triples (wear, scale, rotation)
// plus a sticker id. Charm is a single slot. Confirm field names in the current
// schema dump before use.
inline constexpr std::ptrdiff_t m_stickerSlots             = 0x0; // base of 5-slot sticker array TODO
inline constexpr std::ptrdiff_t m_charmSlot                = 0x0; // single CS2 keychain slot    TODO
inline constexpr std::size_t    kStickerSlotStride         = 0x0; // bytes between slots         TODO
inline constexpr std::size_t    kStickerSlotCount          = 5;

// Per-slot sub-offsets (relative to each sticker slot).
inline constexpr std::ptrdiff_t stkr_id                    = 0x0; // uint32 TODO
inline constexpr std::ptrdiff_t stkr_wear                  = 0x0; // float  TODO
inline constexpr std::ptrdiff_t stkr_scale                 = 0x0; // float  TODO
inline constexpr std::ptrdiff_t stkr_rotation              = 0x0; // float  TODO

// ---- gloves (C_EconItemView on gloves slot) ----
inline constexpr std::ptrdiff_t m_vecMyWearables           = 0x0; // TODO

// ---- view refresh entry point (resolve via SIG_EconItemView_Update) ----
// This is the client-local "the view changed, re-materialize visuals" call.
// Keep as a function-pointer target — never baked into cheat code.
inline constexpr std::ptrdiff_t fn_EconItemView_Update     = 0x0; // TODO

// Required-at-startup set — the verifier walks these and marks the module
// "degraded" when any are zero.
struct Required {
    const char* name;
    std::ptrdiff_t value;
};
inline constexpr Required kRequired[] = {
    { "dwEntityList",             dwEntityList },
    { "dwLocalPlayerController",  dwLocalPlayerController },
    { "m_hPlayerPawn",            m_hPlayerPawn },
    { "m_AttributeManager",       m_AttributeManager },
    { "m_Item",                   m_Item },
    { "m_iItemDefinitionIndex",   m_iItemDefinitionIndex },
    { "m_nFallbackPaintKit",      m_nFallbackPaintKit },
    { "m_flFallbackWear",         m_flFallbackWear },
    { "m_nFallbackSeed",          m_nFallbackSeed },
    { "fn_EconItemView_Update",   fn_EconItemView_Update },
};

} // namespace offsets
