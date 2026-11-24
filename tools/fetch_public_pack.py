"""Download the public Bunda de Urso cadastro into our slim pack.

Uses only the published HTTP API. Writes our own fields, not their schema.
Data license: CC BY 4.0 — attribute Bunda de Urso on every snapshot.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

API = "https://servidor3.lucas.inf.br/openclaw/radar/api.pse"
ATTRIBUTION = (
    "Bunda de Urso — cadastro nacional de radares "
    "(https://servidor3.lucas.inf.br/openclaw/radar/)"
)
UA = "speeduntrap-pack/1.0 (offline HU pack; +CC-BY-4.0 attribution)"


def _get(params: dict, timeout: int = 120) -> dict:
    query = urllib.parse.urlencode(params)
    req = urllib.request.Request(
        API + "?" + query,
        headers={"User-Agent": UA, "Accept": "application/json"},
    )
    last_err = None
    for attempt in range(5):
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                body = resp.read()
                data = json.loads(body.decode("utf-8"))
            if data.get("sucesso") is False:
                raise RuntimeError(data.get("erro") or "API sucesso=false")
            return data, len(body)
        except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError) as err:
            last_err = err
            wait = min(30, 2 ** attempt)
            print(f"retry {attempt + 1}/5 after {wait}s: {err}", file=sys.stderr)
            time.sleep(wait)
    raise RuntimeError(f"gave up talking to API: {last_err}")


def _uniq(values):
    out = []
    for value in values:
        if value is None or value in out:
            continue
        out.append(value)
    return out


def _speed_for(limites: list, faixa_id: str | None, categoria: str):
    hits = []
    for row in limites or []:
        if row.get("categoria_veiculo") != categoria:
            continue
        if faixa_id and row.get("faixa_id") not in (None, faixa_id):
            continue
        kmh = row.get("velocidade_kmh")
        if kmh is not None:
            hits.append(kmh)
    if not hits:
        return None
    return hits[0] if len(set(hits)) == 1 else hits


def slim_point(raw: dict) -> dict:
    loc = raw.get("localizacao") or {}
    via = loc.get("via") or {}
    if not isinstance(via, dict):
        via = {"codigo": via}
    limites = raw.get("limites_velocidade") or []
    faixas = []
    for faixa in raw.get("faixas") or []:
        fid = faixa.get("id")
        faixas.append(
            {
                "id": fid,
                "sentido": faixa.get("sentido"),
                "leves_kmh": _speed_for(limites, fid, "leves"),
                "pesados_kmh": _speed_for(limites, fid, "pesados"),
            }
        )
    equipamentos = []
    for eq in raw.get("equipamentos") or []:
        equipamentos.append(
            {
                "id": eq.get("id"),
                "tipo": eq.get("tipo_radar"),
                "fabricante": eq.get("fabricante"),
                "modelo": eq.get("modelo"),
            }
        )
    lat = loc.get("latitude")
    lon = loc.get("longitude")
    return {
        "id": raw.get("id"),
        "lat": lat,
        "lon": lon,
        "has_gps": lat is not None and lon is not None,
        "uf": loc.get("uf") or raw.get("uf"),
        "municipio": loc.get("municipio"),
        "ibge": loc.get("codigo_ibge"),
        "via": via.get("codigo"),
        "km": loc.get("km"),
        "situacao": raw.get("situacao_operacional"),
        "sentidos": _uniq(f.get("sentido") for f in faixas),
        "vel_leves_kmh": _speed_for(limites, None, "leves"),
        "vel_pesados_kmh": _speed_for(limites, None, "pesados"),
        "faixas": faixas,
        "equipamentos": equipamentos,
        "tipos_radar": raw.get("tipos_radar_presentes") or [],
        "confianca": raw.get("confianca"),
        "gps_qualidade": loc.get("qualidade"),
    }


def fetch_estados() -> dict:
    data, _ = _get({"rota": "/api/v1/status/estados", "formato": "json"})
    return data.get("resultado") or {}


def download(dest: Path, pause_s: float, uf: str | None) -> dict:
    dest.mkdir(parents=True, exist_ok=True)
    points_path = dest / "points.ndjson"
    progress_path = dest / "fetch-progress.json"

    params = {
        "rota": "/api/v1/radares/busca",
        "formato": "json",
        "limite": 1000,
        "incluir_inativos": "true",
    }
    if uf:
        params["uf"] = uf.upper()

    seen = set()
    stats = {
        "pages": 0,
        "points": 0,
        "with_gps": 0,
        "bytes_downloaded": 0,
        "bytes_written": 0,
        "by_uf": {},
        "started_at": datetime.now(timezone.utc).isoformat(),
    }

    mode = "w"
    if progress_path.exists() and points_path.exists():
        prev = json.loads(progress_path.read_text(encoding="utf-8"))
        if prev.get("cursor"):
            params["cursor"] = prev["cursor"]
            stats = prev.get("stats") or stats
            with points_path.open(encoding="utf-8") as existing:
                for line in existing:
                    if line.strip():
                        seen.add(json.loads(line).get("id"))
            mode = "a"
            print(f"resume at page {stats['pages'] + 1}, {stats['points']} points already")

    with points_path.open(mode, encoding="utf-8") as out:
        while True:
            page, raw_len = _get(params)
            stats["bytes_downloaded"] += raw_len
            stats["pages"] += 1
            resultado = page.get("resultado") or {}
            pontos = resultado.get("pontos") or []
            base = page.get("base") or {}
            stats["snapshot_id"] = base.get("snapshot_id")
            stats["publicado_em"] = base.get("publicado_em")

            for raw in pontos:
                point = slim_point(raw)
                pid = point.get("id")
                if pid in seen:
                    continue
                seen.add(pid)
                line = json.dumps(point, ensure_ascii=False, separators=(",", ":"))
                out.write(line + "\n")
                stats["points"] += 1
                stats["bytes_written"] += len(line) + 1
                if point.get("has_gps"):
                    stats["with_gps"] += 1
                key = point.get("uf") or "sem_uf"
                stats["by_uf"][key] = stats["by_uf"].get(key, 0) + 1

            cursor = resultado.get("proximo_cursor")
            print(
                f"page {stats['pages']:>3}  +{len(pontos):<4}  "
                f"total {stats['points']}  "
                f"dl {stats['bytes_downloaded'] / 1_000_000:.1f} MB  "
                f"slim {stats['bytes_written'] / 1_000_000:.2f} MB",
                flush=True,
            )
            progress_path.write_text(
                json.dumps({"cursor": cursor, "stats": stats}, ensure_ascii=False, indent=2),
                encoding="utf-8",
            )
            if cursor is None:
                break
            params["cursor"] = cursor
            if pause_s:
                time.sleep(pause_s)

    stats["finished_at"] = datetime.now(timezone.utc).isoformat()
    return stats


def write_manifest(dest: Path, stats: dict, estados: dict, uf: str | None) -> None:
    manifest = {
        "attribution": ATTRIBUTION,
        "license": "CC BY 4.0 for the published compilation; respect each original source",
        "source_api": API,
        "snapshot_id": stats.get("snapshot_id"),
        "publicado_em": stats.get("publicado_em"),
        "scope": uf.upper() if uf else "BR",
        "points": stats["points"],
        "with_gps": stats["with_gps"],
        "without_gps": stats["points"] - stats["with_gps"],
        "pages": stats["pages"],
        "bytes_downloaded": stats["bytes_downloaded"],
        "bytes_written": stats["bytes_written"],
        "by_uf": dict(sorted(stats["by_uf"].items())),
        "api_status_totais": estados.get("totais"),
        "started_at": stats.get("started_at"),
        "finished_at": stats.get("finished_at"),
        "files": {"points": "points.ndjson"},
    }
    (dest / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Fetch public radar cadastro into our slim pack")
    parser.add_argument("--out", default=None, help="destination folder")
    parser.add_argument("--uf", default=None, help="optional UF filter, e.g. SP")
    parser.add_argument("--pause", type=float, default=0.4, help="seconds between pages")
    args = parser.parse_args()

    status, _ = _get({"rota": "/api/v1/status", "formato": "json"})
    snap = status.get("snapshot_id") or datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    scope = args.uf.upper() if args.uf else "BR"
    dest = Path(args.out) if args.out else Path("data") / "raw" / f"{scope}-{snap}"
    dest = dest.resolve()

    print(f"snapshot {snap}  declared points {status.get('total_pontos')}")
    print(f"writing {dest}")
    estados = fetch_estados()
    stats = download(dest, args.pause, args.uf)
    write_manifest(dest, stats, estados, args.uf)
    print(
        f"done  points={stats['points']}  gps={stats['with_gps']}  "
        f"slim={stats['bytes_written'] / 1_000_000:.2f} MB  "
        f"downloaded={stats['bytes_downloaded'] / 1_000_000:.1f} MB"
    )
    print(f"manifest {dest / 'manifest.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
