#!/usr/bin/env python3
# scripts/update_offsets.py — regenerates common/offsets.hpp from the public
# SteamTracking / cs2-dumper feeds.
#
# Source of truth per spec:
#   https://github.com/SteamTracking/GameTracking-CS2
# and the a2x-style JSON dumps linked from there (cs2-dumper offsets.json /
# schema.json). This script accepts either a local path or a raw URL.

from __future__ import annotations

import argparse
import json
import re
import sys
import textwrap
from pathlib import Path
from urllib.request import urlopen


DEFAULT_OFFSETS_URL = "https://raw.githubusercontent.com/a2x/cs2-dumper/main/output/offsets.json"
DEFAULT_SCHEMA_URL  = "https://raw.githubusercontent.com/a2x/cs2-dumper/main/output/client.dll.json"

HEADER = Path(__file__).resolve().parent.parent / "common" / "offsets.hpp"

# Keys we want. LHS = C++ identifier in offsets.hpp. RHS = (source, path...)
#   source == "client" -> cs2-dumper offsets.json: client.dll.<name>
#   source == "schema" -> schema.json: <class>.<field>
WANT = [
    ("dwEntityList",             "client", "client.dll", "dwEntityList"),
    ("dwLocalPlayerController",  "client", "client.dll", "dwLocalPlayerController"),
    ("dwLocalPlayerPawn",        "client", "client.dll", "dwLocalPlayerPawn"),
    ("dwViewMatrix",             "client", "client.dll", "dwViewMatrix"),
    ("dwViewAngles",             "client", "client.dll", "dwViewAngles"),
    ("dwGlobalVars",             "client", "client.dll", "dwGlobalVars"),

    ("m_hPlayerPawn",            "schema", "CCSPlayerController",       "m_hPlayerPawn"),
    ("m_pInventoryServices",     "schema", "CCSPlayerController",       "m_pInventoryServices"),
    ("m_vecLoadoutItems",        "schema", "CCSPlayer_InventoryServices","m_vecLoadoutItems"),
    ("m_AttributeManager",       "schema", "C_EconEntity",              "m_AttributeManager"),
    ("m_Item",                   "schema", "C_AttributeContainer",      "m_Item"),

    ("m_iItemDefinitionIndex",   "schema", "C_EconItemView",            "m_iItemDefinitionIndex"),
    ("m_iEntityQuality",         "schema", "C_EconItemView",            "m_iEntityQuality"),
    ("m_iItemIDHigh",            "schema", "C_EconItemView",            "m_iItemIDHigh"),
    ("m_iItemIDLow",             "schema", "C_EconItemView",            "m_iItemIDLow"),
    ("m_iAccountID",             "schema", "C_EconItemView",            "m_iAccountID"),
    ("m_nFallbackPaintKit",      "schema", "C_EconItemView",            "m_nFallbackPaintKit"),
    ("m_nFallbackSeed",          "schema", "C_EconItemView",            "m_nFallbackSeed"),
    ("m_flFallbackWear",         "schema", "C_EconItemView",            "m_flFallbackWear"),
    ("m_nFallbackStatTrak",      "schema", "C_EconItemView",            "m_nFallbackStatTrak"),
    ("m_szCustomName",           "schema", "C_EconItemView",            "m_szCustomName"),
    ("m_bInitialized",           "schema", "C_EconItemView",            "m_bInitialized"),
]


def load_json(src: str) -> dict:
    if src.startswith(("http://", "https://")):
        with urlopen(src, timeout=15) as r:
            return json.load(r)
    return json.loads(Path(src).read_text(encoding="utf-8"))


def dig(d: dict, *parts: str):
    cur = d
    for p in parts:
        if cur is None: return None
        cur = cur.get(p)
    return cur


def resolve(name: str, source: str, offsets_doc: dict, schema_doc: dict, path: tuple):
    if source == "client":
        return dig(offsets_doc, *path)
    if source == "schema":
        # schema.json shape: { "client.dll": { "classes": { "C_Foo": { "fields": { "m_x": N } } } } }
        cls, fld = path
        candidates = [
            ("client.dll", "classes", cls, "fields", fld),
            ("classes", cls, "fields", fld),
            (cls, "fields", fld),
            (cls, fld),
        ]
        for cand in candidates:
            v = dig(schema_doc, *cand)
            if v is not None: return v
    return None


def write_header(resolved: dict[str, int | None]) -> str:
    text = HEADER.read_text(encoding="utf-8")
    out = []
    for line in text.splitlines(keepends=True):
        m = re.match(r"(inline constexpr std::ptrdiff_t\s+)(\w+)(\s*=\s*)([^;]+);", line)
        if m and m.group(2) in resolved and resolved[m.group(2)] is not None:
            val = resolved[m.group(2)]
            new = f"{m.group(1)}{m.group(2)}{m.group(3)}0x{val:X};"
            out.append(re.sub(r"//.*$", "", new).rstrip() + "\n")
        else:
            out.append(line)
    return "".join(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--offsets", default=DEFAULT_OFFSETS_URL,
                    help="cs2-dumper offsets.json (url or path)")
    ap.add_argument("--schema",  default=DEFAULT_SCHEMA_URL,
                    help="cs2-dumper client.dll.json schema (url or path)")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    off = load_json(args.offsets)
    sch = load_json(args.schema)

    resolved: dict[str, int | None] = {}
    for name, src, *rest in WANT:
        v = resolve(name, src, off, sch, tuple(rest))
        resolved[name] = int(v) if isinstance(v, int) else None

    missing = [k for k, v in resolved.items() if v is None]
    if missing:
        print("warn: unresolved:", ", ".join(missing), file=sys.stderr)

    new_text = write_header(resolved)
    if args.dry_run:
        sys.stdout.write(new_text)
    else:
        HEADER.write_text(new_text, encoding="utf-8")
        print(f"wrote {HEADER} ({len(resolved) - len(missing)} resolved, {len(missing)} TODO)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
