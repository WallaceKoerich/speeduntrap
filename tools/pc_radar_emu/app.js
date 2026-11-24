/* PC stand-in for the NBT CID overlay. Detection matches poc/id4_radar_core.c. */
(() => {
  const W = 1280;
  const H = 480;
  const PASS_M = 80;
  const SPEED_KMH = 4000;
  const TURN_DPS = 90;
  const RANGES = [50, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000];

  /* Snazzy Maps Orange (style 145431) — same RGB as poc/speeduntrap_ui.c */
  const MC = {
    land: "#282828",
    water: "#161616",
    poi: "#2a2a2a",
    admin: "#404040",
    hw: "#f26c4f",
    ar: "#606060",
    lo: "#4a4a4a",
    transit: "#404040",
    label: "#ffffff",
  };
  const HUD = {
    bg: "rgba(16, 12, 10, 0.72)",
    hot: "#f26c4f",
    white: "#f0f0f0",
    grey: "#b0b0b0",
  };

  const mapCv = document.getElementById("map");
  const hudCv = document.getElementById("hud");
  const mapCtx = mapCv.getContext("2d");
  const hudCtx = hudCv.getContext("2d");
  const statusEl = document.getElementById("status");

  const keys = Object.create(null);
  const car = { lat: -26.961387, lon: -50.422295, hdg: 27.9, speed: 0 };
  const det = {
    popup: 1,
    rangeI: 4,
    enterM: 500,
    alert: 0,
    userMin: 0,
    demo: 0,
    hit: -1,
    closest: 1e9,
    prev: -1,
    showing: 0,
    optN: 0,
    optMs: 0,
    status: "Ready",
  };

  let points = [];
  let roads = [];
  let water = [];
  let testIdx = -1;
  let mapReady = false;
  let metersWide = 1600;
  let lastT = 0;
  let tileDeg = 0.02;
  const LOAD_RING = 8;
  const DROP_RING = 12;
  const tileHave = new Set();
  const tileCache = new Map();
  const tilePending = new Set();
  let lastTileKey = "";

  function haversine(lat1, lon1, lat2, lon2) {
    const p1 = lat1 * Math.PI / 180;
    const p2 = lat2 * Math.PI / 180;
    const dp = (lat2 - lat1) * Math.PI / 180;
    const dl = (lon2 - lon1) * Math.PI / 180;
    const a = Math.sin(dp / 2) ** 2 +
              Math.cos(p1) * Math.cos(p2) * Math.sin(dl / 2) ** 2;
    return 2 * 6371000 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
  }

  function headingOk(p) {
    if (p.heading < 0) return 1;
    let d = Math.abs(car.hdg - p.heading);
    while (d > 180) d = 360 - d;
    return d <= 90 ? 1 : 0;
  }

  function merc(lat, lon) {
    const x = (lon + 180) / 360;
    const s = Math.sin(lat * Math.PI / 180);
    const y = 0.5 - Math.log((1 + s) / (1 - s)) / (4 * Math.PI);
    return [x, y];
  }

  function toScreen(lat, lon) {
    const [cx, cy] = merc(car.lat, car.lon);
    const [x, y] = merc(lat, lon);
    const mpp = metersWide / W;
    const latRad = car.lat * Math.PI / 180;
    const mPerX = 40075016.686 * Math.cos(latRad);
    const px = W / 2 + ((x - cx) * mPerX) / mpp;
    const py = H / 2 + ((y - cy) * 40075016.686) / mpp;
    return [px, py];
  }

  function findNearest() {
    let best = -1;
    let bestd = 1e9;
    for (let i = 0; i < points.length; i++) {
      if (!headingOk(points[i])) continue;
      const d = haversine(car.lat, car.lon, points[i].lat, points[i].lon);
      if (d < bestd) {
        bestd = d;
        best = i;
      }
    }
    return { best, dist: bestd };
  }

  function overlayShow() {
    det.showing = 1;
  }

  function overlayHide() {
    det.showing = 0;
  }

  function endAlert() {
    overlayHide();
    det.alert = 0;
    det.userMin = 0;
    det.demo = 0;
    det.hit = -1;
    det.closest = 1e9;
    det.prev = -1;
  }

  function tickDetect() {
    const leaveM = det.enterM + 100;
    const rearmM = det.enterM + 150;
    let { best, dist } = findNearest();

    if (det.demo && !det.userMin) {
      if (best >= 0) {
        overlayShow();
        det.status = `Demo ${dist.toFixed(0)} m`;
      } else det.status = "Demo, no GPS";
      return { best, dist };
    }

    if (best < 0) {
      det.status = "No traps in heading";
      if (det.alert) endAlert();
      return { best, dist };
    }

    if (!det.alert) {
      if (dist <= det.enterM) {
        det.alert = 1;
        det.userMin = 0;
        det.hit = best;
        det.closest = dist;
        det.prev = dist;
        overlayShow();
      }
    } else {
      if (best !== det.hit)
        dist = haversine(car.lat, car.lon, points[det.hit].lat, points[det.hit].lon);
      if (dist < det.closest) det.closest = dist;
      if (dist > PASS_M && dist > det.closest + 40 && det.closest < PASS_M) {
        endAlert();
      } else if (dist > leaveM) {
        endAlert();
      } else if (!det.userMin) {
        overlayShow();
      }
      det.prev = dist;
    }

    if (det.alert && det.hit >= 0) {
      det.status = det.userMin ? `Trap ${dist.toFixed(0)} m (hidden)` : `Trap ${dist.toFixed(0)} m`;
    } else if (dist < rearmM) {
      det.status = `Near ${dist.toFixed(0)} m`;
    } else {
      det.status = "Popup on";
    }
    return { best, dist };
  }

  function drawChar(ctx, x, y, ch, color, s) {
    if (ch >= "a" && ch <= "z") ch = String.fromCharCode(ch.charCodeAt(0) - 32);
    let code = ch.charCodeAt(0);
    if (code < 32 || code > 90) code = 32;
    const g = FONT5[code - 32];
    ctx.fillStyle = color;
    for (let col = 0; col < 5; col++) {
      const bits = g[col];
      for (let row = 0; row < 7; row++) {
        if (bits & (1 << row))
          ctx.fillRect(x + col * s, y + row * s, s, s);
      }
    }
  }

  function drawText(ctx, x, y, s, color, scale) {
    let cx = x;
    for (let i = 0; i < s.length; i++) {
      if (s[i] === "\n") {
        y += 8 * scale + 4;
        cx = x;
        continue;
      }
      drawChar(ctx, cx, y, s[i], color, scale);
      cx += 6 * scale;
    }
  }

  function strokePoly(ctx, geom, close) {
    if (!geom || geom.length < 2) return;
    let minx = Infinity;
    let miny = Infinity;
    let maxx = -Infinity;
    let maxy = -Infinity;
    const xs = new Array(geom.length);
    const ys = new Array(geom.length);
    for (let i = 0; i < geom.length; i++) {
      const [px, py] = toScreen(geom[i][1], geom[i][0]);
      xs[i] = px;
      ys[i] = py;
      if (px < minx) minx = px;
      if (py < miny) miny = py;
      if (px > maxx) maxx = px;
      if (py > maxy) maxy = py;
    }
    if (maxx < -8 || maxy < -8 || minx > W + 8 || miny > H + 8)
      return;
    ctx.beginPath();
    ctx.moveTo(xs[0], ys[0]);
    for (let i = 1; i < geom.length; i++)
      ctx.lineTo(xs[i], ys[i]);
    if (close)
      ctx.closePath();
    ctx.stroke();
  }

  function fillPoly(ctx, geom) {
    if (!geom || geom.length < 3) return;
    ctx.beginPath();
    for (let i = 0; i < geom.length; i++) {
      const [px, py] = toScreen(geom[i][1], geom[i][0]);
      if (i === 0) ctx.moveTo(px, py);
      else ctx.lineTo(px, py);
    }
    ctx.closePath();
    ctx.fill();
  }

  function visibleTiles() {
    const out = [];
    const [cx, cy] = tileOf(car.lat, car.lon);
    const tileM = tileDeg * 111320;
    const r = Math.max(1, Math.ceil((metersWide / 2) / tileM) + 1);
    for (let ty = cy - r; ty <= cy + r; ty++) {
      for (let tx = cx - r; tx <= cx + r; tx++) {
        const t = tileCache.get(tileKey(tx, ty));
        if (t) out.push(t);
      }
    }
    return out;
  }

  function drawMap(hit) {
    const ctx = mapCtx;
    ctx.fillStyle = MC.land;
    ctx.fillRect(0, 0, W, H);
    const vis = visibleTiles();

    ctx.fillStyle = MC.water;
    ctx.strokeStyle = MC.water;
    ctx.lineWidth = 2;
    for (let t = 0; t < vis.length; t++) {
      const waterList = vis[t].water;
      for (let i = 0; i < waterList.length; i++) {
        const w = waterList[i];
        if (w.c === "poly") fillPoly(ctx, w.g);
        else {
          ctx.strokeStyle = MC.water;
          strokePoly(ctx, w.g, false);
        }
      }
    }

    const styles = {
      lo: { color: MC.lo, w: 1.6, caseW: 3.2 },
      ar: { color: MC.ar, w: 2.6, caseW: 4.6 },
      hw: { color: MC.hw, w: 4.0, caseW: 6.5 },
    };
    ctx.lineJoin = "round";
    ctx.lineCap = "round";
    for (const k of ["lo", "ar", "hw"]) {
      const st = styles[k];
      ctx.strokeStyle = "#000000";
      ctx.lineWidth = st.caseW;
      for (let t = 0; t < vis.length; t++) {
        const list = vis[t].roads;
        for (let i = 0; i < list.length; i++) {
          if (list[i].c === k) strokePoly(ctx, list[i].g, false);
        }
      }
    }
    for (const k of ["lo", "ar", "hw"]) {
      const st = styles[k];
      ctx.strokeStyle = st.color;
      ctx.lineWidth = st.w;
      for (let t = 0; t < vis.length; t++) {
        const list = vis[t].roads;
        for (let i = 0; i < list.length; i++) {
          if (list[i].c === k) strokePoly(ctx, list[i].g, false);
        }
      }
    }

    const [cx, cy] = [W / 2, H / 2];
    ctx.beginPath();
    ctx.strokeStyle = MC.transit;
    ctx.lineWidth = 1;
    ctx.setLineDash([6, 6]);
    const [rx, ry] = toScreen(
      car.lat + (det.enterM / 111320),
      car.lon
    );
    const rpx = Math.abs(ry - cy);
    ctx.arc(cx, cy, rpx, 0, Math.PI * 2);
    ctx.stroke();
    ctx.setLineDash([]);

    const visM = metersWide * 0.7;
    for (let i = 0; i < points.length; i++) {
      const p = points[i];
      const d = haversine(car.lat, car.lon, p.lat, p.lon);
      if (d > visM) continue;
      const [x, y] = toScreen(p.lat, p.lon);
      const active = det.alert && i === det.hit;
      ctx.beginPath();
      ctx.fillStyle = active ? HUD.hot : (i === testIdx ? "#f26c4f" : "#c85844");
      ctx.strokeStyle = active ? "#000000" : MC.admin;
      ctx.lineWidth = 1;
      ctx.arc(x, y, active ? 7 : 4, 0, Math.PI * 2);
      ctx.fill();
      ctx.stroke();
    }

    const rad = car.hdg * Math.PI / 180;
    ctx.save();
    ctx.translate(cx, cy);
    ctx.rotate(rad);
    ctx.beginPath();
    ctx.moveTo(0, -22);
    ctx.lineTo(14, 16);
    ctx.lineTo(0, 8);
    ctx.lineTo(-14, 16);
    ctx.closePath();
    ctx.fillStyle = "#f26c4f";
    ctx.strokeStyle = "#000000";
    ctx.lineWidth = 1.5;
    ctx.fill();
    ctx.stroke();
    ctx.restore();
  }

  function drawHud(hit, dist) {
    const ctx = hudCtx;
    ctx.clearRect(0, 0, W, H);

    if (!det.showing) {
      drawText(ctx, 24, 444, det.status.toUpperCase(), HUD.grey, 2);
      return;
    }
    if (hit < 0) return;
    const p = points[hit];
    const via = p.via && p.via !== "-" ? p.via : "-";
    const brand = p.fab && p.fab !== "-" ? p.fab : "-";
    const model = p.model && p.model !== "-" ? p.model : "-";
    const menuY = 444;
    const lh = 7 * 2;
    const titleH = 7 * 4;
    const distH = 7 * 6;
    const numH = 7 * 4;
    const txtH = 7 * 3;
    const inn = 6;
    const out = 16;
    const blockH = titleH + inn + distH + out +
                   lh + inn + numH + out +
                   lh + inn + numH + out +
                   lh + inn + txtH + out +
                   lh + inn + txtH + inn + txtH;
    let y = Math.floor((menuY - blockH) / 2);
    const x = 20;
    const over = car.speed > p.leves;
    const speedCol = over ? HUD.hot : HUD.grey;
    ctx.fillStyle = HUD.bg;
    ctx.fillRect(0, 0, 420, H);
    drawText(ctx, x, y, "SPEED TRAP", HUD.hot, 4); y += titleH + inn;
    drawText(ctx, x, y, dist >= 0 ? `${dist.toFixed(0)} m` : "-- m", HUD.grey, 6); y += distH + out;
    drawText(ctx, x, y, "SPEED LIMIT", HUD.grey, 2); y += lh + inn;
    drawText(ctx, x, y, `${p.leves} km/h`, HUD.hot, 4); y += numH + out;
    drawText(ctx, x, y, "SPEED", HUD.grey, 2); y += lh + inn;
    drawText(ctx, x, y, `${car.speed.toFixed(0)} km/h`, speedCol, 4); y += numH + out;
    drawText(ctx, x, y, "ROAD", HUD.grey, 2); y += lh + inn;
    drawText(ctx, x, y, via, HUD.grey, 3); y += txtH + out;
    drawText(ctx, x, y, "RADAR", HUD.grey, 2); y += lh + inn;
    drawText(ctx, x, y, brand, HUD.grey, 3); y += txtH + inn;
    drawText(ctx, x, y, model, HUD.grey, 3);
    drawText(ctx, x, menuY, "MENU hide   OPTION x3 show", HUD.grey, 2);
  }

  function optionTap() {
    const t = performance.now();
    if (det.optMs === 0 || t - det.optMs > 2000) det.optN = 0;
    det.optMs = t;
    det.optN++;
    if (det.optN >= 3) {
      det.optN = 0;
      if (det.popup) {
        det.userMin = 0;
        det.demo = 1;
      }
    }
  }

  function setRange(i) {
    det.rangeI = Math.max(0, Math.min(RANGES.length - 1, i));
    det.enterM = RANGES[det.rangeI];
  }

  function physics(dt) {
    let turn = 0;
    if (keys.a || keys.ArrowLeft) turn -= 1;
    if (keys.d || keys.ArrowRight) turn += 1;
    car.hdg = (car.hdg + turn * TURN_DPS * dt + 360) % 360;

    let dir = 0;
    if (keys.w || keys.ArrowUp) dir += 1;
    if (keys.s || keys.ArrowDown) dir -= 1;
    car.speed = dir !== 0 ? SPEED_KMH : 0;
    if (dir === 0) return;
    const m = (SPEED_KMH / 3.6) * dt * dir;
    const rad = car.hdg * Math.PI / 180;
    const dlat = (Math.cos(rad) * m) / 111320;
    const dlon = (Math.sin(rad) * m) / (111320 * Math.cos(car.lat * Math.PI / 180));
    car.lat += dlat;
    car.lon += dlon;
  }

  function frame(t) {
    const dt = lastT ? Math.min(0.05, (t - lastT) / 1000) : 0;
    lastT = t;
    physics(dt);
    syncTiles();
    const { best, dist } = tickDetect();
    const hit = det.alert && det.hit >= 0 ? det.hit : best;
    drawMap(hit);
    drawHud(det.showing ? hit : -1, dist);
    const mapNote = mapReady
      ? `tiles ${tileCache.size} roads ${roads.length}`
      : "map tiles";
    statusEl.textContent =
      `${det.status} · ${car.lat.toFixed(6)}, ${car.lon.toFixed(6)} · ` +
      `hdg ${car.hdg.toFixed(0)} · range ${det.enterM} m · ${mapNote}`;
    requestAnimationFrame(frame);
  }

  const cid = document.getElementById("cid");
  cid.addEventListener("click", () => cid.focus());
  window.addEventListener("load", () => cid.focus());

  window.addEventListener("keydown", (e) => {
    if (document.activeElement !== cid && e.target !== cid) {
      cid.focus();
    }
    const k = e.key.length === 1 ? e.key.toLowerCase() : e.key;
    keys[k] = 1;
    if (k === "m") {
      if (det.showing) {
        overlayHide();
        det.userMin = 1;
        det.demo = 0;
      }
    } else if (k === "o") {
      optionTap();
    } else if (k === "+" || k === "=") {
      setRange(det.rangeI + 1);
    } else if (k === "-" || k === "_") {
      setRange(det.rangeI - 1);
    }
    if (["w", "a", "s", "d", "ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", " "].includes(e.key))
      e.preventDefault();
  });
  window.addEventListener("keyup", (e) => {
    const k = e.key.length === 1 ? e.key.toLowerCase() : e.key;
    keys[k] = 0;
  });
  document.getElementById("cid").addEventListener("wheel", (e) => {
    e.preventDefault();
    const f = e.deltaY > 0 ? 1.12 : 1 / 1.12;
    metersWide = Math.max(400, Math.min(8000, metersWide * f));
  }, { passive: false });

  async function loadJson(url) {
    const r = await fetch(url);
    if (!r.ok) throw new Error(url);
    return r.json();
  }

  function tileKey(tx, ty) {
    return `${tx}_${ty}`;
  }

  function tileOf(lat, lon) {
    return [Math.floor(lon / tileDeg), Math.floor(lat / tileDeg)];
  }

  function rebuildGeom() {
    const r = [];
    const w = [];
    tileCache.forEach((tile) => {
      for (let i = 0; i < tile.roads.length; i++) r.push(tile.roads[i]);
      for (let i = 0; i < tile.water.length; i++) w.push(tile.water[i]);
    });
    roads = r;
    water = w;
  }

  function dropFarTiles(cx, cy) {
    const drop = [];
    tileCache.forEach((_tile, key) => {
      const parts = key.split("_");
      const tx = +parts[0];
      const ty = +parts[1];
      if (Math.abs(tx - cx) > DROP_RING || Math.abs(ty - cy) > DROP_RING)
        drop.push(key);
    });
    if (!drop.length) return;
    for (let i = 0; i < drop.length; i++) tileCache.delete(drop[i]);
    rebuildGeom();
  }

  function fetchTile(key) {
    if (tilePending.has(key) || tileCache.has(key)) return;
    if (tileHave.size && !tileHave.has(key)) return;
    tilePending.add(key);
    fetch(`/api/tile/${key}`)
      .then((r) => (r.ok ? r.json() : null))
      .then((body) => {
        tilePending.delete(key);
        if (!body) return;
        tileCache.set(key, { roads: body.roads || [], water: body.water || [] });
        mapReady = true;
        rebuildGeom();
      })
      .catch(() => tilePending.delete(key));
  }

  function loadRing() {
    const tileM = tileDeg * 111320;
    const viewTiles = Math.ceil((metersWide / 2) / tileM) + 2;
    const lookTiles = Math.ceil(((car.speed / 3.6) * 8) / tileM) + 2;
    return Math.max(LOAD_RING, viewTiles, lookTiles);
  }

  function syncTiles() {
    const [cx, cy] = tileOf(car.lat, car.lon);
    const ring = loadRing();
    const rad = car.hdg * Math.PI / 180;
    const lead = car.speed > 0 ? Math.max(2, Math.ceil(ring * 0.45)) : 0;
    const ox = cx + Math.round(Math.sin(rad) * lead);
    const oy = cy + Math.round(Math.cos(rad) * lead);
    const here = `${cx}_${cy}_${ox}_${oy}_${ring}_${Math.round(metersWide)}`;
    if (here === lastTileKey && tileCache.size) return;
    lastTileKey = here;
    const x0 = Math.min(cx, ox) - ring;
    const x1 = Math.max(cx, ox) + ring;
    const y0 = Math.min(cy, oy) - ring;
    const y1 = Math.max(cy, oy) + ring;
    for (let ty = y0; ty <= y1; ty++) {
      for (let tx = x0; tx <= x1; tx++) {
        fetchTile(tileKey(tx, ty));
      }
    }
    dropFarTiles(cx, cy);
  }

  async function boot() {
    const boot = await loadJson("/api/boot");
    car.lat = boot.fix.lat;
    car.lon = boot.fix.lon;
    car.hdg = boot.fix.hdg;
    points = boot.points;
    testIdx = boot.test_idx;
    if (boot.tiles) {
      tileDeg = boot.tiles.tile_deg || tileDeg;
      (boot.tiles.tiles || []).forEach((k) => tileHave.add(k));
    } else {
      pollTiles();
    }
    syncTiles();
    requestAnimationFrame(frame);
  }

  async function pollTiles() {
    try {
      const idx = await loadJson("/api/tiles");
      if (idx && idx.tiles) {
        tileDeg = idx.tile_deg || tileDeg;
        idx.tiles.forEach((k) => tileHave.add(k));
        lastTileKey = "";
        syncTiles();
        return;
      }
    } catch (_) { /* still slicing */ }
    setTimeout(pollTiles, 1500);
  }

  window.EMU = { keys, car, det };
  boot().catch((err) => {
    statusEl.textContent = String(err);
  });
})();
