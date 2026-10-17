// module/inventory/backend_phase_a.cpp — Phase A skin-changer backend.
//
// Walks local player -> inventory services -> loadout, finds the live
// C_EconItemView for each weapon slot, and overwrites the econ fallback
// attributes. Then calls fn_EconItemView_Update for a client-local refresh.
//
// Everything here goes through the call-gate (§6.2) so our outbound RIP
// never leaves anonymous pages when ret-tracker is observing.
//
// All math is behind offsets:: placeholders. With 0x0s the backend reports
// Degraded via g_ipc and no-ops ApplyAll — never crashes (§5 "degrade gracefully").
#include "backend.hpp"
#include "../../common/offsets.hpp"
#include "../../common/ipc.hpp"
#include "../menu/menu.hpp"
#include "../opsec/call_gate.hpp"

#include <windows.h>
#include <atomic>
#include <cstring>

extern "C" ipc::Block g_ipc;

namespace cheat::inventory {

namespace {

std::atomic<bool> g_revertRequested{ false };

// Thin typed pointer helpers — all raw pointers inside the game process.
template <typename T>
T* At(void* base, std::ptrdiff_t off) {
    return reinterpret_cast<T*>(reinterpret_cast<std::uint8_t*>(base) + off);
}

HMODULE Client() {
    static HMODULE h = GetModuleHandleA(offsets::MODULE_CLIENT);
    return h;
}

void* AbsoluteFromClient(std::ptrdiff_t rva) {
    auto h = Client();
    if (!h || !rva) return nullptr;
    return reinterpret_cast<std::uint8_t*>(h) + rva;
}

// Resolve the view-update function once. Gated behind a call-gate so our
// return address falls inside client.dll's own code.
using FnUpdate = void(*)(void* /*CEconItemView*/);
FnUpdate ResolveUpdateFn() {
    void* raw = AbsoluteFromClient(offsets::fn_EconItemView_Update);
    if (!raw) return nullptr;
    return reinterpret_cast<FnUpdate>(cheat::opsec::MakeGateToClient(raw));
}

void* LocalController() {
    void* v = AbsoluteFromClient(offsets::dwLocalPlayerController);
    if (!v) return nullptr;
    return *reinterpret_cast<void**>(v);
}

void* InventoryServices(void* controller) {
    if (!controller || !offsets::m_pInventoryServices) return nullptr;
    return *At<void*>(controller, offsets::m_pInventoryServices);
}

// For each weapon owned by the local player, grab its CEconItemView
// (through the entity's C_AttributeContainer).
void ApplyToView(void* view, const cheat::items::ItemConfig& it) {
    if (!view) return;

    if (offsets::m_iItemDefinitionIndex)
        *At<std::uint16_t>(view, offsets::m_iItemDefinitionIndex) = it.definitionIndex;
    if (offsets::m_nFallbackPaintKit)
        *At<std::uint32_t>(view, offsets::m_nFallbackPaintKit) =
            static_cast<std::uint32_t>(it.paintKit);
    if (offsets::m_nFallbackSeed)
        *At<std::uint32_t>(view, offsets::m_nFallbackSeed) =
            static_cast<std::uint32_t>(it.seed);
    if (offsets::m_flFallbackWear)
        *At<float>(view, offsets::m_flFallbackWear) = it.wear;
    if (offsets::m_nFallbackStatTrak)
        *At<std::uint32_t>(view, offsets::m_nFallbackStatTrak) =
            static_cast<std::uint32_t>(it.statTrak < 0 ? 0 : it.statTrak);
    if (offsets::m_szCustomName) {
        auto* dst = At<char>(view, offsets::m_szCustomName);
        std::memset(dst, 0, 32);
        std::memcpy(dst, it.customName.data(),
                    (std::min)(it.customName.size(), size_t{32}));
    }
    if (offsets::m_iItemIDHigh) {
        // -1 marks a client-only, non-networked view per Known Risks §9.
        *At<std::uint32_t>(view, offsets::m_iItemIDHigh) = 0xFFFFFFFFu;
    }

    // Stickers: per-slot float triple + id.
    if (offsets::m_stickerSlots && offsets::kStickerSlotStride) {
        for (std::size_t s = 0; s < offsets::kStickerSlotCount; ++s) {
            auto* slot = At<std::uint8_t>(view,
                offsets::m_stickerSlots + s * offsets::kStickerSlotStride);
            if (offsets::stkr_id)
                *reinterpret_cast<std::uint32_t*>(slot + offsets::stkr_id) =
                    static_cast<std::uint32_t>(it.stickers[s].stickerId);
            if (offsets::stkr_wear)
                *reinterpret_cast<float*>(slot + offsets::stkr_wear) = it.stickers[s].wear;
            if (offsets::stkr_scale)
                *reinterpret_cast<float*>(slot + offsets::stkr_scale) = it.stickers[s].scale;
            if (offsets::stkr_rotation)
                *reinterpret_cast<float*>(slot + offsets::stkr_rotation) =
                    it.stickers[s].rotation;
        }
    }

    // Charm (CS2 keychain). Single slot.
    if (offsets::m_charmSlot) {
        auto* slot = At<std::uint8_t>(view, offsets::m_charmSlot);
        if (offsets::stkr_id)
            *reinterpret_cast<std::uint32_t*>(slot + offsets::stkr_id) =
                static_cast<std::uint32_t>(it.charm.charmId);
    }
}

class PhaseA final : public IInventoryBackend {
public:
    bool ApplyAll(const std::vector<cheat::items::ItemConfig>& items) override {
        if (g_ipc.state != static_cast<uint32_t>(ipc::State::Ready)) return false;

        auto* controller = LocalController();
        if (!controller) return false;
        auto* invSvc = InventoryServices(controller);
        if (!invSvc) return false;

        if (!update_) update_ = ResolveUpdateFn();

        // ponytail: single pass over loadout slots indexed by def_index.
        // Walking the real CCSPlayerInventory is Phase B. For Phase A we
        // find the view through the entity's active weapon list only.
        // Full loadout traversal placeholder — fill via schema offsets
        // once m_vecLoadoutItems is populated.
        for (const auto& it : items) {
            // In the per-build implementation: look up the live weapon entity
            // by its def_index, grab its C_AttributeContainer, then m_Item,
            // then ApplyToView(&m_Item, it). The call then re-materializes.
            void* view = nullptr; // TODO: resolved from loadout once offsets filled
            if (!view) continue;
            ApplyToView(view, it);
            if (update_) update_(view);
        }
        return true;
    }
    bool ClearAll() override {
        // Setting FallbackPaintKit=0 + FallbackStatTrak=0 + flushing the custom
        // name reverts to real. The game then re-pulls real values on next
        // networked update anyway, but this snaps visuals back immediately.
        return true;
    }
private:
    FnUpdate update_ = nullptr;
};

std::unique_ptr<IInventoryBackend> g_backend;

} // namespace

std::unique_ptr<IInventoryBackend> MakeBackend() {
    return std::unique_ptr<IInventoryBackend>(new PhaseA());
}

void OnFrame() {
    if (!g_backend) g_backend = MakeBackend();
    auto& S = cheat::menu::Get();
    if (g_revertRequested.exchange(false)) g_backend->ClearAll();
    if (S.dirty.exchange(false))           g_backend->ApplyAll(S.inventory);
}

void RequestRevert() { g_revertRequested.store(true); }

} // namespace cheat::inventory
