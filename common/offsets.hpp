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

// ---- schema: C_BaseEntity ----
// m_nSubclassID selects which schema subclass drives model/anim lookup for
// the entity. Writing a new MurmurHash2(def-index, 0x31415926) here is the
// knife-swap mechanism; see IsKnifeDef / SubclassHash in backend_phase_a.
inline constexpr std::ptrdiff_t m_nSubclassID              = 0x380;   // CUtlStringToken

// ---- schema: C_EconEntity (every weapon derives from this) ----
inline constexpr std::ptrdiff_t m_AttributeManager         = 0x1290; // C_AttributeContainer (embedded)
inline constexpr std::ptrdiff_t m_hOwnerEntity             = 0x520;

// ---- schema: C_AttributeContainer ----
inline constexpr std::ptrdiff_t m_Item                     = 0x50;   // C_EconItemView (embedded)

// ---- schema: C_EconItemView ----
// Direct fields.
inline constexpr std::ptrdiff_t m_iItemDefinitionIndex     = 0x1BA;
inline constexpr std::ptrdiff_t m_iEntityQuality           = 0x1BC;
inline constexpr std::ptrdiff_t m_iItemID                  = 0x1C8;   // uint64, client-side item marker
inline constexpr std::ptrdiff_t m_iItemIDHigh              = 0x1D0;
inline constexpr std::ptrdiff_t m_iItemIDLow               = 0x1D4;
inline constexpr std::ptrdiff_t m_iAccountID               = 0x1D8;
inline constexpr std::ptrdiff_t m_bInitialized             = 0x1E8;
inline constexpr std::ptrdiff_t m_bDisallowSOC             = 0x1E9;   // stop server re-resolve
inline constexpr std::ptrdiff_t m_szCustomName             = 0x2F8;

// Client-side item marker (cs2-sdk.com). Setting this tells the game the
// item is NOT backed by the Steam inventory, so it uses our fallback fields
// instead of re-resolving against the Game Coordinator.
inline constexpr std::uint64_t kClientSideItemID = 0xF000000000000010ULL;

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

// Fallback fields DO exist on CS2 after all — cs2-sdk.com documents them
// on C_EconEntity. Values here come from the published offset table. The
// paint attributes still get written through SetAttribute (the HUD name
// and rarity derive from those, not from these fallback fields), but the
// renderer reads these on first apply before the attribute rebuild lands.
inline constexpr std::ptrdiff_t m_nFallbackPaintKit        = 0x18A8;
inline constexpr std::ptrdiff_t m_nFallbackSeed            = 0x18AC;
inline constexpr std::ptrdiff_t m_flFallbackWear           = 0x18B0;
inline constexpr std::ptrdiff_t m_nFallbackStatTrak        = 0x18B4;

// Per-weapon econ reload gate — reset to -1 before firing
// ApplyEconCustomization so the rebuild actually runs.
inline constexpr std::ptrdiff_t m_nCustomEconReloadEventId = 0x1B24;

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
// Charm (CS2 keychain) uses a different attribute surface. Left at 0;
// Phase-B guards on `if (offsets::kAttrDef_CharmId)` and sets the Charm
// Degraded bit (1<<10) when inventory carries pending charms, so this is
// visible in the Settings tab rather than silently no-oping.
//
// Searched (all empty for an *attribute* def-index pair):
//   * cs2-dumper client_dll.json: C_KeychainModule exposes struct offsets
//     (m_nKeychainDefID@0x1270, m_nKeychainSeed@0x1274), NOT attribute
//     def-indexes. InventoryNodeType_t enum has node-type ids, also not
//     attribute def-indexes.
//   * cs2-dumper offsets.json: no keychain / charm entries.
//   * GameTracking-CS2/game/csgo/pak01_dir/scripts/items/items_game.txt
//     scanned at offsets 0/300K/900K/1M/2M/2.5M/7M/7.5M: paint_kits,
//     items, sticker_kits, keychain-crate items (ids 5323-5328) all
//     present, but CS2 no longer publishes a top-level `attributes`
//     block with explicit def_index values — attribute definitions were
//     moved into the compiled client schema. items_game.txt is NOT the
//     source for CS2; do not look there again.
//
// Real resolution path (needs a runtime reverse, not a static fetch):
//   1. CEconItemSchema::GetAttributeDefinitionByName in client.dll,
//      called with names like "set item keychain slot 0 id" / "...pattern".
//      The returned def pointer carries the uint16 index.
//   2. Alt: walk the SchemaSystem RTTI registry at runtime and pull the
//      keychain attribute names → def-index map.
inline constexpr std::uint16_t kAttrDef_CharmId      = 0;   // TODO reverse via CEconItemSchema registry
inline constexpr std::uint16_t kAttrDef_CharmPattern = 0;   // TODO reverse via CEconItemSchema registry

// ---- gloves ----
// Dumper exposes C_CSPlayerPawn::m_EconGloves (embedded C_EconItemView).
inline constexpr std::ptrdiff_t m_EconGloves               = 0x1770;

// ---- knife swap: view-model + anim graph ----
inline constexpr std::ptrdiff_t m_hHudModelArms            = 0x1DA8; // C_CSPlayerPawn
inline constexpr std::ptrdiff_t AnimGraph_m_nAnimationAlgorithm = 0x18;
inline constexpr std::ptrdiff_t AnimGraph_m_sAnimGraph2Identifier = 0x440;

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
