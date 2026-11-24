# Map tiles (OpenStreetMap)

The car never opens a PBF. You cut OSM once on the PC, pack `hu/radar/map.bin`,
and copy that single file to `/fs/sda0/radar/map.bin`.

1. Download Geofabrik extracts for the states you drive:

   - Sul: https://download.geofabrik.de/south-america/brazil/sul.html
   - Sudeste: https://download.geofabrik.de/south-america/brazil/sudeste.html

2. Put the `.osm.pbf` files in this folder. Names can be `sul-*.osm.pbf` and
   `sudeste-*.osm.pbf` (the dated Geofabrik names are fine).

3. From the repo root (15–40 minutes, needs `osmiter`):

   ```
   py -3 -m pip install -r requirements.txt
   py -3 tools\extract_osm_geojson.py --full
   py -3 tools\pack_map_bin.py
   ```

4. Copy `hu/radar/map.bin` onto the HU. It is about 1.1 GB for Sul+Sudeste.

`--full` tiles the whole PBF. Without `--full`, the extractor cuts a ~13 km box
around `--lat` / `--lon` for the PC emulator.

Road data is © OpenStreetMap contributors, ODbL 1.0.
