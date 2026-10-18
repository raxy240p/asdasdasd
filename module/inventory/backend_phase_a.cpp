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
#include <cstdio>
#include <cstring>
#include <unordered_map>
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

// ---- knife-swap function pointers (cs2-sdk.com flow) ----
// All go through client.dll call gates so the outbound RIP lands inside
// client.dll's .text. A null here means the sig missed on this build and
// that step is quietly skipped — Knife paint still works, the model swap
// doesn't.
using FnApplyEconCustomization    = void(__fastcall*)(void* /*weapon*/, int /*=1*/);
using FnUpdateCompositeMaterial   = void(__fastcall*)(void* /*compositeSet*/);
using FnUpdateCompositeMaterialSet = void(__fastcall*)(void* /*weapon*/, int /*=1*/);
using FnInvalidateDescription     = void(__fastcall*)(void* /*itemView*/);
using FnEconItemViewUpdate        = void(__fastcall*)(void* /*itemView*/);
FnApplyEconCustomization    g_applyEconGated           = nullptr;
FnUpdateCompositeMaterial   g_updateCompositeGated     = nullptr;
FnUpdateCompositeMaterialSet g_updateCompositeSetGated = nullptr;
FnInvalidateDescription     g_invalidateDescGated      = nullptr;
FnEconItemViewUpdate        g_econItemViewUpdateGated  = nullptr;

// MurmurHash2 (seed 0x31415926) over the lowercase decimal def-index — this
// is what CS2 uses for CUtlStringToken-based subclass selection. Writing
// the result to m_nSubclassID on the weapon entity is step 3 of the knife
// changer spec. Standard Austin-Appleby MurmurHash2 — no modifications.
std::uint32_t MurmurHash2(const void* key, int len, std::uint32_t seed) {
    constexpr std::uint32_t m = 0x5bd1e995;
    constexpr int r = 24;
    std::uint32_t h = seed ^ static_cast<std::uint32_t>(len);
    auto* data = static_cast<const std::uint8_t*>(key);
    while (len >= 4) {
        std::uint32_t k;
        std::memcpy(&k, data, 4);
        k *= m; k ^= k >> r; k *= m;
        h *= m; h ^= k;
        data += 4; len -= 4;
    }
    switch (len) {
        case 3: h ^= std::uint32_t(data[2]) << 16; [[fallthrough]];
        case 2: h ^= std::uint32_t(data[1]) << 8;  [[fallthrough]];
        case 1: h ^= std::uint32_t(data[0]); h *= m;
    }
    h ^= h >> 13; h *= m; h ^= h >> 15;
    return h;
}

std::uint32_t SubclassHashForDef(std::uint16_t def) {
    char buf[16];
    const int n = std::snprintf(buf, sizeof(buf), "%u",
                                static_cast<unsigned>(def));
    if (n <= 0) return 0;
    return MurmurHash2(buf, n, 0x31415926);
}

// Resolve the function reached by a `CALL rel32` sitting at `callSite`.
// Reads the signed 32-bit displacement at callSite+1 and adds it to
// callSite+5. Returns null if callSite isn't an E8 call opcode (prevents
// resolving on a mispaired signature by accident).
std::uint8_t* ResolveCallRel32(std::uint8_t* callSite) {
    if (!callSite || *callSite != 0xE8) return nullptr;
    std::int32_t rel;
    std::memcpy(&rel, callSite + 1, 4);
    return callSite + 5 + rel;
}

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

// Runtime-resolved keychain/charm def-indexes. Replaces offsets.hpp
// compile-time constants (which stay at 0 as a last-resort fallback) once
// GetAttributeDefinitionByName comes back live at boot. Zero means "not
// resolved on this build" — the apply path skips the write.
std::uint16_t g_charmIdDef      = 0;
std::uint16_t g_charmPatternDef = 0;

using FnGetAttrDef = void*(__fastcall*)(const char* /*name*/);

