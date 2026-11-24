@echo off
rem speeduntrap: ConnectedDrive Radar settings + map overlay.
rem Same SAS/RHMI as id4_radar_x86. Launch from the launcher or Putty
rem (`speeduntrap --test` for SSH logs only). Copy to /fs/sda0/speeduntrap.

set QNX_HOST=C:\QNX650\host\win32\x86
set QNX_TARGET=C:\QNX650\target\qnx6
set PATH=C:\QNX650\host\win32\x86\usr\bin;%PATH%
rem Point HU_DUMP at your NBT dump's fs/sda0 (libs: libKD, libEGL, libscreen, ...).
if not defined HU_DUMP set HU_DUMP=C:/Users/TR0N/Desktop/HU/DUMP/fs/sda0
set DUMP=%HU_DUMP%
set SYSLIB=%DUMP%/opt/sys/lib

cd /d %~dp0
ntox86-gcc -O2 -Wall -Wno-unused-function -DSPEEDUNTRAP -o speeduntrap ^
  id4_radar.c id4_radar_core.c id4_radar_gps.c speeduntrap_map.c speeduntrap_ui.c ^
  id4_aa_display.c id4_aa_zbe.c id4_aa_cm.c id4_aa_am.c id4_radar_chime.c ^
  %SYSLIB%/libsys_dsi_servicebroker.so.2 ^
  %DUMP%/lib/libKD.so ^
  %DUMP%/lib/libEGL.so ^
  %DUMP%/lib/libGLESv2_CM.so ^
  %DUMP%/lib/graphics/tunnelcreek/libscreen.so.1 ^
  %DUMP%/opt/sys/lib/liba2i_client.so ^
  -lm -lsocket ^
  -Wl,-rpath,/opt/sys/lib:/lib:/lib/graphics:/lib/graphics/tunnelcreek ^
  -Wl,-rpath-link,%DUMP%/lib -Wl,--allow-shlib-undefined
if errorlevel 1 exit /b 1
ntox86-strip speeduntrap
copy /y speeduntrap ..\hu\speeduntrap >nul
echo Built speeduntrap  (copied to hu\speeduntrap)
