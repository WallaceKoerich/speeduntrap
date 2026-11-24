# Contributing

Pull requests are welcome. Keep the PolyForm Noncommercial license. Do not
add a path that lets anyone sell this app or a paid build of it.

## Rules that will get a PR closed

- Copying, adapting, or re-shipping Bunda de Urso collector / schema / web code.
- Inventing trap coordinates, speeds, manufacturer, or model. A point without GPS is not an alert.
- Patching HmiMain3D, injecting Check Control IDs, or slaying HmiMain3D.
- `FinalAction` `SHUTDOWN_SYSTEM` in starter.cfg. A crash must not reboot the HU.
- Windows CRLF on `hu/*.sh`. QNX will print `No such file or directory`.

## Useful PRs

- Safer boot / rollback scripts.
- Better heading / `sentido` filtering.
- Smaller `map.bin` or regional cuts.
- PC emulator fidelity with the CID overlay.
- Translations of the ConnectedDrive Radar page.
- Evidence (HU log, ELF xref) for GPS or audio changes. Do not guess BMW APIs.

## How to test without a car

```
py -3 -m pip install -r requirements.txt
py -3 tools\pc_radar_emu\server.py
```

Open http://127.0.0.1:8765/ — WASD drives, `M` hides the HUD, `O` three times restores it.
