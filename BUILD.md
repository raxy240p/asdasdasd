# Build

Windows + MSVC x64 only. CMake 3.24+, Visual Studio 2022 (or build tools), git.
First-time clone fetches ImGui / nlohmann_json / stb via FetchContent — needs
network on the first configure.

## Fresh build

```
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
```

Outputs:

- `build/injector/Release/loader.exe` — launcher (ImGui window + manual-map)
- `build/module/Release/cheat.dll`    — in-process cheat module
- `build/injector/Release/data/items.json` — DB auto-staged beside the loader

## Options

| Flag | Default | What it does |
|------|---------|--------------|
| `-DSK_MODULE_STOMP=ON`  | off | Use module-stomp injection instead of manual-map |
| `-DSK_VAC_RECON=ON`     | off | Compile the §6.4 VAC recon utilities              |

Append on the configure line, e.g.
`cmake -S . -B build -A x64 -DSK_MODULE_STOMP=ON`.

## Rebuild one target only

```
cmake --build build --config Release --target loader
cmake --build build --config Release --target cheat
```

## Clean rebuild

```
rmdir /s /q build
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
```

## Data regeneration

Pull a public ByMykel CSGO-API checkout, then:

```
python scripts/gen_items_db.py --root path\to\CSGO-API\public\api\en
```

Writes `data/items.json` + `data/items.json.sha256`. The next CMake build copies
it into `build/injector/Release/data/`.

## Offsets refresh

```
python scripts/update_offsets.py
```

Rewrites `common/offsets.hpp` from the latest cs2-dumper feed. Rebuild after.

## Run

Place `loader.exe` and `cheat.dll` beside each other (default layout from the
Release output already does this via `randomize::CheatDllPathForThisRun`).
CS2 must be running and no listed anticheat/analysis process active.
INSERT toggles the in-game menu once injected.
