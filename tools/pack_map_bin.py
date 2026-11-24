#!/usr/bin/env python3
"""Pack radar-tiles-v1 JSON tiles into one little-endian map.bin for QNX.

The HU must not open 292k JSON files. Runtime seeks this pack by (tx, ty).
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TILES = ROOT / "data" / "maps" / "tiles"
OUT = ROOT / "hu" / "radar" / "map.bin"

MAGIC = b"RMAP1\0\0\0"
KIND = {"lo": 0, "ar": 1, "hw": 2, "line": 8, "poly": 9}


def pack_geom(kind: int, coords: list) -> bytes:
    n = min(len(coords), 1023)
    buf = bytearray()
    buf.append(kind & 0xFF)
    buf += struct.pack("<H", n)
    for i in range(n):
        lon, lat = coords[i]
        buf += struct.pack("<ii", int(round(float(lon) * 1e6)), int(round(float(lat) * 1e6)))
    return bytes(buf)


def pack_tile(path: Path) -> bytes:
    body = json.loads(path.read_text(encoding="utf-8"))
    geoms: list[bytes] = []
    for rec in body.get("roads") or []:
        g = rec.get("g") or []
        if len(g) < 2:
            continue
        geoms.append(pack_geom(KIND.get(rec.get("c"), 0), g))
    for rec in body.get("water") or []:
        g = rec.get("g") or []
        if len(g) < 2:
            continue
        k = 9 if rec.get("c") == "poly" else 8
        geoms.append(pack_geom(k, g))
    return struct.pack("<H", len(geoms)) + b"".join(geoms)


def main() -> int:
    index_path = TILES / "index.json"
    if not index_path.exists():
        print(f"missing {index_path} — run extract_osm_geojson.py --full first", file=sys.stderr)
        return 1
    meta = json.loads(index_path.read_text(encoding="utf-8"))
    keys = meta.get("tiles") or []
    parsed: list[tuple[int, int, str]] = []
    for key in keys:
        a, b = key.split("_", 1)
        parsed.append((int(a), int(b), key))
    parsed.sort()
    n = len(parsed)
    deg_e6 = int(round(float(meta.get("tile_deg") or 0.02) * 1e6))
    header_len = 32
    index_len = n * 16
    data_off = header_len + index_len
    OUT.parent.mkdir(parents=True, exist_ok=True)
    print(f"packing {n} tiles -> {OUT}  data_off={data_off}", flush=True)

    entries: list[tuple[int, int, int, int]] = []
    with OUT.open("wb") as f:
        f.write(b"\0" * data_off)
        off = data_off
        for i, (tx, ty, key) in enumerate(parsed):
            blob = pack_tile(TILES / f"{key}.json")
            f.write(blob)
            entries.append((tx, ty, off, len(blob)))
            off += len(blob)
            if (i + 1) % 20000 == 0:
                print(f"  {i + 1:,}/{n:,}  {off / 1e6:.1f} MB", flush=True)
        f.seek(0)
        f.write(struct.pack(
            "<8sIIIIII",
            MAGIC, 1, deg_e6, n, header_len, data_off, 0,
        ))
        for tx, ty, o, ln in entries:
            f.write(struct.pack("<iiII", tx, ty, o, ln))
    print(f"wrote {OUT}  {OUT.stat().st_size / 1e6:.1f} MB  tiles={n}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
