# WLED Lighting Console — ESP32-S3

A standalone lighting console built on WLED: 4 faders, 8 macro buttons + BLACKOUT, 3 function buttons (SHIFT / BACK / GO),
rotary encoder, OLED, DMX in/out, line-in audio, and a native USB port that turns the whole panel into a USB MIDI
controller and a DMX-USB interface (ENTTEC DMX USB Pro protocol). Optional W5500 Ethernet.

- Target: ESP32-S3-WROOM-1 **N16R8** (16 MB quad flash, 8 MB octal PSRAM), PlatformIO on Windows
- Base: wled/WLED `main` (17.0.0-dev), tested on commit `20e7a71` (2026-10-25), arduino-esp32 **3.3.8**
- WLED 16.0.x is **not** supported: it ships arduino-esp32 2.0.18, which has no USB MIDI

Contents: [1 Hardware](#1-hardware) · [2 Package contents](#2-package-contents) · [3 Installation](#3-installation) ·
[4 First configuration](#4-first-configuration-in-wled) · [5 Operation](#5-operation) · [6 USB mode](#6-usb-mode) ·
[7 DMX](#7-dmx) · [8 Audio](#8-audio) · [9 Ethernet](#9-ethernet-optional) · [10 Settings reference](#10-settings-reference) ·
[11 Info page](#11-info-page) · [12 Troubleshooting](#12-troubleshooting) · [13 Status and limits](#13-status-and-limits) ·
[14 Updating WLED](#14-updating-wled) · [15 License](#15-license)

---

## 1. Hardware

### Pin map

| Function | GPIO | Notes |
|---|---|---|
| Fader 1 / 2 / 3 / 4 | 1 / 2 / 4 / 5 | ADC1, linear 10 k (B103), brightness / speed / intensity / color |
| Macro buttons 1–8 | 16, 38, 39, 40, 41, 42, 45, 46 | to GND, internal pull-up; macro *n* = *n*-th pin of this list |
| BLACKOUT | 3 | to GND, internal pull-up |
| Rotary encoder | CLK 6, DT 7, SW 15 | KY-040 type |
| I2C | SDA 8, SCL 9 | MCP23017 (default `0x27`) and OLED |
| MCP23017 buttons | PA0 SHIFT, PA1 BACK, PA2 GO | to GND, internal pull-ups |
| DMX out | TX 17 (UART2) | RS485 module; the firmware drives no direction pin |
| DMX in | RX 18 (UART1) | RS485 module with fixed receive direction; no TX/EN pin used |
| Audio (PCM1808) | BCK 14, SD 21, WS 47, MCLK 48 | WLED is I2S master and supplies MCLK |
| Native USB | D− 19, D+ 20 | USB MIDI + one COM port |
| Ethernet (`console_s3_eth` only) | SCLK 12, MOSI 11, MISO 13, CS 10, RST 43 | INT **not connected**, see [section 9](#9-ethernet-optional) |
| Devkit USB-UART | TX 43, RX 44 | flashing and serial log; released for the W5500 in the Ethernet build |

### Notes

- The ADC saturates at about 3.1 V on the S3, so the faders are calibrated to `raw-max ≈ 4050`.
- Both DMX RS485 modules must have their direction fixed (output: always transmit, input: always receive) or switch
  it automatically. RDM is therefore not possible.
- The W5500 is a 3.3 V device. Never feed it 5 V on a 3.3V/VCC pin.

---

## 2. Package contents

`console_update.zip` (and this repository, which holds the same files) has the layout of the WLED source tree
(`WLED-main\`). Copy it **over** a WLED checkout.

| Path | What it is |
|---|---|
| `usermods/console_faders/` | 4 faders → brightness / speed / intensity / color (palette with SHIFT), touch-activated |
| `usermods/console_io/` | MCP23017 buttons (publishes the SHIFT state), macro/BLACKOUT scanning, SHIFT/BACK/GO logic, encoder in USB mode |
| `usermods/console_usb/` | USB composite device (MIDI + COM), USB/standalone mode switching, OLED "MIDI-USB" screen |
| `usermods/console_dmx/` | Standalone DMX pins + ENTTEC DMX USB Pro bridge (+ `enttec_pro.h`) |
| `usermods/console_eth/` | Info-page row for the W5500 (active only in `console_s3_eth`) |
| `wled00/console_bus.h` | State shared between the console usermods, MIDI map |
| `wled00/eth_w5500.cpp` | W5500 support (compiled only in `console_s3_eth`) |
| `wled00/src/dependencies/dmx/SparkFunDMX.*` | **Replace** WLED's DMX output driver (non-blocking, configurable pin) |
| `dmx_patch/patch_core_dmx.diff` | Core patch: DMX (3 files) |
| `eth_patch/patch_core_eth.diff` | Core patch: SPI Ethernet (6 files), only needed for `console_s3_eth` |
| `alt_patch/patch_alt.diff` | Patch for the two stock "ALT" usermods (encoder, display) |
| `platformio_override.ini` | Environments `console_s3` and `console_s3_eth` |
| `patch_dmx.txt`, `patch_eth.txt` | Detailed notes and manual fallbacks for the two core patches |
| `test_enttec.ps1` | PC-side test of the ENTTEC bridge, no third-party software needed |
| `NOTICE` | Copyright and third-party licences (the repository also has `LICENSE`, the EUPL-1.2 text, which WLED already ships) |

---

## 3. Installation

Requirements: PlatformIO (VS Code extension or CLI), git, a WLED `main` checkout (`WLED-main\`, a git repository).

**1. Copy the files.** Extract `console_update.zip` over `WLED-main\` and confirm the overwrite. From the repository
instead (PowerShell; `README.md` and `LICENSE` are excluded so WLED's own are kept):

```powershell
git clone https://github.com/wled/WLED.git WLED-main
git -C WLED-main checkout 20e7a71          # the commit this was tested on
git clone https://github.com/bob45ultras/Wled_Console_s3.git
robocopy Wled_Console_s3 WLED-main /E /XD .git /XF README.md LICENSE
```

If you customised `platformio_override.ini`, merge by hand instead of overwriting it.

**2. Apply the patches.** PowerShell, inside `WLED-main\`. `--check` changes nothing; run it first.

```powershell
git apply --ignore-whitespace --check dmx_patch\patch_core_dmx.diff
git apply --ignore-whitespace dmx_patch\patch_core_dmx.diff

git apply --ignore-whitespace --check alt_patch\patch_alt.diff
git apply --ignore-whitespace alt_patch\patch_alt.diff

# only if you build console_s3_eth (harmless otherwise: everything is behind WLED_USE_ETHERNET_SPI)
git apply --ignore-whitespace --check eth_patch\patch_core_eth.diff
git apply --ignore-whitespace eth_patch\patch_core_eth.diff
```

The patches are independent of each other (any order works). If `--check` fails because a patch is **already applied**,
`git apply --reverse --check <patch>` succeeds: skip it. If it fails for another reason, the upstream code has moved:
follow the manual steps in `patch_dmx.txt` / `patch_eth.txt`.

What they change:

| Patch | Files | Purpose |
|---|---|---|
| DMX | `dmx_input.cpp`, `wled.cpp`, `xml.cpp` | input works with RX only; frames forwarded to USB; no input processing in USB mode; DMX TX pin configurable (stock WLED reserves GPIO2 = fader 2) |
| ALT | `usermod_v2_rotary_encoder_ui_ALT.cpp`, `usermod_v2_four_line_display_ALT.cpp` | encoder UI is paused in USB mode; display no longer takes the button-0 double press (network screen is opened with GO) |
| Ethernet | `cfg.cpp`, `const.h`, `network.cpp`, `wled_ethernet.h`, `wled_serial.cpp`, `xml.cpp` | SPI W5500 instead of WLED's RMII-only Ethernet (the S3 has no RMII) |

**3. Build and flash.**

```powershell
pio run -e console_s3 -t upload          # WiFi only (normal firmware)
pio run -e console_s3_eth -t upload      # with W5500 Ethernet
```

Flash through the devkit's **UART** port (the serial log also goes there, 115200 baud). The native USB port is for the
console's own MIDI/DMX functions. If an upload fails, hold BOOT, tap RESET, release BOOT and retry
(on the native USB port the board then shows up as the ROM bootloader).

---

## 4. First configuration in WLED

Everything is in the WLED web UI. Settings already saved on the device win over the firmware defaults.

1. **Config → LED Preferences → Buttons.** Define buttons for GPIO `16, 38, 39, 40, 41, 42, 45, 46` (macros 1–8) and `3`
   (BLACKOUT) as push buttons and assign their actions (e.g. short press → preset). The firmware allows 10 buttons
   (`WLED_MAX_BUTTONS=10`). Button order does not matter: the usermods match buttons by GPIO.
2. **Config → Usermods → Console-IO.** Set `first-preset` / `last-preset` (range stepped by BACK/GO) and
   `shift-preset-1…8` (preset recalled by SHIFT + macro).
3. **Config → Usermods → Console-Faders.** Calibrate: read the raw values on the Info page with each fader fully down
   and fully up, enter the lowest/highest as `raw-min` / `raw-max`. Set `invert` so that "up" = maximum.
4. **Config → Usermods → AudioReactive.** The build defaults are I2S type 4 ("generic I2S with master clock") and
   pins 14 / 21 / 47 / 48. A saved configuration overrides them, so check them once.
5. **Config → Usermods → Global I2C GPIOs.** SDA 8, SCL 9. The build sets no default: until this is saved,
   Console-IO shows "Global I2C not set".
6. **Config → Sync Interfaces.** DMX output (start address, channels per fixture, optional Proxy Universe) and DMX input
   (mode + address). See [section 7](#7-dmx).
7. OLED: keep the existing four-line-display setup. Encoder pins come from the build flags (6 / 7 / 15).

---

## 5. Operation

### Faders

Each fader is **inactive** until moved by more than about 1.5 % of its travel; then it takes control, and it releases
control after `release-ms` (1.5 s) of standing still. At boot all faders are inactive, so nothing jumps at power-up.
While inactive, a fader never overwrites changes made from the web UI, encoder, presets or apps. When a fader takes
control, the value jumps to the fader position.

| Fader | GPIO | Controls |
|---|---|---|
| 1 | 1 | Brightness (never reaches 0: use BLACKOUT or power to turn off) |
| 2 | 2 | Effect speed of the selected segments (main segment if none selected) |
| 3 | 4 | Effect intensity of the selected segments |
| 4 | 5 | Primary color: hue sweep at full saturation. **With SHIFT held: palette** (see below) |

**SHIFT + color fader = palette.** Hold SHIFT and move the color fader: instead of the hue it selects the palette of the
selected segments. The travel is divided evenly among all palettes, in the order of WLED's palette list (built-in,
usermod, custom), so the ends of the fader are the first and the last palette. The function is decided when the fader
takes control. If SHIFT is pressed or released while the fader is in control, it lets go without any jump and must be
moved again (more than `touch`) to take control with the new function. Using it this way also cancels the
"hold SHIFT = switch mode" action, so a long palette adjustment never changes the USB/standalone mode. In USB mode the
fader only sends its MIDI CC, with or without SHIFT. `shift-palette` (Console-Faders) turns the function off.
The Info page marks the fader `P` while it acts as a palette selector. With many palettes the steps are fine (about
1 mm each): a short WLED transition time makes the changes feel more direct.

### Buttons (standalone mode)

| Control | Action |
|---|---|
| Macro 1–8, BLACKOUT | Handled by WLED as configured in Buttons |
| SHIFT + macro *n* (short press) | Recalls preset `shift-preset-n`; WLED ignores that macro press. `0` = no action. The decision is taken when the macro is pressed: SHIFT must already be down |
| BACK | Previous preset in `first-preset…last-preset`, immediately on press |
| GO (short press) | Next preset, on release |
| GO held ≥ `go-hold-ms` (3 s) | Network info screen on the OLED (7 s); the next preset is not triggered |
| SHIFT + color fader | Palette selection (see above) |
| SHIFT held ≥ `mode-hold-ms` (3 s) | Switches USB ↔ standalone (see section 6). Cancelled if a macro is pressed or the color fader is used during the hold |
| Encoder + OLED | Stock `rotary_encoder_ui_ALT` + `four_line_display_ALT` behaviour |

BACK/GO wrap around at the ends of the range. A preset number that does not exist is skipped with one extra press.

---

## 6. USB mode

When a PC configures the native USB port (and is not suspended) for at least 300 ms, the console enters **USB mode**:

- every control only sends MIDI; WLED no longer sees faders, macros, BLACKOUT, SHIFT/BACK/GO or the encoder;
- the OLED shows **MIDI-USB** with a USB logo;
- the COM port becomes an ENTTEC DMX USB Pro interface (see [section 7](#7-dmx)) and WLED's own DMX is suspended;
- the current fader positions are sent at once, so the PC program aligns itself.

Unplugging the cable (or suspending the PC) returns to standalone immediately. Holding SHIFT for 3 s switches mode by hand:
USB → standalone shows "STANDALONE / controls on WLED"; repeating it returns to USB; with no PC connected it shows
"NO PC / connect USB". The manual choice is forgotten when the cable is unplugged. `force-standalone`
(Console-USB settings) ignores the PC altogether.

USB identity: VID `0x303A`, PID `0x4003`, product "Console MIDI+DMX". Windows shows one MIDI device and a
"USB Serial Device (COMx)" (the DMX port).

### MIDI map (channel 1)

| Control | Message | Value |
|---|---|---|
| Faders 1–4 | CC 16–19 | 0–127 |
| Encoder rotation | CC 30 | relative: 1 = clockwise, 127 = counter-clockwise |
| Macros 1–8 | Note 36–43 | on = 100, off = 0 |
| BLACKOUT | Note 44 | |
| SHIFT / BACK / GO | Note 48 / 49 / 50 | |
| Encoder button | Note 51 | |

---

## 7. DMX

**Standalone.** Output: WLED's DMX driver, replaced by a non-blocking version (a dedicated task refreshes the line
at about 40 Hz), UART2, TX = GPIO17. Input: WLED's `esp_dmx` on UART1, RX = GPIO18. `console_dmx` sets the input pins
at every boot (`force-pins`); DMX mode and address are set in Config → Sync Interfaces as usual.

**USB mode.** The COM port speaks ENTTEC DMX USB Pro:

| Direction | Label | Behaviour |
|---|---|---|
| PC → console | 6 | DMX frame → RS485 output (GPIO17) |
| console → PC | 5 | frames received on GPIO18, sent when they change, plus a refresh every 250 ms |
| PC → console | 3, 10 | parameters and serial number requests are answered |

In USB mode the received DMX does not touch the effects and the DMX output does not show the pixels. Back in standalone,
WLED takes the output again by itself.

**Test without third-party software:** put the console in USB mode, find the COM port in Device Manager and run

```powershell
powershell -ExecutionPolicy Bypass -File .\test_enttec.ps1 -Port COM7
```

It checks the serial-number and parameter replies, then sends frames for 5 s. The Info row of Console-DMX must show
`PC>DMX n` growing.

**QLC+ does not see the console.** Its "DMX USB" plugin only recognises FTDI chips (D2XX driver); the console is a native
ESP32-S3 USB device. Do not fake the FTDI VID/PID. Options: programs that speak ENTTEC Pro on any COM port
(e.g. TouchDesigner, pyserial scripts); or, for output only, Art-Net/E1.31 over the network with Config → DMX Output →
Proxy Universe = N. DMX input to the PC only goes through the USB bridge.

**Compatibility with AudioReactive.** No pin or core conflict (audio uses 14/21/47/48, DMX 17/18). WLED pauses audio
processing while it is in *realtime* mode. DMX input modes that drive the pixels directly (SINGLE_RGB, SINGLE_DRGB,
MULTIPLE_*) enter it, which is harmless since effects are not visible then; the effect modes (EFFECT, EFFECT_W,
EFFECT_SEGMENT*) do not, so audio-reactive effects keep running.

---

## 8. Audio

PCM1808 line-in on I2S: BCK = 14, SD = 21, WS = 47, MCLK = 48. WLED generates BCK/LRCK and supplies MCLK (256 × fs).
Build defaults: `SR_DMTYPE=4`, `I2S_CKPIN=14`, `I2S_SDPIN=21`, `I2S_WSPIN=47`, `MCLK_PIN=48`.
Settings are in Config → Usermods → AudioReactive.

---

## 9. Ethernet (optional)

WLED `main` only supports RMII Ethernet and the S3 has no RMII, so the console uses a **W5500** SPI module through the
driver built into the Arduino core. It is a separate environment, `console_s3_eth`; the normal firmware is unchanged.

| W5500 module | ESP32-S3 |
|---|---|
| SCLK | GPIO12 |
| MOSI | GPIO11 |
| MISO | GPIO13 |
| SCS | GPIO10 |
| RST | GPIO43 (must be connected) |
| INT | **not connected** (the chip is polled every 10 ms) |
| 3.3V / GND | 3.3V / GND |

- Do **not** wire INT to GPIO44: it is the UART0 RX the devkit's USB-serial bridge uses for flashing; uploads then fail
  with "no sync reply".
- In this build UART0 is released: **no serial log** and no serial Adalight/Improv. Native USB (MIDI, DMX) is unaffected.
- Select Config → WiFi Setup → Ethernet Type → "W5500 (SPI)" (already the default). WiFi stays as fallback.
- The module draws about 130 mA when linked; keep the wires short.
- Before connecting a new module, measure MISO to GND with a multimeter. A very low resistance means a faulty module
  (this was the failure of the first module used).
- Full bring-up procedure and the meaning of every `Console-ETH` message: `patch_eth.txt`.

---

## 10. Settings reference

Config → Usermods. Pin changes take effect after a reboot.

**Console-IO**

| Key | Default | Meaning |
|---|---|---|
| `enabled` | on | |
| `i2c-address` | 39 (`0x27`) | MCP23017 address in decimal. If absent, `0x20…0x27` are scanned |
| `debounce-ms` | 15 | |
| `mode-hold-ms` | 3000 | SHIFT hold for USB ↔ standalone |
| `go-hold-ms` | 3000 | GO hold for the network screen |
| `first-preset`, `last-preset` | 1, 16 | range stepped by BACK / GO |
| `shift-preset-1…8` | 0 | preset for SHIFT + macro *n* (0–250, 0 = none) |

**Console-Faders**

| Key | Default | Meaning |
|---|---|---|
| `enabled` | on | |
| `invert` | on | up = maximum |
| `shift-palette` | on | SHIFT + color fader selects the palette (standalone only) |
| `pin-brightness`, `pin-speed`, `pin-intensity`, `pin-color` | 1, 2, 4, 5 | |
| `raw-min`, `raw-max` | 40, 4050 | ADC readings at the ends of travel |
| `raw-mid` | 0 (off) | ADC reading at mid travel, for logarithmic-taper faders only |
| `hysteresis` | 14 | ADC counts, against jitter |
| `touch` | 60 | movement that wakes an inactive fader (at least `hysteresis` + 10) |
| `release-ms` | 1500 | standing still this long → fader inactive |

**Console-USB**

| Key | Default | Meaning |
|---|---|---|
| `enabled` | on | |
| `force-standalone` | off | ignore the PC |
| `bootloader-reset` | off | let the COM port reboot the board into download mode (needs reboot). Enable only if you flash over the native USB |

**Console-DMX**

| Key | Default | Meaning |
|---|---|---|
| `enabled` | on | |
| `force-pins` | on | enforce the input pins at every boot |
| `dmx-in-pin` | 18 | RX GPIO of the input module, −1 = input off |
| `dmx-in-port` | 1 | UART of `esp_dmx` (1 or 2; 2 belongs to the output) |

---

## 11. Info page

| Row | Examples |
|---|---|
| Console-Faders | `812* 2034 55 4000P` — raw ADC values, `*` = fader active, `P` = color fader acting as palette selector |
| Console-IO | `SHIFT BACK* GO @27` — `*` = pressed; `MCP23017 not found. On the bus: …`; `Global I2C not set` |
| Console-USB | `USB mode (MIDI), MIDI sent: 1234`; `standalone (manual)`; `PC connected, waiting`; `not connected` |
| Console-DMX | `standalone, input receiving, output 4210 frames`; `ENTTEC USB: PC>DMX 150, DMX>PC 12, dropped 0` |
| Console-ETH | `100 Mbps full, 192.168.1.50` and the error messages listed in `patch_eth.txt` |

---

## 12. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Upload: "no sync reply", "serial TX path seems to be down" | Wire on GPIO44 (W5500 INT). Disconnect it. Otherwise BOOT + RESET and upload via the native USB port |
| Fader 2 dead / reported in use | The DMX core patch is missing (stock WLED reserves GPIO2 for DMX) |
| A fader works backwards | Toggle `invert` |
| A fader does not reach 0 or 255 | Recalibrate `raw-min` / `raw-max` from the Info-page values |
| Console-IO: "MCP23017 not found" | The row lists the I2C addresses present; set `i2c-address` or check wiring and pull-ups |
| Macro presses do nothing in USB mode | Expected: they go out as MIDI notes |
| Console looks frozen after a program closes the COM port | Download-mode reboot; reset the board. Keep `bootloader-reset` off |
| QLC+ does not list the console | See [section 7](#7-dmx) |
| Audio settings in the build ignored | A saved AudioReactive configuration wins; edit it in the UI |
| No serial log | Normal in `console_s3_eth` (UART0 belongs to the W5500) |
| Patch fails after `git pull` | Upstream moved; see [section 14](#14-updating-wled) |

---

## 13. Status and limits

| Area | Status |
|---|---|
| Faders (touch activation, all four parameters, calibration, SHIFT + color fader = palette) | Verified on hardware |
| Standalone buttons (macros, SHIFT + macro, BACK/GO, GO hold) | Verified on hardware |
| USB MIDI (faders, buttons, encoder), OLED "MIDI-USB" screen, mode switching | Verified on hardware |
| ENTTEC bridge (serial, parameters, PC → DMX frames) | Verified with `test_enttec.ps1` |
| PCM1808 line-in with AudioReactive | Verified on hardware |
| DMX output and input on real DMX equipment | **Pending** (needs a controller or a light) |
| W5500 Ethernet | **Pending**: builds and links; the first module was faulty (MISO held low), so start-up with a live chip is untested |
| Battery / power stage | **Pending** |

Known limits:

- No RDM (the input module cannot transmit).
- In USB mode the DMX output keeps the last frame received from the PC even if the program closes the COM port; it
  changes only when USB is unplugged or the console returns to standalone.
- Toward the PC, DMX frames are sent on change plus every 250 ms; ENTTEC's "receive on change" mode (label 9) is not
  implemented, whole frames are always sent.
- Ethernet build: no serial log, no serial Adalight/Improv; UART0 (GPIO43/44) must not be reused.

---

## 14. Updating WLED

The core patches live in files WLED itself changes. After a `git pull`:

1. `git status`, then undo the patches first if you want a clean tree: `git apply --reverse <patch>` for each, or
   `git stash`.
2. Pull, then repeat the `--check` / apply steps of [section 3](#3-installation).
3. A failed `--check` points at the hunk that moved; use the manual steps in `patch_dmx.txt` and `patch_eth.txt`
   (the ALT patch is two one-line changes, shown below).
4. `SparkFunDMX.cpp/.h` replace WLED's own files: if upstream changed them, compare before overwriting.
5. Build both environments, then repeat the quick checks: Info page rows, one fader, one SHIFT + macro, USB mode.

ALT patch by hand: in `usermod_v2_rotary_encoder_ui_ALT.cpp` add `#include "console_bus.h"` after `#include "wled.h"` and
change `if (!enabled) return;` in `RotaryEncoderUIUsermod::loop()` to `if (!enabled || ConsoleBus::usbActive()) return;`;
in `usermod_v2_four_line_display_ALT.cpp` replace `|| b // button 0 only` in `FourLineDisplayUsermod::handleButton()`
with `|| true`.

Keep your own work under version control (a branch or a fork) so the patched tree can be recreated from the diffs.

---

## 15. License

Copyright (c) 2026 E-Boats Systems. Licensed under the **European Union Public Licence v. 1.2** (EUPL-1.2): see `LICENSE`.
Third-party parts (patches to WLED, the modified SparkFun DMX driver, libraries used at build time) are listed in `NOTICE`.

In short: using and modifying the firmware for yourself needs nothing. If you give or sell firmware or devices that contain it,
the recipient must get the source (this repository plus the WLED commit it was built from), the `LICENSE` and `NOTICE`
files, and the copyright notices kept. The firmware includes GPL-3.0 code (arduinoFFT, via audioreactive); the EUPL allows
this combination, but not closed-source firmware. Schematics and enclosures are not covered. This is not legal advice.
