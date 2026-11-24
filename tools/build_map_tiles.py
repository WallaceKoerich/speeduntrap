#!/usr/bin/env python3
"""Cut OSM into ~2 km tiles. Runtime (PC and QNX) loads only the window
around the car — same idea as streaming game chunks.

Tile id is floor(lon / tile_deg), floor(lat / tile_deg). A way that
crosses a border is stored in every tile it touches so the viewer never
clips a road at the seam.
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAPS = ROOT / "data" / "maps"
EXTRACT = MAPS / "pc_extract.json"
TILES = MAPS / "tiles"
TILE_DEG = 0.02  # ~2.2 km
FORMAT = "radar-tiles-v1"


def tile_xy(lat: float, lon: float, deg: float = TILE_DEG) -> tuple[int, int]:
    return int(math.floor(lon / deg)), int(math.floor(lat / deg))


def tiles_along(coords: list[list[float]], deg: float = TILE_DEG) -> set[tuple[int, int]]:
    out: set[tuple[int, int]] = set()
    prev: tuple[int, int] | None = None
    for lon, lat in coords:
        tx, ty = tile_xy(lat, lon, deg)
        if prev is not None:
            px, py = prev
            steps = max(abs(tx - px), abs(ty - py), 1)
            for i in range(steps + 1):
                out.add((px + (tx - px) * i // steps, py + (ty - py) * i // steps))
        else:
            out.add((tx, ty))
        prev = (tx, ty)
    return out


def slice_extract(src: Path, dest: Path, deg: float = TILE_DEG) -> dict:
    pack = json.loads(src.read_text(encoding="utf-8"))
    buckets: dict[tuple[int, int], dict] = defaultdict(lambda: {"roads": [], "water": []})
    for rec in pack.get("roads") or []:
        g = rec.get("g") or []
        if len(g) < 2:
            continue
        for t in tiles_along(g, deg):
            buckets[t]["roads"].append(rec)
    for rec in pack.get("water") or []:
        g = rec.get("g") or []
        if len(g) < 2:
            continue
        for t in tiles_along(g, deg):
            buckets[t]["water"].append(rec)

    if dest.exists():
        for old in dest.glob("*.json"):
            if old.name != "index.json":
                old.unlink()
    dest.mkdir(parents=True, exist_ok=True)

    keys: list[str] = []
    for (tx, ty), body in sorted(buckets.items()):
        key = f"{tx}_{ty}"
        keys.append(key)
        (dest / f"{key}.json").write_text(
            json.dumps({"tx": tx, "ty": ty, "roads": body["roads"], "water": body["water"]},
                       separators=(",", ":")),
            encoding="utf-8",
        )

    index = {
        "format": FORMAT,
        "tile_deg": deg,
        "tile_m": round(deg * 111320.0),
        "tiles": keys,
        "source": src.name,
        "stats": {
            "tiles": len(keys),
            "roads": len(pack.get("roads") or []),
            "water": len(pack.get("water") or []),
        },
    }
    (dest / "index.json").write_text(json.dumps(index, separators=(",", ":")), encoding="utf-8")
    return index


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--extract", type=Path, default=EXTRACT)
    ap.add_argument("--out", type=Path, default=TILES)
    ap.add_argument("--tile-deg", type=float, default=TILE_DEG)
    args = ap.parse_args()
    if not args.extract.exists():
        print(f"missing {args.extract}", file=sys.stderr)
        return 1
    index = slice_extract(args.extract, args.out, args.tile_deg)
    print(
        f"wrote {args.out}  tiles={index['stats']['tiles']}  "
        f"{index['tile_m']} m cells  format={FORMAT}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
