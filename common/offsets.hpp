// common/offsets.hpp — single source of truth for CS2 offsets/schema fields.
//
// Values pulled from the public cs2-dumper feed anchored on
//   https://github.com/SteamTracking/GameTracking-CS2
// Refresh with: `python scripts/update_offsets.py`.
//
// 0x0 means "field doesn't exist on the current CS2 build" (a CSGO carryover
// that didn't survive the schema rewrite). The verifier reports those as a
// group so the Settings tab can show "fallback-field path unavailable".
#pragma once

#include <cstddef>
#include <cstdint>

namespace offsets {

// ---- module names (resolved at runtime via PEB walk) ----
inline constexpr const char* MODULE_CLIENT  = "client.dll";
inline constexpr const char* MODULE_ENGINE  = "engine2.dll";
inline constexpr const char* MODULE_SCHEMA  = "schemasystem.dll";
inline constexpr const char* MODULE_TIER0   = "tier0.dll";

// ---- client.dll absolute RVAs (cs2-dumper output/offsets.json) ----
inline constexpr std::ptrdiff_t dwEntityList               = 0x271A908;
inline constexpr std::ptrdiff_t dwLocalPlayerController    = 0x253CB78;
inline constexpr std::ptrdiff_t dwLocalPlayerPawn          = 0x2565718;
inline constexpr std::ptrdiff_t dwViewMatrix               = 0x256B020;
inline constexpr std::ptrdiff_t dwViewAngles               = 0x257BCC8;
inline constexpr std::ptrdiff_t dwGlobalVars               = 0x2230090;

// ---- schema: CCSPlayerController ----
inline constexpr std::ptrdiff_t m_hPlayerPawn              = 0x92C;
inline constexpr std::ptrdiff_t m_pInGameMoneyServices     = 0x0;    // not used by Phase-B
inline constexpr std::ptrdiff_t m_pInventoryServices       = 0x820;  // CCSPlayer_InventoryServices

// ---- schema: C_CSPlayerPawn ----
inline constexpr std::ptrdiff_t m_pWeaponServices          = 0x12F0; // CPlayer_WeaponServices*

// ---- schema: CPlayer_WeaponServices ----
inline constexpr std::ptrdiff_t m_hActiveWeapon            = 0x60;   // CHandle
inline constexpr std::ptrdiff_t m_hMyWeapons               = 0x48;   // CUtlVector<CHandle>

// ---- schema: CCSPlayer_InventoryServices ----
// Not currently exposed by cs2-dumper's schema pass. Phase-B uses the
// weapon-services path (m_hMyWeapons) which covers held and dropped weapons.
inline constexpr std::ptrdiff_t m_pInventory               = 0x0;
inline constexpr std::ptrdiff_t m_vecLoadoutItems          = 0x0;

// ---- schema: C_EconEntity (every weapon derives from this) ----
inline constexpr std::ptrdiff_t m_AttributeManager         = 0x1290; // C_AttributeContainer (embedded)
inline constexpr std::ptrdiff_t m_hOwnerEntity             = 0x520;

// ---- schema: C_AttributeContainer ----
inline constexpr std::ptrdiff_t m_Item                     = 0x50;   // C_EconItemView (embedded)

// ---- schema: C_EconItemView ----
// Direct fields.
inline constexpr std::ptrdiff_t m_iItemDefinitionIndex     = 0x1BA;
inline constexpr std::ptrdiff_t m_iEntityQuality           = 0x1BC;
inline constexpr std::ptrdiff_t m_iItemIDHigh              = 0x1D0;
inline constexpr std::ptrdiff_t m_iItemIDLow               = 0x1D4;
inline constexpr std::ptrdiff_t m_iAccountID               = 0x1D8;
inline constexpr std::ptrdiff_t m_szCustomName             = 0x2F8;
inline constexpr std::ptrdiff_t m_bInitialized             = 0x1E8;

// Attribute list — paint/wear/seed/stattrak go through CAttributeList, not
// through dead CSGO fallback fields. Pulled from cs2-dumper:
//   CAttributeList::m_Attributes = 0x8   (CNetworkUtlVectorBase<CEconItemAttribute>)
//   CEconItemAttribute::m_iAttributeDefinitionIndex = 0x30
//   CEconItemAttribute::m_flValue                   = 0x34
inline constexpr std::ptrdiff_t m_AttributeList              = 0x208; // on CEconItemView
inline constexpr std::ptrdiff_t m_NetworkedDynamicAttributes = 0x280; // on CEconItemView
inline constexpr std::ptrdiff_t AttrList_m_Attributes        = 0x08;  // on CAttributeList
inline constexpr std::ptrdiff_t Attr_m_iAttributeDefinitionIndex = 0x30; // on CEconItemAttribute
inline constexpr std::ptrdiff_t Attr_m_flValue                  = 0x34;
inline constexpr std::size_t    Attr_sizeof                     = 0x38; // layout stride

// CSGO fallback fields — kept for symmetry with Appendix B. All zero on CS2.
inline constexpr std::ptrdiff_t m_nFallbackPaintKit        = 0x0;
inline constexpr std::ptrdiff_t m_nFallbackSeed            = 0x0;
inline constexpr std::ptrdiff_t m_flFallbackWear           = 0x0;
inline constexpr std::ptrdiff_t m_nFallbackStatTrak        = 0x0;

// Attribute DEFINITION indexes (items_game.txt). CS2 kept CSGO's numbering
// for the common fields; refresh after a schema break by cross-checking
// scripts/items/items_game.txt in GameTracking-CS2.
inline constexpr std::uint16_t kAttrDef_PaintKit     = 6;   // "set item texture prefab"
inline constexpr std::uint16_t kAttrDef_Seed         = 7;   // "set item texture seed"
inline constexpr std::uint16_t kAttrDef_Wear         = 8;   // "set item texture wear"
inline constexpr std::uint16_t kAttrDef_StatTrak     = 80;  // "kill eater"
inline constexpr std::uint16_t kAttrDef_StatTrakType = 81;  // "kill eater score type"
inline constexpr std::uint16_t kAttrDef_CustomName   = 111; // "custom name attr"
// Sticker slot block (per-slot stride 4; slot 0 occupies 113..116).
inline constexpr std::uint16_t kAttrDef_StickerSlotBase = 113;

// Sticker slot count is fixed; attribute-driven path uses kAttrDef_StickerSlotBase.
inline constexpr std::size_t   kStickerSlotCount = 5;
// Per-slot sub-attribute offsets within the slot block.
enum : std::uint16_t { kStk_Id = 0, kStk_Wear = 1, kStk_Scale = 2, kStk_Rotation = 3 };
// Charm (CS2 keychain) uses a different attribute surface; needs reversing
// confirmation per build. Left as placeholder def-indexes.
inline constexpr std::uint16_t kAttrDef_CharmId      = 0;   // TODO verify
inline constexpr std::uint16_t kAttrDef_CharmPattern = 0;   // TODO verify

// ---- gloves ----
// Dumper exposes C_CSPlayerPawn::m_EconGloves (embedded C_EconItemView).
inline constexpr std::ptrdiff_t m_EconGloves               = 0x1770;

// ---- view refresh entry point ----
// Left at 0 intentionally. cs2-dumper's offsets.json exposes no
// fnCEconItemView_Update / fnEconItemViewUpdate / equivalent, and the
// client-local refresh thunk's RVA is build-specific. CS2's tick-driven
// C_EconEntity::OnDataChanged picks up our m_iItemIDHigh=-1 + attribute
// writes on the next frame without an explicit poke — this is the
// production refresh path. An explicit call would only save 0-1 frames
// of latency on the first apply. Resolve via SIG_EconItemView_Update
// (also marked TODO) if sub-frame latency ever matters.
inline constexpr std::ptrdiff_t fn_EconItemView_Update     = 0x0;

// Required-at-startup set — the verifier walks these.
struct Required {
    const char* name;
    std::ptrdiff_t value;
};
inline constexpr Required kRequired[] = {
    { "dwEntityList",            dwEntityList },
    { "dwLocalPlayerController", dwLocalPlayerController },
    { "m_hPlayerPawn",           m_hPlayerPawn },
    { "m_pWeaponServices",       m_pWeaponServices },
    { "m_hMyWeapons",            m_hMyWeapons },
    { "m_AttributeManager",      m_AttributeManager },
    { "m_Item",                  m_Item },
    { "m_iItemDefinitionIndex",  m_iItemDefinitionIndex },
    { "m_AttributeList",         m_AttributeList },
};

} // namespace offsets
