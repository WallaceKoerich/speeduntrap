"""Merge local TEST controls into a slim snapshot without touching official IDs."""

from __future__ import annotations

import argparse
import json
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONTROLS = ROOT / "data" / "local" / "test-controls.ndjson"


def _load_ndjson(path: Path) -> list[dict]:
    rows = []
    if not path.exists():
        return rows
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.strip():
            rows.append(json.loads(line))
    return rows


def apply(snapshot: Path) -> dict:
    points_path = snapshot / "points.ndjson"
    manifest_path = snapshot / "manifest.json"
    if not points_path.exists():
        raise FileNotFoundError(points_path)

    controls = _load_ndjson(CONTROLS)
    existing = _load_ndjson(points_path)
    known = {row.get("id") for row in existing}
    added = []
    with points_path.open("a", encoding="utf-8") as out:
        for row in controls:
            pid = row.get("id")
            if pid in known:
                continue
            if row.get("origin") != "test_control":
                raise ValueError(f"{pid} is missing origin=test_control")
            out.write(json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n")
            added.append(pid)
            known.add(pid)

    manifest = {}
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["test_controls"] = {
        "file": str(CONTROLS.relative_to(ROOT)).replace("\\", "/"),
        "ids": [row.get("id") for row in controls],
        "added_this_run": added,
        "applied_at": datetime.now(timezone.utc).isoformat(),
    }
    if added:
        manifest["points"] = int(manifest.get("points") or len(existing)) + len(added)
        gps_added = sum(1 for row in controls if row.get("id") in added and row.get("has_gps"))
        manifest["with_gps"] = int(manifest.get("with_gps") or 0) + gps_added
        by_uf = manifest.setdefault("by_uf", {})
        for row in controls:
            if row.get("id") in added:
                key = row.get("uf") or "TEST"
                by_uf[key] = int(by_uf.get(key) or 0) + 1
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return {"added": added, "already": [row.get("id") for row in controls if row.get("id") not in added]}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--snapshot", required=True, help="Folder that contains points.ndjson")
    args = parser.parse_args()
    result = apply(Path(args.snapshot))
    print(f"added {result['added'] or 'none'}; already present {result['already'] or 'none'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
