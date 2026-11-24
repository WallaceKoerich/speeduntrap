# speeduntrap

Offline speed-trap warning for **BMW NBT iDrive 4** (QNX 6.5.0 x86, F30-class
head unit). It draws its own CID overlay — map + HUD — when you get within the
warning range of a packed radar. It does **not** patch HmiMain3D and does **not**
inject Check Control messages.

You copy three things onto the USB stick (`/fs/sda0`): the binary, the trap list,
the map pack, and a short chime. Updates are those same files again. Nothing
talks to the network in the car.

## License

**PolyForm Noncommercial 1.0.0** — see [LICENSE](LICENSE).

You may use this in your own car, study it, modify it, and send pull requests.
You may **not sell** it, sell a build of it, or use it for a commercial purpose.

This is source-available software with a no-sale rule. OSI “Open Source”
licenses allow selling; this one does not. That is intentional.

Trap coordinates in `points.txt` are **CC BY 4.0** from [Bunda de Urso](https://servidor3.lucas.inf.br/openclaw/radar/).
Roads in `map.bin` are **© OpenStreetMap contributors (ODbL)**. See [NOTICE](NOTICE).

Bunda de Urso’s **code** is not open source. This repo does not contain their
collector, schema, or website. We only consume the published API export.

---

## What you need

- A BMW NBT iDrive 4 head unit you can SSH into (same USB / Open HU path used
  for other CID apps).
- This folder on a PC (clone, or the copy on your Desktop).
- Python 3.10+ **on the PC only**, when you refresh traps or rebuild the map.

The prebuilt `hu/speeduntrap` is what you install. Rebuilding the binary is
optional and needs the QNX 6.5.0 x86 toolchain plus HU shared libraries.

---

## 1. Install on the car (once)

Everything for the USB stick lives in **`hu/`**.

| This folder | On the head unit |
| --- | --- |
| `hu/speeduntrap` | `/fs/sda0/speeduntrap` |
| `hu/radar/points.txt` | `/fs/sda0/radar/points.txt` |
| `hu/radar/chime.wav` | `/fs/sda0/radar/chime.wav` |
| `hu/radar/map.bin` | `/fs/sda0/radar/map.bin` |
| `hu/start_speeduntrap.sh` | `/fs/sda0/start_speeduntrap.sh` |
| `hu/install_boot.sh` | `/fs/sda0/install_boot.sh` (once) |
| `hu/rollback_boot.sh` | `/fs/sda0/rollback_boot.sh` (keep it) |

`map.bin` is about **1.1 GB**. GitHub will not host it in the git tree. If this
Desktop folder still has `hu/radar/map.bin`, copy that. If you cloned from
GitHub and the file is missing, [rebuild the map](#3-update-the-map-tiles) first.

Scripts **must be Unix LF**. A Windows CRLF copy makes QNX say
`No such file or directory` even though `ls` shows the file.

SSH:

```sh
mount -uw /fs/sda0
mkdir -p /fs/sda0/radar
# copy the files from USB / SCP into those paths, then:
chmod 775 /fs/sda0/speeduntrap /fs/sda0/start_speeduntrap.sh /fs/sda0/install_boot.sh /fs/sda0/rollback_boot.sh
```

### Run it now (no reboot)

```sh
slay speeduntrap
/fs/sda0/speeduntrap
```

Or `/fs/sda0/speeduntrap --test` for Putty logs without the ConnectedDrive page.

If you already have the ConnectedDrive **Launcher**, you can start it from
**Launcher → speeduntrap** instead of SSH. The Radar settings page (Popup,
Range) is registered by this binary.

### Start at every boot (optional)

```sh
/fs/sda0/install_boot.sh
shutdown -S reboot
```

That edits the **flash** starter file (`starter.cfg.0x10ee` or `.0x1c42`), not
the OEM copy under `/fs/sda0/repository/istep/`. It adds `start_speeduntrap.sh`
as a HostsProcess next to HmiMain3D. It does **not** kill HmiMain3D. A crash
uses `OnTerminate IGNORE_ALL` / `FinalAction IGNORE` — it must not reboot the HU.

If the installer says `/fs/sda0` is read-only, remount and run it again. If it
says the cfg looks broken, **do not reboot**; restore with `rollback_boot.sh`.

Boot log: `/fs/sda0/speeduntrap_boot.log`.

### Undo boot

```sh
chmod 775 /fs/sda0/rollback_boot.sh
/fs/sda0/rollback_boot.sh
shutdown -S reboot
```

---

## 2. Use it

1. Open **ConnectedDrive** on the CID.
2. Open **speeduntrap**.
3. **Popup** — master on/off for the overlay. Stored as `/fs/sda0/radar_popup.on`.
4. **Range − / Range +** — 50 m, 100 m, then 200 … 1000 m. Stored as `/fs/sda0/radar_range.m`.
5. **Stop** — restore HmiMain3D, turn Popup off. The process stays running so the
   plugin page does not vanish (killing it from that menu reboots some HUs).

When Popup is on and you are inside Range of a packed trap (same heading /
`sentido` when the pack has it):

- HmiMain3D is hidden with WalControl.
- The overlay takes the whole CID (1280×480): map, distance, light/heavy limits,
  your speed, orange chevron.
- A short stereo WAV chime plays once (`/fs/sda0/radar/chime.wav`).
- After you pass the point, HmiMain3D comes back.

**MENU** on the iDrive knob hides the overlay. **OPTION** three times within
about two seconds brings it up for the nearest trap (demo), any distance.
Rotate the knob to zoom.

If Popup is off, the process must not call WalControl and must not hide HMI.

GPS is the OEM fused dead-reckoning feed maps already use (`PosReal.DNAVIPos`).
Open the OEM map once after boot so NavigationPositioning is alive. Fallback
file `/fs/sda0/radar/fix.txt` (`lat lon [heading]`) is used only if that attach
fails — do not leave the TEST pin in `fix.txt` or the HUD will sit on 0 m.

---

## 3. Update the speed-trap database

On the PC, from this repo root:

```
py -3 tools\fetch_public_pack.py
py -3 tools\make_hu_pack.py
```

`fetch_public_pack.py` talks to the public Bunda de Urso API and writes a slim
snapshot under `data/raw/` (our fields, not their schema). `make_hu_pack.py`
writes `hu/radar/points.txt`. Points without GPS are dropped. TEST controls in
`data/local/test-controls.ndjson` are merged in.

Copy **only** the new list onto the car:

```
hu/radar/points.txt  →  /fs/sda0/radar/points.txt
```

No reboot. The next start of `speeduntrap` loads it. Optional UF filter:

```
py -3 tools\fetch_public_pack.py --uf SC
```

Do not invent coordinates. Do not ship Bunda de Urso’s fat `fontes` / documents.

Built-in garage TEST trap (from `data/local/`):

```
-26.962599  -50.421094   light 30 km/h   heavy 20 km/h
```

---

## 4. Update the map tiles

The HU opens **one** file: `hu/radar/map.bin` (format `RMAP1`). It does not
open 292 000 JSON tiles.

1. Download Geofabrik `.osm.pbf` extracts into `data/maps/` (see
   [data/maps/README.md](data/maps/README.md)). Sul + Sudeste is what the
   current pack was built from.
2. On the PC:

   ```
   py -3 -m pip install -r requirements.txt
   py -3 tools\extract_osm_geojson.py --full
   py -3 tools\pack_map_bin.py
   ```

   First step is 15–40 minutes. Second writes `hu/radar/map.bin` (~1.1 GB).
3. Copy that file to `/fs/sda0/radar/map.bin`. Restart `speeduntrap`.

For a small PC preview around one coordinate instead of the whole region:

```
py -3 tools\extract_osm_geojson.py --lat -26.961387 --lon -50.422295
```

---

## 5. Preview on a PC (no car)

Same 1280×480 overlay. WASD drives at an exaggerated speed.

```
py -3 tools\pc_radar_emu\server.py
```

Browser: http://127.0.0.1:8765/  
`M` hides the HUD, `O` three times restores it, `+/-` changes Range, mouse
wheel zooms. Detection uses `hu/radar/points.txt`. Map tiles come from
`data/maps/tiles/` after an extract.

---

## 6. Rebuild the QNX binary (optional)

Prebuilt `hu/speeduntrap` is enough to install. To compile:

- QNX 6.5.0 SDP on Windows (`C:\QNX650`).
- Shared libs from an NBT dump (`libKD`, `libEGL`, `libGLESv2_CM`, `libscreen`,
  `libsys_dsi_servicebroker`, `liba2i_client`). Edit the `DUMP=` line in
  `poc/build_speeduntrap.cmd`.

```
poc\build_speeduntrap.cmd
copy poc\speeduntrap hu\speeduntrap
```

`poc/qnx_cert.h` is a **self-signed SAS client cert** for this app (“Hello World
App” / Rogue Root CA). It is not an OEM BMW secret. You need it to compile.

---

## Layout

```
hu/                  copy this onto /fs/sda0
poc/                 QNX sources + build script
tools/               fetch traps, pack map, PC emulator
data/raw/            slim API snapshots (CC BY 4.0)
data/local/          TEST points only — never from the public API
data/maps/           OSM PBF in, tiles out (not in git)
```

---

## Do not

- Do not slay HmiMain3D.
- Do not patch Check Control / `CPopupController`.
- Do not set starter `FinalAction` to `SHUTDOWN_SYSTEM`.
- Do not edit `/fs/sda0/repository/istep/...`.
- Do not copy Bunda de Urso collector / schema / web code onto the HU.
- Do not sell this software.

If something wedges the overlay: `slay speeduntrap` then start it again. One
Ctrl+C from SSH restores HMI.
