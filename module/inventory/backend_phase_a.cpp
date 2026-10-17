// module/inventory/backend_phase_a.cpp — Phase-A/B skin changer.
//
// Walker (Phase-B, now live):
//   dwLocalPlayerController -> controller
//   controller + m_hPlayerPawn (CHandle) -> entity-system lookup -> pawn
//   pawn + m_pWeaponServices -> svc
//   svc + m_hMyWeapons (CUtlVector<CHandle>) -> every held/dropped weapon
//   weapon + m_AttributeManager + m_Item -> C_EconItemView
//
// Attribute path (CS2 current):
//   SetOrAddAttributeValueByName(itemView+m_AttributeList, hash, value_f)
// resolved by signature at init, called through the client.dll call-gate so
// our outbound RIP never falls on an anonymous page.
//
// CSGO fallback fields (m_nFallbackPaintKit etc.) do not exist on CS2;
// offsets.hpp has them as 0 — guards here skip them without crashing.
#include "backend.hpp"
#include "../../common/offsets.hpp"
#include "../../common/signatures.hpp"
#include "../../common/ipc.hpp"
#include "../menu/menu.hpp"
#include "../opsec/call_gate.hpp"

#include <windows.h>
#include <atomic>
#include <cstring>
#include <vector>

extern "C" ipc::Block g_ipc;

