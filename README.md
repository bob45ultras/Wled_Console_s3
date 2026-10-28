# Wled_Console_s3

Standalone lighting console built on [WLED](https://github.com/wled/WLED) for an ESP32-S3-WROOM-1 N16R8:
4 faders, 8 macro buttons + BLACKOUT, SHIFT / BACK / GO, rotary encoder, OLED, DMX in/out, PCM1808 line-in audio,
and a native USB port that turns the panel into a USB MIDI controller and an ENTTEC DMX USB Pro interface.
Optional W5500 Ethernet.

This repository is **not** a WLED fork. It holds only what is added to, or patched into, a WLED checkout, laid out like the
WLED source tree: custom usermods (`usermods/console_*`), a few core files, three small patches
(`dmx_patch/`, `alt_patch/`, `eth_patch/`) and `platformio_override.ini` with the build environments.

Tested on wled/WLED `main` commit `20e7a71` (2026-10-25), arduino-esp32 3.3.8.

## Quick start

```powershell
git clone https://github.com/wled/WLED.git WLED-main
git -C WLED-main checkout 20e7a71
git clone https://github.com/bob45ultras/Wled_Console_s3.git
robocopy Wled_Console_s3 WLED-main /E /XD .git /XF README.md
cd WLED-main
git apply --ignore-whitespace dmx_patch\patch_core_dmx.diff
git apply --ignore-whitespace alt_patch\patch_alt.diff
pio run -e console_s3 -t upload
```

Add `git apply --ignore-whitespace eth_patch\patch_core_eth.diff` and build `console_s3_eth` only for the W5500 Ethernet version.

Everything else (wiring, settings, MIDI map, DMX, troubleshooting, status) is in **[CONSOLE_README.md](CONSOLE_README.md)**.

## Status

Verified on hardware: faders (including SHIFT + color fader = palette), standalone buttons, USB MIDI (faders, buttons, encoder),
OLED "MIDI-USB" screen, ENTTEC bridge (PC test script), line-in audio. Pending: DMX on real equipment, W5500 with a working
module, battery/power stage.

## License

The usermods use WLED's API; WLED is under the EUPL v. 1.2. A license for this repository has not been chosen yet.
