@echo off
cd /d "%~dp0.."
py -3 tools\pc_radar_emu\server.py
pause
