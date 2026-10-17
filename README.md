# cs2 skin loader

Two programs. The injector maps `cheat.dll` into `cs2.exe` on your click.
The DLL lives inside the game and renders a menu over it (INSERT to toggle).

The user launches Steam + CS2 themselves. The injector only watches and
injects. Nothing here starts the game or touches `trustedlaunch.cfg`.

## Layout

```
repo/
├─ injector/      Win32 + ImGui + DX11 loader exe
├─ module/        the cheat DLL
│  ├─ hooks/        inline detours (Present, WndProc)
│  ├─ menu/         Inventory / Add Item / Settings tabs
│  ├─ items/        data model + preview
│  ├─ inventory/    Phase-A backend (skin changer)
│  ├─ render/       DX11 bringup + icon cache
│  └─ opsec/        call gates, header erase, offset verifier
├─ common/        shared offsets.hpp / signatures.hpp / ipc.hpp
├─ scripts/       update_offsets.py, gen_items_db.py
└─ data/          items.json (generated)
```

## Build

```powershell
# x64 only, MSVC. CMake pulls ImGui / nlohmann-json / stb automatically.
cmake --preset=win64            # or: cmake -S . -B build -A x64
cmake --build build --config Release
```

Artifacts land in `build\Release\loader.exe` and `build\Release\cheat.dll`.
Put them next to each other and run `loader.exe`.

## Flow

1. Run `loader.exe`. It self-copies to a random `%TEMP%\<rand>\<rand>.exe`
   and relaunches. The original melts.
2. The window lists Vanguard / EAC / BattlEye / FACEIT / Steam process
   status. Steam must be closed at launch; close any that are red, hit
   `RECHECK`.
3. Launch Steam, then CS2. The injector detects the window; `INJECT` enables.
4. Click `INJECT`. The loader reads `cheat.dll` into memory and manual-maps
   it into `cs2.exe` via thread hijack. On ready-flag it shows "Injected".
5. In game: press `INSERT`. Menu opens: Inventory / Add Item / Settings.

## Offsets

All CS2 offsets live in `common/offsets.hpp`. `0x0` means unverified
placeholder. Refresh with:

```powershell
python scripts/update_offsets.py
# pulls from SteamTracking / cs2-dumper (a2x) feeds
```

Source of truth: <https://github.com/SteamTracking/GameTracking-CS2>.

The module boots to `Ready` only when the full `kRequired` set in
`offsets.hpp` is non-zero. Any zero drops it to `Degraded`: menu still
works, `ApplyAll` no-ops, the Settings tab lists which offsets are TODO.

## Items DB

```powershell
# ByMykel-style layout
python scripts/gen_items_db.py --root path\to\dumps
# or individual files
python scripts/gen_items_db.py --skins skins.json --stickers stickers.json ...
```

Writes `data/items.json` + a short SHA-256 next to it.

## Status

- **Offsets**: baked from cs2-dumper (`output/offsets.json` +
  `output/client_dll.json`) anchored on SteamTracking/GameTracking-CS2.
  CSGO `m_nFallbackPaintKit`-family fields do not exist on CS2 and are
  kept as 0 by design — CS2 uses attribute injection via
  `CAttributeList::SetOrAddAttributeValueByName`.
- **Phase-B walker**: live. Controller → pawn → `CPlayer_WeaponServices`
  → `m_hMyWeapons` → `m_AttributeManager + m_Item` → `C_EconItemView`.
  All outbound into `client.dll` routes through the call-gate pool.
- **Live preview**: live. Off-screen DX11 render target with a textured
  rotating quad; the panorama draw is a real DX11 pass, not a static icon.
- **`SK_VAC_RECON`** (`-DSK_VAC_RECON=ON`): hooks the 161/163 payload
  builders and SuspendAll/ResumeAll around their bodies; passively reads
  the VacProcessMonitor shared mapping. Off by default.
- **`SK_MODULE_STOMP`** (`-DSK_MODULE_STOMP=ON`, injector-side): fallback
  mapper that places the image inside unused RX padding of a signed,
  loaded, LDR-tracked module (`rendersystemdx11.dll` by default). Off
  by default.

## Open items (per spec §9)

- Attribute-set function (`SetOrAddAttributeValueByName`) is resolved by
  AOB at runtime; refresh the sig after schema-breaking game updates.
- `fn_EconItemView_Update` sig refresh — kept in `signatures.hpp`,
  resolver lives in `backend_phase_a.cpp`.
- Full `CCSPlayer_InventoryServices::m_vecLoadoutItems` walk — cs2-dumper
  doesn't expose the field; Phase-B uses `m_hMyWeapons` which covers
  held/dropped weapons only.

## VAC posture summary (ground rules — do not break)

- No LDR entry, no new threads, no VEH, no debug registers, no VMT swaps.
- Inline detours only; registry restores byte-for-byte.
- Outbound calls into `client.dll` go through call gates (`opsec/call_gate`).
- Never write `EntitySpottedState_t` or `CGlowProperty`. This is a skin
  changer. The field monitor is memcmp-watching those exact objects.
- Never open files with EXECUTE from inside the game. Plain reads (icons,
  config) are fine.
