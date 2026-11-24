#!/usr/bin/env python3
"""Cut highways + water from the Geofabrik PBFs into a compact JSON pack.

PC emulator only. QNX still has no map renderer.
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

from collections import defaultdict

from osmiter import iter_from_osm

ROOT = Path(__file__).resolve().parents[1]
MAPS = ROOT / "data" / "maps"
DEFAULT_OUT = MAPS / "pc_extract.json"
TILES = MAPS / "tiles"
TILE_DEG = 0.02
DEFAULT_LAT = -26.961387
DEFAULT_LON = -50.422295
DEFAULT_HDG = 27.9

HIGHWAY_CLASS = {
    "motorway": "hw",
    "motorway_link": "hw",
    "trunk": "hw",
    "trunk_link": "hw",
    "primary": "ar",
    "primary_link": "ar",
    "secondary": "ar",
    "secondary_link": "ar",
    "tertiary": "lo",
    "tertiary_link": "lo",
    "unclassified": "lo",
    "residential": "lo",
    "living_street": "lo",
}

WATER_NATURAL = {"water", "bay", "pond", "lake"}
WATER_LANDUSE = {"reservoir", "basin"}
WATERWAY_POLY = {"riverbank", "dock"}
WATERWAY_LINE = {"river", "canal", "stream"}


def rd(v: float) -> float:
    return round(v, 6)


def in_bbox(lat: float, lon: float, bb: tuple[float, float, float, float]) -> bool:
    west, south, east, north = bb
    return south <= lat <= north and west <= lon <= east


def simplify(coords: list[list[float]], min_m: float = 6.0) -> list[list[float]]:
    if len(coords) <= 2:
        return coords
    keep = [coords[0]]
    plat, plon = coords[0][1], coords[0][0]
    for lon, lat in coords[1:-1]:
        dlat = (lat - plat) * 111320.0
        dlon = (lon - plon) * 111320.0 * max(0.2, math.cos(math.radians(lat)))
        if dlat * dlat + dlon * dlon >= min_m * min_m:
            keep.append([lon, lat])
            plat, plon = lat, lon
    keep.append(coords[-1])
    return keep


def classify_way(tags: dict[str, str], skip_stream: bool = False) -> tuple[str, str] | None:
    hw = tags.get("highway")
    if hw in HIGHWAY_CLASS:
        return "road", HIGHWAY_CLASS[hw]
    natural = tags.get("natural")
    landuse = tags.get("landuse")
    waterway = tags.get("waterway")
    if natural in WATER_NATURAL or landuse in WATER_LANDUSE or waterway in WATERWAY_POLY:
        return "water", "poly"
    if tags.get("water") and natural != "coastline":
        return "water", "poly"
    if waterway in WATERWAY_LINE:
        if skip_stream and waterway == "stream":
            return None
        return "water", "line"
    return None


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


def collect_ways(pbf: Path, skip_stream: bool) -> tuple[list[tuple[str, str, list[int]]], set[int]]:
    ways: list[tuple[str, str, list[int]]] = []
    needed: set[int] = set()
    n = 0
    print(f"  pass 1 ways {pbf.name}", flush=True)
    for feat in iter_from_osm(pbf):
        kind = feat.get("type")
        if kind == "node":
            continue
        if kind == "relation":
            break
        if kind != "way":
            continue
        n += 1
        if n % 400000 == 0:
            print(f"    ways {n:,} kept {len(ways):,}", flush=True)
        cls = classify_way(feat.get("tag") or {}, skip_stream=skip_stream)
        if cls is None:
            continue
        nds = feat.get("nd") or []
        if len(nds) < 2:
            continue
        ways.append((cls[0], cls[1], nds))
        needed.update(nds)
    print(f"    kept {len(ways):,} ways  {len(needed):,} node ids", flush=True)
    return ways, needed


def collect_nodes(pbf: Path, needed: set[int]) -> dict[int, tuple[float, float]]:
    nodes: dict[int, tuple[float, float]] = {}
    n = 0
    print(f"  pass 2 nodes {pbf.name}", flush=True)
    for feat in iter_from_osm(pbf):
        kind = feat.get("type")
        if kind != "node":
            break
        n += 1
        if n % 2000000 == 0:
            print(f"    nodes {n:,} kept {len(nodes):,}", flush=True)
        nid = int(feat["id"])
        if nid in needed:
            nodes[nid] = (float(feat["lon"]), float(feat["lat"]))
    print(f"    stored {len(nodes):,} / {len(needed):,} nodes", flush=True)
    return nodes


def bucket_ways(
    buckets: dict[tuple[int, int], dict],
    ways: list[tuple[str, str, list[int]]],
    nodes: dict[int, tuple[float, float]],
) -> None:
    for group, klass, nds in ways:
        coords = way_coords(nds, nodes)
        if len(coords) < 2:
            continue
        coords = simplify(coords, 8.0)
        if len(coords) < 2:
            continue
        rec = {"c": klass, "g": coords}
        key = "roads" if group == "road" else "water"
        for t in tiles_along(coords):
            buckets[t][key].append(rec)


def write_tile_dir(dest: Path, buckets: dict[tuple[int, int], dict], source: str) -> dict:
    if dest.exists():
        for old in dest.glob("*.json"):
            old.unlink()
    dest.mkdir(parents=True, exist_ok=True)
    keys: list[str] = []
    n_roads = n_water = 0
    for (tx, ty), body in buckets.items():
        key = f"{tx}_{ty}"
        keys.append(key)
        n_roads += len(body["roads"])
        n_water += len(body["water"])
        (dest / f"{key}.json").write_text(
            json.dumps({"tx": tx, "ty": ty, "roads": body["roads"], "water": body["water"]},
                       separators=(",", ":")),
            encoding="utf-8",
        )
    keys.sort()
    index = {
        "format": "radar-tiles-v1",
        "tile_deg": TILE_DEG,
        "tile_m": round(TILE_DEG * 111320.0),
        "tiles": keys,
        "source": source,
        "stats": {"tiles": len(keys), "roads": n_roads, "water": n_water},
    }
    (dest / "index.json").write_text(json.dumps(index, separators=(",", ":")), encoding="utf-8")
    return index


def extract_full_tiles(pbfs: list[Path], dest: Path) -> dict:
    buckets: dict[tuple[int, int], dict] = defaultdict(lambda: {"roads": [], "water": []})
    for pbf in pbfs:
        print(f"tiling {pbf.name} ...", flush=True)
        ways, needed = collect_ways(pbf, skip_stream=True)
        nodes = collect_nodes(pbf, needed)
        bucket_ways(buckets, ways, nodes)
        del ways, needed, nodes
    staging = dest.with_name(dest.name + ".new")
    index = write_tile_dir(staging, buckets, ",".join(p.name for p in pbfs))
    if dest.exists():
        backup = dest.with_name(dest.name + ".old")
        if backup.exists():
            for old in backup.glob("*"):
                old.unlink()
            backup.rmdir()
        dest.rename(backup)
    staging.rename(dest)
    return index


def way_coords(nds: list[int], nodes: dict[int, tuple[float, float]]) -> list[list[float]]:
    out: list[list[float]] = []
    for nid in nds:
        pt = nodes.get(nid)
        if pt is None:
            if out:
                break
            continue
        lon, lat = pt
        out.append([rd(lon), rd(lat)])
    return out


def extract(pbfs: list[Path], bb: tuple[float, float, float, float]) -> dict:
    nodes: dict[int, tuple[float, float]] = {}
    roads: list[dict] = []
    water: list[dict] = []
    n_nodes = n_ways = 0

    for pbf in pbfs:
        print(f"reading {pbf.name} ...", flush=True)
        for feat in iter_from_osm(pbf):
            kind = feat.get("type")
            if kind == "node":
                n_nodes += 1
                if n_nodes % 1000000 == 0:
                    print(f"  nodes scanned {n_nodes:,}  kept {len(nodes):,}", flush=True)
                lat = float(feat["lat"])
                lon = float(feat["lon"])
                if in_bbox(lat, lon, bb):
                    nodes[int(feat["id"])] = (lon, lat)
            elif kind == "way":
                n_ways += 1
                if n_ways % 200000 == 0:
                    print(f"  ways {n_ways:,}  bbox nodes {len(nodes):,}", flush=True)
                cls = classify_way(feat.get("tag") or {})
                if cls is None:
                    continue
                coords = way_coords(feat.get("nd") or [], nodes)
                if len(coords) < 2:
                    continue
                coords = simplify(coords)
                if len(coords) < 2:
                    continue
                group, klass = cls
                rec = {"c": klass, "g": coords}
                if group == "road":
                    roads.append(rec)
                else:
                    water.append(rec)
            elif kind == "relation":
                break

    west, south, east, north = bb
    return {
        "bbox": [rd(west), rd(south), rd(east), rd(north)],
        "roads": roads,
        "water": water,
        "stats": {
            "nodes": n_nodes,
            "roads": len(roads),
            "water": len(water),
            "pbf": [p.name for p in pbfs],
        },
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lat", type=float, default=DEFAULT_LAT)
    ap.add_argument("--lon", type=float, default=DEFAULT_LON)
    ap.add_argument("--radius-deg", type=float, default=0.12,
                    help="half-size of the extract box in degrees (~13 km)")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--pbf", type=Path, nargs="*", default=None)
    ap.add_argument("--full", action="store_true",
                    help="tile entire Sul + Sudeste PBFs (no bbox). Slow, 15-40 min.")
    args = ap.parse_args()

    pbfs = args.pbf
    if not pbfs:
        def newest(prefix: str) -> Path | None:
            dated = MAPS / f"{prefix}-261114.osm.pbf"
            if dated.exists():
                return dated
            hits = sorted(MAPS.glob(f"{prefix}*.osm.pbf"))
            return hits[-1] if hits else None

        pbfs = []
        sul = newest("sul")
        sudeste = newest("sudeste")
        if sul:
            pbfs.append(sul)
        if sudeste and (args.full or args.lat > -25.5 or args.lon > -48.5):
            pbfs.append(sudeste)

    if args.full:
        missing = [p for p in pbfs if not p.exists()]
        if missing or not pbfs:
            print("missing PBF:", missing or "none listed", file=sys.stderr)
            return 1
        index = extract_full_tiles(pbfs, TILES)
        st = index["stats"]
        print(
            f"wrote {TILES}  tiles={st['tiles']} roads={st['roads']} "
            f"water={st['water']}",
            flush=True,
        )
        return 0
    missing = [p for p in pbfs if not p.exists()]
    if missing or not pbfs:
        print("missing PBF:", missing or "none listed", file=sys.stderr)
        return 1

    r = args.radius_deg
    bb = (args.lon - r, args.lat - r, args.lon + r, args.lat + r)
    pack = extract(pbfs, bb)
    pack["center"] = {"lat": rd(args.lat), "lon": rd(args.lon), "hdg": DEFAULT_HDG}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(pack, separators=(",", ":")), encoding="utf-8")
    st = pack["stats"]
    print(
        f"wrote {args.out}  roads={st['roads']} water={st['water']} "
        f"nodes={st['nodes']}  {args.out.stat().st_size / 1e6:.1f} MB",
        flush=True,
    )
    tiler = ROOT / "tools" / "build_map_tiles.py"
    if tiler.exists():
        import subprocess
        subprocess.check_call([sys.executable, str(tiler), "--extract", str(args.out)])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
