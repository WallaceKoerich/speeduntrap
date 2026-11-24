#!/usr/bin/env python3
"""Open the 1280x480 CID radar emulator in a local browser."""
from __future__ import annotations

import json
import re
import subprocess
import sys
import threading
import webbrowser
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
PACK = ROOT / "hu" / "radar" / "points.txt"
FIX = ROOT / "data" / "local" / "last_fix.json"
EXTRACT = ROOT / "data" / "maps" / "pc_extract.json"
TILES = ROOT / "data" / "maps" / "tiles"
EXTRACTOR = ROOT / "tools" / "extract_osm_geojson.py"
TILER = ROOT / "tools" / "build_map_tiles.py"

HOST = "127.0.0.1"
PORT = 8765
TILE_NAME = re.compile(r"^-?\d+_-?\d+$")

_extract_lock = threading.Lock()
_extract_proc: subprocess.Popen | None = None


def load_fix() -> dict:
    if FIX.exists():
        return json.loads(FIX.read_text(encoding="utf-8"))
    return {"lat": -26.961387, "lon": -50.422295, "hdg": 27.9, "src": "PosReal.DNAVIPos"}


def load_points() -> tuple[list[dict], int]:
    pts: list[dict] = []
    test_idx = -1
    if not PACK.exists():
        return pts, test_idx
    for line in PACK.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        tok = line.split()
        if len(tok) < 8:
            continue
        rec = {
            "lat": float(tok[0]),
            "lon": float(tok[1]),
            "leves": int(float(tok[2])),
            "pesados": int(float(tok[3])),
            "heading": int(float(tok[4])),
            "via": tok[5],
            "fab": tok[6],
            "model": tok[7],
        }
        if rec["via"] == "TEST" or (abs(rec["lat"] + 26.962599) < 1e-6 and
                                    abs(rec["lon"] + 50.421094) < 1e-6):
            test_idx = len(pts)
        pts.append(rec)
    return pts, test_idx


def tiles_ready() -> bool:
    return (TILES / "index.json").exists()


def ensure_tiles(fix: dict) -> None:
    global _extract_proc
    if tiles_ready():
        return
    with _extract_lock:
        if tiles_ready():
            return
        if _extract_proc is not None and _extract_proc.poll() is None:
            return
        if EXTRACT.exists() and EXTRACT.stat().st_size > 1000:
            print("slicing map extract into ~2 km tiles...", flush=True)
            _extract_proc = subprocess.Popen(
                [sys.executable, str(TILER)], cwd=str(ROOT)
            )
            return
        print("OSM extract missing — starting Sul PBF cut...", flush=True)
        _extract_proc = subprocess.Popen(
            [
                sys.executable,
                str(EXTRACTOR),
                "--lat",
                str(fix["lat"]),
                "--lon",
                str(fix["lon"]),
            ],
            cwd=str(ROOT),
        )


def load_tile_index() -> dict | None:
    path = TILES / "index.json"
    if not path.exists():
        return None
    return json.loads(path.read_text(encoding="utf-8"))


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(HERE), **kwargs)

    def log_message(self, fmt: str, *args) -> None:
        sys.stdout.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _json(self, payload: object, code: int = 200) -> None:
        data = json.dumps(payload).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _file_json(self, path: Path, code: int = 200) -> None:
        data = path.read_bytes()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self) -> None:
        if self.path.startswith("/api/boot"):
            fix = load_fix()
            pts, test_idx = load_points()
            index = load_tile_index()
            if index is None:
                ensure_tiles(fix)
            self._json({"fix": fix, "points": pts, "test_idx": test_idx, "tiles": index})
            return
        if self.path.startswith("/api/tiles"):
            index = load_tile_index()
            if index is None:
                ensure_tiles(load_fix())
                self._json({"pending": True}, 202)
                return
            self._json(index)
            return
        if self.path.startswith("/api/tile/"):
            key = self.path.split("/api/tile/", 1)[1].split("?", 1)[0]
            if key.endswith(".json"):
                key = key[:-5]
            if not TILE_NAME.match(key):
                self._json({"error": "bad tile"}, 400)
                return
            path = TILES / f"{key}.json"
            if not path.exists():
                self._json({"tx": 0, "ty": 0, "roads": [], "water": []}, 200)
                return
            self._file_json(path)
            return
        super().do_GET()


def main() -> int:
    fix = load_fix()
    ensure_tiles(fix)
    httpd = ThreadingHTTPServer((HOST, PORT), Handler)
    url = f"http://{HOST}:{PORT}/"
    print(f"CID emulator {url}  start {fix['lat']}, {fix['lon']} hdg {fix['hdg']}", flush=True)
    threading.Timer(0.6, lambda: webbrowser.open(url)).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