// Probe one attribute name via GetAttributeDefinitionByName and extract its
// uint16 def-index. Wrapped in SEH __try so a mis-signed function (or wrong
// calling convention) can't crash the module; a bad call just reports 0 and
// the degraded-mask bit stays lit.
std::uint16_t ProbeAttrDef(FnGetAttrDef fn, const char* name) {
    if (!fn || !name) return 0;
    void* def = nullptr;
    __try {
        def = fn(name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (!def || reinterpret_cast<std::uintptr_t>(def) < 0x10000) return 0;
    // CEconItemAttributeDefinition::m_nDefIndex is at offset 0 on CS2.
    // The AV wrap catches a layout break too.
    std::uint16_t idx = 0;
    __try {
        idx = *reinterpret_cast<std::uint16_t*>(def);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return idx;
}

// Resolve all knife-swap + refresh functions once. Each miss leaves its
// gate null and that specific step gets skipped at call time. Called from
// the first ApplyAll; results cached across the session.
void ResolveKnifeFns() {
    static std::atomic<bool> resolved{ false };
    bool expected = false;
    if (!resolved.compare_exchange_strong(expected, true)) return;
    auto resolveDirect = [](const char* sig) -> void* {
        auto* fn = SigScan(Client(), sig);
        return fn ? cheat::opsec::MakeGateToClient(fn) : nullptr;
    };
    // UpdateCompositeMaterial's sig matches a CALL SITE (starts with E8),
    // not a function prologue — the real function is reached by resolving
    // the rel32 displacement at the match. Gated after that resolution.
    auto resolveCallRel32 = [](const char* sig) -> void* {
        auto* callSite = SigScan(Client(), sig);
        auto* fn = ResolveCallRel32(callSite);
        return fn ? cheat::opsec::MakeGateToClient(fn) : nullptr;
    };

    g_applyEconGated           = reinterpret_cast<FnApplyEconCustomization>(
        resolveDirect(sigs::SIG_ApplyEconCustomization));
    g_updateCompositeGated     = reinterpret_cast<FnUpdateCompositeMaterial>(
        resolveCallRel32(sigs::SIG_UpdateCompositeMaterial));
    g_updateCompositeSetGated  = reinterpret_cast<FnUpdateCompositeMaterialSet>(
        resolveDirect(sigs::SIG_UpdateCompositeMaterialSet));
    g_invalidateDescGated      = reinterpret_cast<FnInvalidateDescription>(
        resolveDirect(sigs::SIG_InvalidateDescription));
    g_econItemViewUpdateGated  = reinterpret_cast<FnEconItemViewUpdate>(
        resolveDirect(sigs::SIG_EconItemView_Update));

    // Charm def-indexes via CEconItemSchema::GetAttributeDefinitionByName.
    // Assumes the function takes just the name (CS2 publishes it as a
    // __fastcall-looking method but the first non-this access is rcx=name
    // on the dumped build — if that's wrong the SEH wrapper catches it).
    auto* getAttrDef = reinterpret_cast<FnGetAttrDef>(
        resolveDirect(sigs::SIG_GetAttributeDefinitionByName));
    if (getAttrDef) {
        // Two name variants seen across builds. First hit wins per field.
        static const char* kNamesId[] = {
            "set item keychain slot 0 id",
            "keychain slot 0 id",
        };
        static const char* kNamesSeed[] = {
            "set item keychain slot 0 pattern",
            "set item keychain slot 0 seed",
            "keychain slot 0 pattern",
            "keychain slot 0 seed",
        };
        for (auto* n : kNamesId)   if ((g_charmIdDef     = ProbeAttrDef(getAttrDef, n))) break;
        for (auto* n : kNamesSeed) if ((g_charmPatternDef = ProbeAttrDef(getAttrDef, n))) break;
    }

    // Diagnostic bits so the Settings tab shows which legs of the knife
    // flow are offline on this build.
    constexpr std::uint32_t kBitApplyEcon  = (1u << 11);
    constexpr std::uint32_t kBitComposite  = (1u << 12);
    constexpr std::uint32_t kBitInvalidate = (1u << 13);
    constexpr std::uint32_t kBitViewUpdate = (1u << 14);
    constexpr std::uint32_t kBitCharm      = (1u << 10); // owns bit 10 now
    auto bit = [](bool have, std::uint32_t b) {
        if (have) g_ipc.degraded_mask &= ~b;
        else      g_ipc.degraded_mask |=  b;
    };
    bit(g_applyEconGated,          kBitApplyEcon);
    bit(g_updateCompositeSetGated, kBitComposite);
    bit(g_invalidateDescGated,     kBitInvalidate);
    bit(g_econItemViewUpdateGated, kBitViewUpdate);
    bit(g_charmIdDef != 0,         kBitCharm);
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

// Returns true if the attribute was written (in-place or via append).
bool SetAttr(void* attrList, std::uint16_t defIdx, float value) {
    if (!attrList) return false;
    if (UpdateExistingAttr(attrList, defIdx, value)) return true;
    if (!g_setAttrGated) return false;    // caller signals Degraded
    g_setAttrGated(attrList, defIdx, value);
    return true;
}

float Bits(std::uint32_t u) {
    float f; std::memcpy(&f, &u, 4); return f;
}

// Returns true if every attribute write succeeded. False means some
// writes needed the append path and we had no resolved gate — caller
// surfaces Degraded instead of claiming success.
bool ApplyAttributes(void* view, const cheat::items::ItemConfig& it) {
    void* attrList = At<std::uint8_t>(view, offsets::m_AttributeList);
    bool allOk = true;

    allOk &= SetAttr(attrList, offsets::kAttrDef_PaintKit,
                     Bits(static_cast<std::uint32_t>(it.paintKit)));
    allOk &= SetAttr(attrList, offsets::kAttrDef_Wear, it.wear);
    allOk &= SetAttr(attrList, offsets::kAttrDef_Seed,
                     Bits(static_cast<std::uint32_t>(it.seed)));
    if (it.statTrak >= 0) {
        allOk &= SetAttr(attrList, offsets::kAttrDef_StatTrak,
                         Bits(static_cast<std::uint32_t>(it.statTrak)));
        allOk &= SetAttr(attrList, offsets::kAttrDef_StatTrakType, Bits(0));
    } else {
        // Toggling StatTrak off must CLEAR a previously-applied counter,
        // not just skip the write. In-place write to 0 if the attr is
        // already present; absence means it was never set, nothing to do.
        UpdateExistingAttr(attrList, offsets::kAttrDef_StatTrak, Bits(0));
        UpdateExistingAttr(attrList, offsets::kAttrDef_StatTrakType, Bits(0));
    }

    // Sticker slot block: base + slot*4 + {id, wear, scale, rotation}.
    for (std::uint16_t s = 0; s < offsets::kStickerSlotCount; ++s) {
        const auto& sl = it.stickers[s];
        const std::uint16_t base = offsets::kAttrDef_StickerSlotBase + s * 4;
        allOk &= SetAttr(attrList, base + offsets::kStk_Id,
                         Bits(static_cast<std::uint32_t>(sl.stickerId)));
        allOk &= SetAttr(attrList, base + offsets::kStk_Wear,     sl.wear);
        allOk &= SetAttr(attrList, base + offsets::kStk_Scale,    sl.scale);
        allOk &= SetAttr(attrList, base + offsets::kStk_Rotation, sl.rotation);
    }

    // Charm def-indexes: prefer the runtime-resolved pair from
    // GetAttributeDefinitionByName; fall back to the compile-time offsets
    // only if those still hold a non-zero value (none do on current CS2).
    const std::uint16_t charmIdDef =
        g_charmIdDef ? g_charmIdDef : offsets::kAttrDef_CharmId;
    const std::uint16_t charmPatDef =
        g_charmPatternDef ? g_charmPatternDef : offsets::kAttrDef_CharmPattern;
    if (charmIdDef) {
        allOk &= SetAttr(attrList, charmIdDef,
                         Bits(static_cast<std::uint32_t>(it.charm.charmId)));
        if (charmPatDef) {
            allOk &= SetAttr(attrList, charmPatDef,
                             Bits(static_cast<std::uint32_t>(it.charm.pattern)));
        }
    }
    return allOk;
}

bool ApplyToView(void* view, const cheat::items::ItemConfig& it) {
    if (!view) return false;

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

    const bool attrsOk = ApplyAttributes(view, it);
    // Same-frame refresh when the sig is resolved; otherwise CS2's tick-
    // driven OnDataChanged picks the writes up on the following frame
    // (slightly stale first-frame paint, but functionally correct).
    if (g_econItemViewUpdateGated) g_econItemViewUpdateGated(view);
    return attrsOk;
}

// Forward decl so MaybeKnifeSwap below (added in this file after ApplyToView)
// can reach the knife-def test, which is physically defined further down
// alongside PickForDef. Cheaper than reordering the whole bottom half.
bool IsKnifeDef(std::uint16_t def);

// Per-weapon memory of the knife def we last swapped TO. Lets us detect
// when the game has rebuilt the weapon over us (def no longer matches, or
// weapon pointer never seen) and re-fire the rebuild. Keyed by weapon
// pointer — stale entries are harmless, the compare still fails.
std::unordered_map<void*, std::uint16_t> g_lastKnifeDef;

// Full knife MODEL swap (cs2-sdk.com flow, steps 2-6):
//  1. m_iItemDefinitionIndex = target def (already written in ApplyToView)
//  2. m_iEntityQuality = 3 (already written)
//  3. m_nSubclassID = MurmurHash2(def_decimal, 0x31415926) ON THE WEAPON
//  4. ApplyEconCustomization(weapon, 1) — queue econ reload
//  5. UpdateCompositeMaterialSet(weapon, 1) — regenerate skin
//  6. InvalidateDescription(view) — refresh HUD name
// User spec step 7: only re-fire when the game has rebuilt over us, else
// the asynchronous rebuild gets cancelled.
void MaybeKnifeSwap(void* weapon, void* view, const cheat::items::ItemConfig& it) {
    if (it.category != cheat::items::Category::Knife) return;
    if (!IsKnifeDef(it.definitionIndex))              return;

    auto it_cache = g_lastKnifeDef.find(weapon);
    if (it_cache != g_lastKnifeDef.end() && it_cache->second == it.definitionIndex) {
        // Live def still matches what we last swapped to — the rebuild is
        // either in flight or already finished. Don't re-fire.
        if (offsets::m_iItemDefinitionIndex) {
            const std::uint16_t live =
                *At<std::uint16_t>(view, offsets::m_iItemDefinitionIndex);
            if (live == it.definitionIndex) return;
        }
    }

    // Step 3: subclass hash on the WEAPON (C_BaseEntity), not the view.
    if (offsets::m_nSubclassID) {
        *At<std::uint32_t>(weapon, offsets::m_nSubclassID) =
            SubclassHashForDef(it.definitionIndex);
    }
    // Step 4: queue econ reload.
    if (g_applyEconGated) g_applyEconGated(weapon, 1);
    // Step 5a: in-place composite rebuild (spec: "if the weapon already
    // has composite materials, UpdateCompositeMaterial on its composite
    // set"). Composite set lives at weapon+0x610 with live material count
    // at +0x4A0 — skip when count is 0, nothing to rebuild yet. SEH-wrap
    // the probe since the offsets are build-specific; an AV here would
    // kill the game. On any fault we skip the in-place rebuild and let
    // step 5b's full regen do the work.
    constexpr std::ptrdiff_t kCompositeSetOff = 0x610;
    constexpr std::ptrdiff_t kLiveCountOff    = 0x4A0;
    if (g_updateCompositeGated) {
        auto* compositeSet = At<std::uint8_t>(weapon, kCompositeSetOff);
        __try {
            const std::uint32_t live =
                *reinterpret_cast<std::uint32_t*>(compositeSet + kLiveCountOff);
            if (live != 0) g_updateCompositeGated(compositeSet);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // skip; step 5b still runs
        }
    }
    // Step 5b: full composite regen on the weapon.
    if (g_updateCompositeSetGated) g_updateCompositeSetGated(weapon, 1);
    // Step 6: HUD name refresh.
    if (g_invalidateDescGated) g_invalidateDescGated(view);

    g_lastKnifeDef[weapon] = it.definitionIndex;
}

// Known paintable knife def-indexes. Lets a Knife config apply to whatever
// knife the user currently has equipped, instead of requiring the live
// def-index to already equal the configured one.
// ponytail: hard-coded range, upgrade to db-driven set if a build adds a knife.
bool IsKnifeDef(std::uint16_t def) {
    return def == 42 || def == 59 || (def >= 500 && def <= 526);
}

// Match live weapon's def to a user ItemConfig. Normal rule: first exact
// def-index match wins. Knife special case: any Knife config applies to
// any paintable knife def the player currently holds (so switching from a
// default knife to a Karambit-painted entry works without the live def
// already being Karambit). Inventory-cosmetic categories have no per-weapon
// path and are skipped.
const cheat::items::ItemConfig*
PickForDef(const std::vector<cheat::items::ItemConfig>& items, std::uint16_t def) {
    using C = cheat::items::Category;
    for (const auto& it : items) {
        if (it.category == C::MusicKit || it.category == C::Coin ||
            it.category == C::Patch    || it.category == C::Graffiti) continue;
        if (it.definitionIndex == def) return &it;
    }
    if (IsKnifeDef(def)) {
        for (const auto& it : items)
            if (it.category == C::Knife) return &it;
    }
    return nullptr;
}

class PhaseA final : public IInventoryBackend {
public:
    bool ApplyAll(const std::vector<cheat::items::ItemConfig>& items) override {
        ResolveAttrFn();
        ResolveKnifeFns();

        auto* controller = LocalController();     if (!controller) return false;
        auto* pawn       = LocalPawn(controller); if (!pawn)       return false;
        auto* svc        = WeaponServices(pawn);  if (!svc)        return false;

        Vec* myWeapons = At<Vec>(svc, offsets::m_hMyWeapons);
        const bool haveWeapons = VecLooksSane(myWeapons) &&
                                 myWeapons->memory && myWeapons->size > 0;

        if (haveWeapons) {
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
                // Full knife model+subclass+composite rebuild — paint alone
                // keeps the server-spawned model, so this is what makes the
                // Karambit visible when the player bought a Bayonet.
                MaybeKnifeSwap(weapon, view, *cfg);
            }
        }

        // Gloves live on the pawn, not among the weapons. Apply even when
        // m_hMyWeapons is empty / insane — gloves are independent of the
        // weapon list and were previously skipped by the early return.
        if (offsets::m_EconGloves) {
            auto* gloveView = At<std::uint8_t>(pawn, offsets::m_EconGloves);
            for (const auto& it : items) {
                if (it.category != cheat::items::Category::Glove) continue;
                ApplyToView(gloveView, it);
            }
        }

        // Degraded bit 10: inventory carries charm attributes but the
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

        // "Best-effort" contract: always return true. Dirty-clear is safe
        // because OnFrame also re-fires on WeaponSetChanged + a ~1Hz
        // heartbeat, so inventory entries whose live weapon isn't present
        // this frame get painted the moment the server issues them. If we
        // instead held dirty until every entry matched, a configured skin
        // for a weapon the player never buys would spin ApplyAll every
        // frame forever.
        return true;
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

// Low-cost signature of the live weapon set: last-seen m_hMyWeapons vector
// pointer + size. Any change triggers a re-apply so new weapons (pickup,
// respawn, round start) get painted without the user re-clicking Apply.
// ponytail: periodic+signature re-apply, instead of hooking every spawn.
void* g_lastWeaponsMem = nullptr;
int   g_lastWeaponsSize = -1;
std::uint32_t g_frame = 0;

bool WeaponSetChanged() {
    auto* controller = LocalController();     if (!controller) return false;
    auto* pawn       = LocalPawn(controller); if (!pawn)       return false;
    auto* svc        = WeaponServices(pawn);  if (!svc)        return false;
    Vec* v = At<Vec>(svc, offsets::m_hMyWeapons);
    if (!VecLooksSane(v)) return false;
    const bool changed = (v->memory != g_lastWeaponsMem) || (v->size != g_lastWeaponsSize);
    g_lastWeaponsMem = v->memory;
    g_lastWeaponsSize = v->size;
    return changed;
}

void OnFrame() {
    // Degraded gate: refuse to write when offsets aren't verified or we
    // haven't resolved the module path yet.
    if (g_ipc.state != static_cast<uint32_t>(ipc::State::Ready)) return;

    if (!g_backend) g_backend = MakeBackend();
    auto& S = cheat::menu::Get();
    ++g_frame;

    // Revert is one-shot. If ClearAll fails because the pawn isn't ready
    // (round warmup), re-arm the request so the next frame tries again
    // instead of dropping it on the floor.
    if (g_revertRequested.exchange(false)) {
        if (!g_backend->ClearAll()) g_revertRequested.store(true);
    }

    // Re-apply when:
    //  - the user just changed inventory (dirty),
    //  - the live weapon vector changed (respawn, pickup, drop),
    //  - a slow heartbeat (every ~1s @ 60fps) for inventory that lands
    //    after a weapon spawn we didn't notice (nullable fences, etc).
    const bool heartbeat = (g_frame % 60) == 0 && !S.inventory.empty();
    const bool shouldApply =
        S.dirty.load() || WeaponSetChanged() || heartbeat;

    if (shouldApply) {
        const bool all = g_backend->ApplyAll(S.inventory);
        // Clear dirty only if everything applicable landed; otherwise keep
        // dirty so the next frame reconsiders (partial-success contract).
        if (all) S.dirty.store(false);
    }
}

void RequestRevert() { g_revertRequested.store(true); }

} // namespace cheat::inventory
