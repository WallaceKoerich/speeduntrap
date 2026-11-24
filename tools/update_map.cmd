@echo off
cd /d "%~dp0.."
echo Put Geofabrik sul*.osm.pbf and sudeste*.osm.pbf in data\maps\ first.
echo This takes 15-40 minutes, then writes hu\radar\map.bin (~1.1 GB).
py -3 -m pip install -r requirements.txt
py -3 tools\extract_osm_geojson.py --full
if errorlevel 1 exit /b 1
py -3 tools\pack_map_bin.py
if errorlevel 1 exit /b 1
echo Copy hu\radar\map.bin to /fs/sda0/radar/map.bin on the car.
pause
