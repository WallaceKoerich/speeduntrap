#!/usr/bin/env python3
"""Export a QNX-friendly trap list from a slim snapshot + TEST controls.

Line format (spaces, no JSON):
  lat lon leves pesados heading via fabricante modelo

heading is -1 when unknown. Points without GPS are dropped.
Copy the result to the HU as /fs/sda0/radar/points.txt
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONTROLS = ROOT / "data" / "local" / "test-controls.ndjson"
PACKS = ROOT / "data" / "packs"

SAFE = re.compile(r"[^A-Za-z0-9._+-]+")


def token(value: object, fallback: str = "-") -> str:
    if value is None:
        return fallback
    s = str(value).strip()
    if not s:
        return fallback
    s = SAFE.sub("_", s).strip("_")
    return s[:23] or fallback


def heading_of(row: dict) -> int:
    for key in ("heading", "rumo", "azimute"):
        v = row.get(key)
        if isinstance(v, (int, float)) and 0 <= float(v) <= 360:
            return int(round(float(v))) % 360
    sentidos = row.get("sentidos") or []
    if isinstance(sentidos, str):
        sentidos = [sentidos]
    text = " ".join(str(s) for s in sentidos).upper()
    if "NORTE" in text or text == "N":
        return 0
    if "LESTE" in text or text == "L" or text == "E":
        return 90
    if "SUL" in text or text == "S":
        return 180
    if "OESTE" in text or text == "O" or text == "W":
        return 270
    return -1


def row_to_line(row: dict) -> str | None:
    lat = row.get("lat")
    lon = row.get("lon")
    if lat is None or lon is None:
        return None
    try:
        lat_f = float(lat)
        lon_f = float(lon)
    except (TypeError, ValueError):
        return None
    leves = row.get("vel_leves_kmh")
    pesados = row.get("vel_pesados_kmh")
    try:
        leves_i = int(leves) if leves is not None else 0
    except (TypeError, ValueError):
        leves_i = 0
    try:
        pesados_i = int(pesados) if pesados is not None else 0
    except (TypeError, ValueError):
        pesados_i = 0
    fab = "-"
    model = "-"
    eqs = row.get("equipamentos") or []
    if eqs and isinstance(eqs, list) and isinstance(eqs[0], dict):
        fab = token(eqs[0].get("fabricante"))
        model = token(eqs[0].get("modelo"))
    via = token(row.get("via"))
    return f"{lat_f:.6f} {lon_f:.6f} {leves_i} {pesados_i} {heading_of(row)} {via} {fab} {model}"


def load_ndjson(path: Path) -> list[dict]:
    rows = []
    if not path.exists():
        return rows
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.strip():
            rows.append(json.loads(line))
    return rows


def latest_snapshot() -> Path:
    raw = ROOT / "data" / "raw"
    cands = sorted(raw.glob("*/points.ndjson"), key=lambda p: p.stat().st_mtime, reverse=True)
    if not cands:
        raise FileNotFoundError(f"no snapshot under {raw}")
    return cands[0].parent


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--snapshot", help="Folder that contains points.ndjson")
    parser.add_argument("--out", help="Output points.txt")
    args = parser.parse_args()
    snap = Path(args.snapshot) if args.snapshot else latest_snapshot()
    out = Path(args.out) if args.out else ROOT / "hu" / "radar" / "points.txt"
    rows = load_ndjson(snap / "points.ndjson")
    known = {r.get("id") for r in rows}
    for row in load_ndjson(CONTROLS):
        if row.get("id") not in known:
            rows.append(row)
    lines = ["# lat lon leves pesados heading via fabricante modelo"]
    kept = 0
    for row in rows:
        line = row_to_line(row)
        if line:
            lines.append(line)
            kept += 1
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {out} points={kept} from {snap}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
