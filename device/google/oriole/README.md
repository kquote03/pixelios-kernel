# Pixel 6 (oriole)

SoC: `soc/google/gs101` (shared with the Pixel 6a, `device/google/bluejay`). Status: bring-up, see `device/google/oriole/docs/PORT.md`.

Tools target this phone only when asked: `export BOARD=oriole` (default is `bluejay`; `python3 tools/boards.py --target` prints the current target).
Captures from this device go in `hardware/` (sanitized: no serial numbers).
