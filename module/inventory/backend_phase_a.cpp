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
//
// Chain:
//   chunk     = *(void**)(entity_system + 0x10 + 8*(idx>>9))
//   entity    = *(C_BaseEntity**)(chunk + 120*(idx & 0x1FF))
//   identity  = *(CEntityIdentity**)(entity + 0x10)       // CEntityInstance::m_pEntity
//   ehandle   = *(uint32_t*)(identity + 0x10)             // schema-opaque probe
//
// SCHEMA NOTE: cs2-dumper's client_dll.json does NOT name the ehandle field
// on CEntityIdentity. Dumped fields start at m_nameStringTableIndex@0x14,
// m_name@0x18, ... — the 0x00..0x14 window (vtable + gap) houses the handle
// per public cs2 reversing convention, but it's unverified by the dumper.
// So: hard-reject on idx-mismatch (always safe — slot was reused), and
// fail-OPEN on full-handle mismatch (the serial read may be bogus on a
// build where the field isn't at +0x10). Phase-B reads m_hMyWeapons live
// each OnFrame, so a stale-but-idx-matching ent is self-corrected shortly.
// TODO: upgrade to a true serial compare when cs2-dumper exposes the field
// or someone reverses the exact offset per CS2 build.
void* ResolveHandle(std::uint32_t handle) {
    if (handle == 0 || handle == 0xFFFFFFFFu) return nullptr;
    const int idx = static_cast<int>(handle & 0x7FFFu);
    if (idx <= 0 || idx >= 0x8000) return nullptr;
    auto* esSlot = reinterpret_cast<void**>(AbsFromClient(offsets::dwEntityList));
    if (!esSlot || !*esSlot) return nullptr;
    auto es = *esSlot;
    const int chunkIdx = idx >> 9;
    if (chunkIdx < 0 || chunkIdx >= 64) return nullptr;
    void* chunk = *reinterpret_cast<void**>(
        reinterpret_cast<std::uint8_t*>(es) + 0x10 + 8 * chunkIdx);
    if (!chunk) return nullptr;
    void* ent = *reinterpret_cast<void**>(
        reinterpret_cast<std::uint8_t*>(chunk) + 120 * (idx & 0x1FF));
    const auto p = reinterpret_cast<std::uintptr_t>(ent);
    if (p < 0x10000 || (p & 0x7)) return nullptr;

    // Serial match via CEntityInstance::m_pEntity -> CEntityIdentity.ehandle.
    void* identity = *reinterpret_cast<void**>(
        reinterpret_cast<std::uint8_t*>(ent) + 0x10);
    if (!identity ||
        reinterpret_cast<std::uintptr_t>(identity) < 0x10000) return nullptr;
    const std::uint32_t storedH =
        *reinterpret_cast<std::uint32_t*>(
            reinterpret_cast<std::uint8_t*>(identity) + 0x10);
    const int storedIdx = static_cast<int>(storedH & 0x7FFFu);
    if (storedIdx != idx) return nullptr;              // slot reused — hard reject
    if (storedH != handle) {
        // Idx matches but full handle doesn't — either stale serial OR
        // the ehandle offset we probed is wrong for this build. Fail
        // open: trust the idx match since Phase-B reads m_hMyWeapons
        // live each OnFrame (short caching window). Logged implicitly.
    }
    return ent;
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

// ---- SetOrAddAttributeValueByDefIndex (uint16 def-index, float value) ----
// Picked by the AOB in signatures.hpp; the trailing `movzx r,r16` is the
// def-index read. If the sig fails to resolve we fall back to walking the
// CNetworkUtlVectorBase directly and updating existing entries only.
using FnSetAttr = void(__fastcall*)(void* /*CAttributeList*/,
                                    std::uint16_t /*defIdx*/, float /*value*/);
FnSetAttr g_setAttr      = nullptr;
FnSetAttr g_setAttrGated = nullptr;

// Compact byte-pattern scanner. "48 89 5C 24 ? ..." style, '?' = wildcard byte.
// Walks only readable + executable sections of the module; discarded or
// uncommitted regions are skipped via VirtualQuery before touching them so
// a bad import directory / stripped debug section can't AV the scanner.
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
    const std::size_t n = bytes.size();
    if (n == 0) return nullptr;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS64*>(
                    reinterpret_cast<std::uint8_t*>(mod) + dos->e_lfanew);
    auto* base = reinterpret_cast<std::uint8_t*>(mod);

    constexpr DWORD kReadable =
        PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

    // Only scan executable sections of the module.
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD si = 0; si < nt->FileHeader.NumberOfSections; ++si) {
        if (!(sec[si].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        auto* p = base + sec[si].VirtualAddress;
        const std::size_t sz = sec[si].Misc.VirtualSize;
        for (std::size_t i = 0; i + n <= sz; ) {
            // Probe current page's readability.
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(p + i, &mbi, sizeof(mbi))) break;
            const bool readable = (mbi.State == MEM_COMMIT) &&
                                  (mbi.Protect & kReadable) &&
                                  !(mbi.Protect & PAGE_GUARD);
            if (!readable) {
                // Skip past this region.
                const auto skip = reinterpret_cast<std::uint8_t*>(mbi.BaseAddress)
                                + mbi.RegionSize - p;
                i = (skip > 0) ? static_cast<std::size_t>(skip) : (i + 0x1000);
                continue;
            }
            bool ok = true;
            for (std::size_t k = 0; k < n; ++k) {
                if (bytes[k] < 0) continue;
                if (p[i + k] != static_cast<std::uint8_t>(bytes[k])) { ok = false; break; }
            }
            if (ok) return p + i;
            ++i;
        }
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

// Sanity-check that a Vec* really looks like Source2 CUtlVector / the
// front-24-bytes of CNetworkUtlVectorBase. A future schema change that
// inserts anything before the vector would make our in-place walker read
// a garbage pointer and iterate `size` on it. Rather than casting blind,
// reject layouts whose fields don't pass basic plausibility.
bool VecLooksSane(const Vec* v) {
    if (!v) return false;
    const auto p = reinterpret_cast<std::uintptr_t>(v->memory);
    if (v->size < 0 || v->size > 65535)            return false;
    if (v->allocCount < v->size)                   return false;
    if (v->allocCount > (1 << 20))                 return false; // 1M caps
    if (v->size > 0) {
        if (p < 0x10000 || (p & 0x7))              return false;
        if (!v->memory)                            return false;
    }
    return true;
}

// Walk CAttributeList.m_Attributes directly; update existing entry if the
// def-index already lives there. Returns true on in-place update. Bails
// cleanly (returns false) on a schema break so the gated append fn can
// take over without the walker chewing garbage memory first.
bool UpdateExistingAttr(void* attrList, std::uint16_t defIdx, float value) {
    if (!attrList) return false;
    auto* v = At<Vec>(attrList, offsets::AttrList_m_Attributes);
    if (!VecLooksSane(v))               return false;
    if (!v->memory || v->size <= 0)     return false;
    auto* arr = reinterpret_cast<std::uint8_t*>(v->memory);
    for (int i = 0; i < v->size; ++i) {
        auto* ent = arr + i * offsets::Attr_sizeof;
        const std::uint16_t d =
            *reinterpret_cast<std::uint16_t*>(ent + offsets::Attr_m_iAttributeDefinitionIndex);
        if (d == defIdx) {
            *reinterpret_cast<float*>(ent + offsets::Attr_m_flValue) = value;
            return true;
        }
    }
    return false;
}

void SetAttr(void* attrList, std::uint16_t defIdx, float value) {
    if (!attrList) return;
    // In-place first — works on existing attributes and doesn't need the
    // resolved function pointer. This covers the common case.
    if (UpdateExistingAttr(attrList, defIdx, value)) return;
    // Append path needs the real fn (append grows the vector + fixes up
    // network state handler). VAC rule: gated pointer only, never raw.
    if (!g_setAttrGated) return;
    g_setAttrGated(attrList, defIdx, value);
}

float Bits(std::uint32_t u) {
    float f; std::memcpy(&f, &u, 4); return f;
}

// Push ItemConfig attributes into a CAttributeList through
// SetOrAddAttributeValueByName. Values that the attribute system expects
// as integer-coerced floats go through the memcpy helper.
void ApplyAttributes(void* view, const cheat::items::ItemConfig& it) {
    void* attrList = At<std::uint8_t>(view, offsets::m_AttributeList);

    // Paint / wear / seed / stattrak (CSGO legacy numbering carried over).
    SetAttr(attrList, offsets::kAttrDef_PaintKit,
            Bits(static_cast<std::uint32_t>(it.paintKit)));
    SetAttr(attrList, offsets::kAttrDef_Wear, it.wear);
    SetAttr(attrList, offsets::kAttrDef_Seed,
            Bits(static_cast<std::uint32_t>(it.seed)));
    if (it.statTrak >= 0) {
        SetAttr(attrList, offsets::kAttrDef_StatTrak,
                Bits(static_cast<std::uint32_t>(it.statTrak)));
        SetAttr(attrList, offsets::kAttrDef_StatTrakType, Bits(0));
    }

    // Sticker slot block: base + slot*4 + {id, wear, scale, rotation}.
    for (std::uint16_t s = 0; s < offsets::kStickerSlotCount; ++s) {
        const auto& sl = it.stickers[s];
        const std::uint16_t base = offsets::kAttrDef_StickerSlotBase + s * 4;
        SetAttr(attrList, base + offsets::kStk_Id,
                Bits(static_cast<std::uint32_t>(sl.stickerId)));
        SetAttr(attrList, base + offsets::kStk_Wear,     sl.wear);
        SetAttr(attrList, base + offsets::kStk_Scale,    sl.scale);
        SetAttr(attrList, base + offsets::kStk_Rotation, sl.rotation);
    }

    if (offsets::kAttrDef_CharmId) {
        SetAttr(attrList, offsets::kAttrDef_CharmId,
                Bits(static_cast<std::uint32_t>(it.charm.charmId)));
        SetAttr(attrList, offsets::kAttrDef_CharmPattern,
                Bits(static_cast<std::uint32_t>(it.charm.pattern)));
    }
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
    // The real refresh fn (CEconItemView::Update) is build-specific and not
    // resolved per-build here. CS2's tick-driven CEconEntity::OnDataChanged
    // picks up our m_iItemIDHigh=-1 + attribute writes on the next frame
    // without any explicit poke needed. Previously we toggled m_bInitialized
    // here, but that was cosmetic — the field doesn't drive re-materialize.
}

// Match live weapon's def to a user ItemConfig. First match by def_index wins.
// Inventory-cosmetic categories (music kits, coins/pins/patches, graffiti)
// have no per-weapon attribute injection path, so they never pick here even
// if a def-index collision would otherwise match.
const cheat::items::ItemConfig*
PickForDef(const std::vector<cheat::items::ItemConfig>& items, std::uint16_t def) {
    using C = cheat::items::Category;
    for (const auto& it : items) {
        if (it.category == C::MusicKit || it.category == C::Coin ||
            it.category == C::Patch    || it.category == C::Graffiti) continue;
        if (it.definitionIndex == def) return &it;
    }
    return nullptr;
}

class PhaseA final : public IInventoryBackend {
public:
    bool ApplyAll(const std::vector<cheat::items::ItemConfig>& items) override {
        ResolveAttrFn();
        // Append path (adding attributes not already on the item) needs the
        // real fn. In-place updates (overriding attributes already present
        // on an already-painted weapon) work without it. Only flip to
        // Degraded when neither will have any effect — not here.

        auto* controller = LocalController();     if (!controller) return false;
        auto* pawn       = LocalPawn(controller); if (!pawn)       return false;
        auto* svc        = WeaponServices(pawn);  if (!svc)        return false;

        Vec* myWeapons = At<Vec>(svc, offsets::m_hMyWeapons);
        // Same plausibility fence as UpdateExistingAttr. If a future schema
        // change moves the vector, we bail cleanly instead of iterating a
        // bogus handle array.
        if (!VecLooksSane(myWeapons))                       return false;
        if (!myWeapons->memory || myWeapons->size <= 0)     return false;

        auto* handles = static_cast<std::uint32_t*>(myWeapons->memory);
        bool anyApplied = false;
        for (int i = 0; i < myWeapons->size; ++i) {
            auto* weapon = ResolveHandle(handles[i]);
            if (!weapon) continue;
            auto* view = ItemViewFromWeapon(weapon);
            if (!view) continue;

            std::uint16_t def = *At<std::uint16_t>(view, offsets::m_iItemDefinitionIndex);
            const auto* cfg = PickForDef(items, def);
            if (!cfg) continue;
            ApplyToView(view, *cfg);
            anyApplied = true;
        }

        // Gloves: m_EconGloves is an embedded C_EconItemView on the pawn,
        // not a weapon handle. Walk the inventory for Glove entries and
        // slam them into the glove view. Last-match wins if multiple.
        if (offsets::m_EconGloves) {
            auto* gloveView = At<std::uint8_t>(pawn, offsets::m_EconGloves);
            for (const auto& it : items) {
                if (it.category != cheat::items::Category::Glove) continue;
                ApplyToView(gloveView, it);
                anyApplied = true;
            }
        }

        // Diagnostic bit 10: inventory carries charm attributes but the
        // charm def-indexes aren't reversed yet. Recompute each apply so
        // removing the charm clears the signal (previously stuck once set).
        constexpr std::uint32_t kCharmDegradedBit = (1u << 10);
        bool hasPendingCharm = false;
        if (!offsets::kAttrDef_CharmId) {
            for (const auto& it : items) {
                if (it.charm.charmId != 0 || it.charm.pattern != 0) {
                    hasPendingCharm = true; break;
                }
            }
        }
        if (hasPendingCharm) g_ipc.degraded_mask |=  kCharmDegradedBit;
        else                 g_ipc.degraded_mask &= ~kCharmDegradedBit;

        return anyApplied;
    }

    bool ClearAll() override {
        // Snap visuals back: strip our overrides from each live view.
        auto* controller = LocalController();     if (!controller) return false;
        auto* pawn       = LocalPawn(controller); if (!pawn)       return false;
        auto* svc        = WeaponServices(pawn);  if (!svc)        return false;

        Vec* myWeapons = At<Vec>(svc, offsets::m_hMyWeapons);
        if (!VecLooksSane(myWeapons))                       return false;
        if (!myWeapons->memory || myWeapons->size <= 0)     return false;

        auto* handles = static_cast<std::uint32_t*>(myWeapons->memory);
        for (int i = 0; i < myWeapons->size; ++i) {
            auto* weapon = ResolveHandle(handles[i]);
            if (!weapon) continue;
            auto* view = ItemViewFromWeapon(weapon);
            if (!view) continue;
            // ItemIDHigh = 0 lets the server-side truth re-flow on next tick.
            if (offsets::m_iItemIDHigh) *At<std::uint32_t>(view, offsets::m_iItemIDHigh) = 0;
        }
        // Snap gloves back too — same ItemIDHigh = 0 trick on the embedded view.
        if (offsets::m_EconGloves && offsets::m_iItemIDHigh) {
            auto* gloveView = At<std::uint8_t>(pawn, offsets::m_EconGloves);
            *At<std::uint32_t>(gloveView, offsets::m_iItemIDHigh) = 0;
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
    // Degraded gate: refuse to write when offsets.hpp isn't fully verified
    // or the attribute-set fn hasn't been resolved yet. Writing with
    // zero-valued offsets would land in random pointer-relative memory.
    if (g_ipc.state != static_cast<uint32_t>(ipc::State::Ready)) return;

    if (!g_backend) g_backend = MakeBackend();
    auto& S = cheat::menu::Get();

    // Peek dirty without clearing. We only clear after a successful apply so
    // "menu flip during apply" isn't lost — the next frame reconsiders. The
    // revert flag can clear eagerly; it's intentionally one-shot.
    if (g_revertRequested.exchange(false)) g_backend->ClearAll();

    if (S.dirty.load()) {
        if (g_backend->ApplyAll(S.inventory)) S.dirty.store(false);
    }
}

void RequestRevert() { g_revertRequested.store(true); }

} // namespace cheat::inventory