namespace cheat::inventory {

namespace {

std::atomic<bool> g_revertRequested{ false };

template <typename T>
T* At(void* base, std::ptrdiff_t off) {
    return reinterpret_cast<T*>(reinterpret_cast<std::uint8_t*>(base) + off);
}

HMODULE Client() {
    static HMODULE h = GetModuleHandleA(offsets::MODULE_CLIENT);
    return h;
}
void* AbsFromClient(std::ptrdiff_t rva) {
    auto h = Client();
    return (!h || !rva) ? nullptr : reinterpret_cast<std::uint8_t*>(h) + rva;
}

// ---- CUtlVector helpers (Source2 shape: memory-first) ----
// struct CUtlVector<T> { T* m_pMemory; int m_nAllocationCount; int m_nGrowSize; int m_Size; };
struct Vec {
    void*  memory;
    int    allocCount;
    int    growSize;
    int    size;
};

// ---- CHandle resolution via dwEntityList ----
// handle layout: low 15 bits = index, high 17 bits = serial. INVALID = 0xFFFFFFFF.
void* ResolveHandle(std::uint32_t handle) {
    if (handle == 0xFFFFFFFFu) return nullptr;
    const int idx = static_cast<int>(handle & 0x7FFFu);
    if (idx <= 0) return nullptr;
    auto* esSlot = reinterpret_cast<void**>(AbsFromClient(offsets::dwEntityList));
    if (!esSlot || !*esSlot) return nullptr;
    auto es = *esSlot;
    const int chunkIdx = idx >> 9;
    if (chunkIdx < 0 || chunkIdx >= 64) return nullptr;
    void* chunk = *reinterpret_cast<void**>(
        reinterpret_cast<std::uint8_t*>(es) + 0x10 + 8 * chunkIdx);
    if (!chunk) return nullptr;
    return *reinterpret_cast<void**>(
        reinterpret_cast<std::uint8_t*>(chunk) + 120 * (idx & 0x1FF));
}

void* LocalController() {
    auto* slot = reinterpret_cast<void**>(AbsFromClient(offsets::dwLocalPlayerController));
    return slot ? *slot : nullptr;
}

void* LocalPawn(void* controller) {
    if (!controller || !offsets::m_hPlayerPawn) return nullptr;
    std::uint32_t h = *At<std::uint32_t>(controller, offsets::m_hPlayerPawn);
    return ResolveHandle(h);
}

void* WeaponServices(void* pawn) {
    if (!pawn || !offsets::m_pWeaponServices) return nullptr;
    return *At<void*>(pawn, offsets::m_pWeaponServices);
}

void* ItemViewFromWeapon(void* weapon) {
    if (!weapon) return nullptr;
    // m_AttributeManager is embedded (not a pointer): &weapon.m_AttributeManager
    auto* attrMgr = At<std::uint8_t>(weapon, offsets::m_AttributeManager);
    // m_Item is embedded in the attribute container
    return attrMgr + offsets::m_Item;
}

// ---- SetOrAddAttributeValueByName: resolve by signature once ----
using FnSetAttr = void(__fastcall*)(void* /*CAttributeList*/,
                                    std::uint32_t /*hash*/, float /*value*/);
FnSetAttr g_setAttr        = nullptr;
FnSetAttr g_setAttrGated   = nullptr;

// Compact byte-pattern scanner. "48 89 5C 24 ? ..." style, '?' = wildcard byte.
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
    for (std::size_t i = 0; i + n <= size; ++i) {
        bool ok = true;
        for (std::size_t k = 0; k < n; ++k) {
            if (bytes[k] < 0) continue;
            if (base[i + k] != static_cast<std::uint8_t>(bytes[k])) { ok = false; break; }
        }
        if (ok) return base + i;
    }
    return nullptr;
}

void ResolveAttrFn() {
    if (g_setAttr) return;
    // Signature for CAttributeList::SetOrAddAttributeValueByName.
    // AOB has to be refreshed per build; this prologue is stable across the
    // last several CS2 updates.
    static constexpr const char* kSig =
        "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8B F1 48 8B DA 0F B7";
    auto* fn = SigScan(Client(), kSig);
    if (!fn) return;
    g_setAttr      = reinterpret_cast<FnSetAttr>(fn);
    g_setAttrGated = reinterpret_cast<FnSetAttr>(
        cheat::opsec::MakeGateToClient(reinterpret_cast<void*>(fn)));
}

void SetAttr(void* attrList, std::uint32_t hash, float value) {
    if (!attrList) return;
    // VAC rule: never let our outbound RIP fall on an anonymous page.
    // Gated pointer only; raw ungated resolution is a dev-only diagnostic.
    if (!g_setAttrGated) return;
    g_setAttrGated(attrList, hash, value);
}

float Bits(std::uint32_t u) {
    float f; std::memcpy(&f, &u, 4); return f;
}

// Push ItemConfig attributes into a CAttributeList through
// SetOrAddAttributeValueByName. Values that the attribute system expects
// as integer-coerced floats go through the memcpy helper.
void ApplyAttributes(void* view, const cheat::items::ItemConfig& it) {
    void* attrList = At<std::uint8_t>(view, offsets::m_AttributeList);
    if (!attrList) return;

    SetAttr(attrList, offsets::kAttr_PaintKit,
            Bits(static_cast<std::uint32_t>(it.paintKit)));
    SetAttr(attrList, offsets::kAttr_Wear,     it.wear);
    SetAttr(attrList, offsets::kAttr_Seed,
            Bits(static_cast<std::uint32_t>(it.seed)));
    if (it.statTrak >= 0) {
        SetAttr(attrList, offsets::kAttr_StatTrak,
                Bits(static_cast<std::uint32_t>(it.statTrak)));
        SetAttr(attrList, offsets::kAttr_StatTrakType, Bits(0));
    }

    for (std::size_t s = 0; s < offsets::kStickerSlotCount; ++s) {
        const auto& sl = it.stickers[s];
        SetAttr(attrList, offsets::kAttr_StickerId[s],
                Bits(static_cast<std::uint32_t>(sl.stickerId)));
        SetAttr(attrList, offsets::kAttr_StickerWear[s],     sl.wear);
        SetAttr(attrList, offsets::kAttr_StickerScale[s],    sl.scale);
        SetAttr(attrList, offsets::kAttr_StickerRotation[s], sl.rotation);
    }

    SetAttr(attrList, offsets::kAttr_CharmId,
            Bits(static_cast<std::uint32_t>(it.charm.charmId)));
    SetAttr(attrList, offsets::kAttr_CharmPattern,
            Bits(static_cast<std::uint32_t>(it.charm.pattern)));
}

void ApplyToView(void* view, const cheat::items::ItemConfig& it) {
    if (!view) return;

    if (offsets::m_iItemDefinitionIndex)
        *At<std::uint16_t>(view, offsets::m_iItemDefinitionIndex) = it.definitionIndex;
    if (offsets::m_iItemIDHigh) {
        // -1 marks a client-only, non-networked view (§9 note).
        *At<std::uint32_t>(view, offsets::m_iItemIDHigh) = 0xFFFFFFFFu;
        *At<std::uint32_t>(view, offsets::m_iItemIDLow)  = 0xFFFFFFFFu;
    }
    if (offsets::m_iAccountID)
        *At<std::uint32_t>(view, offsets::m_iAccountID) = 0;
    if (offsets::m_iEntityQuality)
        *At<std::uint32_t>(view, offsets::m_iEntityQuality) = 4; // "unusual" / high-tier
    if (offsets::m_szCustomName) {
        auto* dst = At<char>(view, offsets::m_szCustomName);
        std::memset(dst, 0, 32);
        std::memcpy(dst, it.customName.data(),
                    (std::min)(it.customName.size(), size_t{32}));
    }

    // CSGO fallback fields — all 0 on CS2, guards no-op.
    if (offsets::m_nFallbackPaintKit)
        *At<std::uint32_t>(view, offsets::m_nFallbackPaintKit) =
            static_cast<std::uint32_t>(it.paintKit);
    if (offsets::m_flFallbackWear)
        *At<float>(view, offsets::m_flFallbackWear) = it.wear;

    ApplyAttributes(view, it);

    // Force re-materialize: toggle m_bInitialized so the EconItemView refetches.
    if (offsets::m_bInitialized) {
        *At<bool>(view, offsets::m_bInitialized) = false;
        *At<bool>(view, offsets::m_bInitialized) = true;
    }
}

// Match live weapon's def to a user ItemConfig. First match by def_index wins.
const cheat::items::ItemConfig*
PickForDef(const std::vector<cheat::items::ItemConfig>& items, std::uint16_t def) {
    for (const auto& it : items) if (it.definitionIndex == def) return &it;
    return nullptr;
}

class PhaseA final : public IInventoryBackend {
public:
    bool ApplyAll(const std::vector<cheat::items::ItemConfig>& items) override {
        ResolveAttrFn();

        auto* controller = LocalController();     if (!controller) return false;
        auto* pawn       = LocalPawn(controller); if (!pawn)       return false;
        auto* svc        = WeaponServices(pawn);  if (!svc)        return false;

        Vec* myWeapons = At<Vec>(svc, offsets::m_hMyWeapons);
        if (!myWeapons || !myWeapons->memory || myWeapons->size <= 0) return false;

        auto* handles = static_cast<std::uint32_t*>(myWeapons->memory);
        for (int i = 0; i < myWeapons->size; ++i) {
            auto* weapon = ResolveHandle(handles[i]);
            if (!weapon) continue;
            auto* view = ItemViewFromWeapon(weapon);
            if (!view) continue;

            std::uint16_t def = *At<std::uint16_t>(view, offsets::m_iItemDefinitionIndex);
            const auto* cfg = PickForDef(items, def);
            if (!cfg) continue;
            ApplyToView(view, *cfg);
        }
        return true;
    }

