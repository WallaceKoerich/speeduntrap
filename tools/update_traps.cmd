@echo off
cd /d "%~dp0.."
echo Fetching public Bunda de Urso cadastro (CC BY 4.0)...
py -3 tools\fetch_public_pack.py
if errorlevel 1 exit /b 1
echo Packing hu\radar\points.txt ...
py -3 tools\make_hu_pack.py
if errorlevel 1 exit /b 1
echo Copy hu\radar\points.txt to /fs/sda0/radar/points.txt on the car.
pause
