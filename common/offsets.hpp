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

// Attribute list — the CS2 way to override paint kit / wear / seed / stattrak.
// CSGO's m_nFallbackPaintKit etc. do not exist on this build; attributes
// are injected via CAttributeList::SetOrAddAttributeValueByName. Phase-B uses
// these two bases to write per-attribute entries.
inline constexpr std::ptrdiff_t m_AttributeList              = 0x208; // CAttributeList
inline constexpr std::ptrdiff_t m_NetworkedDynamicAttributes = 0x280; // CAttributeList

// CSGO fallback fields — kept for symmetry with Appendix B. All zero on CS2.
inline constexpr std::ptrdiff_t m_nFallbackPaintKit        = 0x0;
inline constexpr std::ptrdiff_t m_nFallbackSeed            = 0x0;
inline constexpr std::ptrdiff_t m_flFallbackWear           = 0x0;
inline constexpr std::ptrdiff_t m_nFallbackStatTrak        = 0x0;

// Attribute name hashes used by SetOrAddAttributeValueByName.
// Hashes are stable across CS2 builds (cheat-public knowledge).
inline constexpr std::uint32_t kAttr_PaintKit        = 0x79'F3'3F'F0u; // "set item texture prefab"
inline constexpr std::uint32_t kAttr_Wear            = 0xF6'62'5E'1Fu; // "set item texture wear"
inline constexpr std::uint32_t kAttr_Seed            = 0x4E'B2'51'4Du; // "set item texture seed"
inline constexpr std::uint32_t kAttr_StatTrak        = 0x2E'10'34'20u; // "kill eater"
inline constexpr std::uint32_t kAttr_StatTrakType    = 0xA1'B2'C9'B3u; // "kill eater score type"

// ---- stickers / charm (keychain) attachment layout ----
// Dumper doesn't expose sticker slot bases directly on EconItemView; they
// come through attributes "sticker slot %i id/wear/scale/rotation" too.
// Keeping these as 0 means Phase-B uses the attribute path for stickers.
inline constexpr std::ptrdiff_t m_stickerSlots             = 0x0;
inline constexpr std::ptrdiff_t m_charmSlot                = 0x0;
inline constexpr std::size_t    kStickerSlotStride         = 0x0;
inline constexpr std::size_t    kStickerSlotCount          = 5;
inline constexpr std::ptrdiff_t stkr_id                    = 0x0;
inline constexpr std::ptrdiff_t stkr_wear                  = 0x0;
inline constexpr std::ptrdiff_t stkr_scale                 = 0x0;
inline constexpr std::ptrdiff_t stkr_rotation              = 0x0;

// Sticker attribute name hashes.
inline constexpr std::uint32_t kAttr_StickerId[5] = {
    0xC5'C1'8D'BAu, 0xB8'B4'D9'E9u, 0xDB'A7'59'0Au, 0xE5'15'7D'A3u, 0x1C'90'2B'8Fu
};
inline constexpr std::uint32_t kAttr_StickerWear[5] = {
    0xBE'5B'5A'A3u, 0x8F'F5'51'8Du, 0xCF'5B'69'8Fu, 0x80'EA'8B'F6u, 0x27'5D'E4'77u
};
inline constexpr std::uint32_t kAttr_StickerScale[5] = {
    0x95'A5'F1'EBu, 0xD3'04'69'69u, 0x7F'46'1A'EFu, 0x55'1A'A6'C4u, 0xB9'C3'A2'A2u
};
inline constexpr std::uint32_t kAttr_StickerRotation[5] = {
    0xD4'70'FA'29u, 0x56'E6'51'0Au, 0x7E'65'B0'4Du, 0x2A'69'82'A4u, 0xB4'94'FA'36u
};
inline constexpr std::uint32_t kAttr_CharmId         = 0x6E'E0'C4'38u; // "keychain slot 0 id"
inline constexpr std::uint32_t kAttr_CharmPattern    = 0x7F'27'8D'0Au; // "keychain slot 0 pattern"

// ---- gloves ----
// Dumper exposes C_CSPlayerPawn::m_EconGloves (embedded C_EconItemView).
inline constexpr std::ptrdiff_t m_EconGloves               = 0x1770;

// ---- view refresh entry point (resolve via SIG_EconItemView_Update) ----
inline constexpr std::ptrdiff_t fn_EconItemView_Update     = 0x0; // TODO sig-scan at runtime

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
