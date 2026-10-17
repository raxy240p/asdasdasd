#!/usr/bin/env python3
# scripts/gen_items_db.py — compile a public ByMykel-style CS2 item dump
# into data/items.json consumed at runtime per §5.3.
#
# Input: one JSON per subject (skins, stickers, charms, agents, cases, keys).
# Can take a repo root (expects skins.json etc.) or individual --skins paths.

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
OUT  = ROOT / "data" / "items.json"


CAT_FROM_WEAPON_CATEGORY = {
    "rifles":   "Rifles",
    "smgs":     "SMGs",
    "pistols":  "Pistols",
    "heavy":    "Heavy",
    "shotguns": "Heavy",
    "sniper rifles": "Rifles",
    "machineguns":   "Heavy",
    "knives":   "Knives",
    "gloves":   "Gloves",
}


def norm_rarity(r: str) -> str:
    r = (r or "").lower().replace(" ", "_")
    if "contraband" in r:        return "contraband"
    if "covert" in r:            return "covert"
    if "classified" in r:        return "classified"
    if "restricted" in r:        return "restricted"
    if "mil"       in r:         return "milspec"
    if "industrial" in r:        return "industrial"
    if "consumer" in r or "common" in r: return "common"
    if "extraordinary" in r or "exceedingly" in r: return "exceedingly_rare"
    return r or "common"


def ingest_skins(path: Path, out: dict):
    data = json.loads(path.read_text(encoding="utf-8"))
    for row in data:
        wname = row.get("weapon", {}).get("name") or row.get("weapon", {}).get("weapon_name")
        if not wname: continue
        cat_raw = (row.get("category", {}) or {}).get("name", "")
        cat_name = CAT_FROM_WEAPON_CATEGORY.get(cat_raw.lower(), "Rifles")
        buck = out["categories"].setdefault(cat_name, {})
        w    = buck.setdefault(wname, {
            "def_index": row.get("weapon", {}).get("weapon_id", 0),
            "paints": [],
        })
        w["paints"].append({
            "name": row.get("pattern", {}).get("name", row.get("name", "?")),
            "kit":  row.get("paint_index", 0),
            "rarity": norm_rarity((row.get("rarity") or {}).get("name", "")),
            "finish_quality_min": row.get("min_float", 0.0),
            "max":                row.get("max_float", 1.0),
        })


def ingest_simple(path: Path, out_bucket: dict, id_key: str, def_key: str):
    for row in json.loads(path.read_text(encoding="utf-8")):
        name = row.get("name") or row.get("market_hash_name")
        if not name: continue
        out_bucket[name] = {
            id_key: row.get(id_key) or row.get(def_key, 0),
            "rarity": norm_rarity((row.get("rarity") or {}).get("name", "")),
        }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, help="Dir containing *.json dumps (ByMykel layout)")
    ap.add_argument("--skins",    type=Path)
    ap.add_argument("--stickers", type=Path)
    ap.add_argument("--charms",   type=Path)
    ap.add_argument("--agents",   type=Path)
    ap.add_argument("--cases",    type=Path)
    ap.add_argument("--keys",     type=Path)
    args = ap.parse_args()

    out = { "schema_version": 2, "categories": {}, "Stickers": {}, "Charms": {},
            "Agents": {}, "Cases": {}, "Keys": {}, }

    def pick(flag, default_name):
        if flag: return flag
        if args.root: return args.root / default_name
        return None

    if p := pick(args.skins,    "skins.json"):    ingest_skins(p, out)
    if p := pick(args.stickers, "stickers.json"): ingest_simple(p, out["Stickers"], "sticker_id", "id")
    if p := pick(args.charms,   "keychains.json"):ingest_simple(p, out["Charms"],   "charm_id",   "id")
    if p := pick(args.agents,   "agents.json"):   ingest_simple(p, out["Agents"],   "def_index",  "def_index")
    if p := pick(args.cases,    "crates.json"):   ingest_simple(p, out["Cases"],    "def_index",  "def_index")
    if p := pick(args.keys,     "keys.json"):     ingest_simple(p, out["Keys"],     "def_index",  "def_index")

    OUT.parent.mkdir(parents=True, exist_ok=True)
    body = json.dumps(out, indent=2, sort_keys=True)
    OUT.write_text(body, encoding="utf-8")
    sig = hashlib.sha256(body.encode()).hexdigest()[:12]
    (OUT.with_suffix(".sha256")).write_text(sig + "\n", encoding="utf-8")
    print(f"wrote {OUT}  checksum={sig}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