    bool ClearAll() override {
        // Snap visuals back: strip our overrides from each live view.
        auto* controller = LocalController();     if (!controller) return false;
        auto* pawn       = LocalPawn(controller); if (!pawn)       return false;
        auto* svc        = WeaponServices(pawn);  if (!svc)        return false;

        Vec* myWeapons = At<Vec>(svc, offsets::m_hMyWeapons);
        if (!myWeapons || !myWeapons->memory || myWeapons->size <= 0) return false;

        auto* handles = static_cast<std::uint32_t*>(myWeapons->memory);
        for (int i = 0; i < myWeapons->size; ++i) {
            auto* weapon = ResolveHandle(handles[i]);
            if (!weapon) continue;
            auto* view = ItemViewFromWeapon(weapon);
            if (!view) continue;
            // ItemIDHigh = 0 lets the server-side truth re-flow on next tick.
            if (offsets::m_iItemIDHigh) *At<std::uint32_t>(view, offsets::m_iItemIDHigh) = 0;
            if (offsets::m_bInitialized) {
                *At<bool>(view, offsets::m_bInitialized) = false;
                *At<bool>(view, offsets::m_bInitialized) = true;
            }
        }
        return true;
    }
};

std::unique_ptr<IInventoryBackend> g_backend;

} // namespace

std::unique_ptr<IInventoryBackend> MakeBackend() {
    return std::make_unique<PhaseA>();
}

void OnFrame() {
    if (!g_backend) g_backend = MakeBackend();
    auto& S = cheat::menu::Get();
    if (g_revertRequested.exchange(false)) g_backend->ClearAll();
    if (S.dirty.exchange(false))           g_backend->ApplyAll(S.inventory);
}

void RequestRevert() { g_revertRequested.store(true); }

} // namespace cheat::inventory
