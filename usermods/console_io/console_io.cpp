// SPDX-License-Identifier: EUPL-1.2
// Copyright (c) 2026 E-Boats Systems (https://www.eboatsystems.com)

#include "wled.h"
#include <Wire.h>
#include <string.h>
#include "console_bus.h"

// Rotary encoder: same pins as the rotary_encoder_ui_ALT usermod (flags in platformio_override.ini)
#ifndef ENCODER_DT_PIN
  #define ENCODER_DT_PIN 7
#endif
#ifndef ENCODER_CLK_PIN
  #define ENCODER_CLK_PIN 6
#endif
#ifndef ENCODER_SW_PIN
  #define ENCODER_SW_PIN 15
#endif

/*
 * console_io — console inputs that are not native to WLED
 *
 * Stage A: MCP23017 expander (I2C) with the 3 function buttons
 *   PA0 = SHIFT, PA1 = BACK, PA2 = GO     (button to GND, internal pull-ups)
 *
 * Functions in STANDALONE (USB mode: everything goes out as MIDI notes, see console_bus.h):
 *   BACK / GO      previous / next preset, within the range "first-preset".."last-preset"
 *                  (config). Missing presets in the range are skipped with one extra press.
 *   GO held        network information on the OLED after "go-hold-ms" (default 3 s); the next
 *                  preset starts on RELEASE, only if the press was short.
 *   SHIFT + macro  recalls the preset set in "shift-preset-N" (N = 1..8, 0 = no action),
 *                  short press only. WLED ignores the macro pressed together with SHIFT: its
 *                  normal preset does not start. What counts is SHIFT being pressed at the moment
 *                  the macro is pressed.
 *   SHIFT held     switches mode USB <-> standalone after "mode-hold-ms"; if a macro is pressed
 *                  during the hold, or the color fader takes control (SHIFT + color fader = palette,
 *                  see console_faders), the mode change is cancelled.
 *
 * Uses WLED's global I2C bus (Config > Usermods > Global I2C GPIOs):
 * SDA=GPIO8, SCL=GPIO9. WLED initializes it by itself, there is no Wire.begin() here.
 *
 * Diagnostics: if the chip does not answer at the configured address, the usermod
 *   - looks for an MCP23017 at the other addresses 0x20..0x27 and uses the first one it finds;
 *   - shows all the I2C devices present on the bus on the Info page.
 */

class ConsoleIO : public Usermod {
  private:
    enum { NUM_BTN = 3 };
    enum : uint8_t { BTN_SHIFT = 0, BTN_BACK = 1, BTN_GO = 2 };

    // MCP23017 registers (BANK=0 mode, the default)
    enum : uint8_t {
      REG_IODIRA = 0x00, REG_IODIRB = 0x01,
      REG_IPOLA  = 0x02, REG_IPOLB  = 0x03,
      REG_GPINTENA = 0x04, REG_GPINTENB = 0x05,
      REG_GPPUA  = 0x0C, REG_GPPUB  = 0x0D,
      REG_GPIOA  = 0x12
    };

    static const unsigned long POLL_MS  = 5;      // read every 5 ms
    static const unsigned long RETRY_MS = 5000;   // search for the chip if absent

    // --- configuration (Config > Usermods) ---
    bool enabled    = true;
    int  addr       = 0x27;
    int  debounceMs = 15;
    int  modeHoldMs = 3000;      // SHIFT held this long = switch mode USB <-> standalone
    int  goHoldMs   = 3000;      // GO held this long = network information (standalone)
    int  stepFirst  = 1;         // BACK / GO step through the presets from stepFirst to stepLast
    int  stepLast   = 16;
    int  shiftPreset[8] = {0, 0, 0, 0, 0, 0, 0, 0};   // SHIFT + macro 1..8 -> preset (0 = no action)

    // --- runtime state ---
    bool noI2c   = false;      // global I2C bus not configured
    bool present = false;
    bool autoAddr = false;     // address found automatically, different from the configured one
    int  curAddr = -1;         // address actually in use
    char scanBuf[40] = "";     // I2C addresses found on the bus (hexadecimal)
    bool pressed[NUM_BTN]   = {false, false, false};   // filtered state
    bool cand[NUM_BTN]      = {false, false, false};   // candidate waiting for debounce
    unsigned long candSince[NUM_BTN] = {0, 0, 0};
    uint8_t failCount = 0;
    unsigned long lastPoll = 0, lastRetry = 0;
    uint32_t eventCount = 0;
    int8_t lastId = -1;
    bool lastPressed = false;
    unsigned long shiftSince = 0;    // when SHIFT was pressed
    bool shiftFired = false;         // the long press has already switched mode
    bool shiftUsed  = false;         // a macro was pressed during this SHIFT press: no mode change
    unsigned long goSince = 0;       // when GO was pressed
    bool goFired = false;            // the long press has already opened the network information
    uint8_t lastTarget = 0;          // last preset requested by BACK / GO
    uint8_t lastCurAtApply = 0;      // WLED's currentPreset at the time of that request

    // 8 macros + BLACKOUT: native WLED buttons. In USB mode we read them directly from the GPIOs
    // (and handleButton tells WLED to ignore them): this way we do not depend on WLED's internal
    // button configuration.
    bool nPressed[ConsoleBus::NATIVE_COUNT]        = {};
    bool nCand[ConsoleBus::NATIVE_COUNT]           = {};
    unsigned long nSince[ConsoleBus::NATIVE_COUNT] = {};
    bool nInit = false;
    unsigned long lastScan = 0;
    bool nLow[ConsoleBus::NATIVE_COUNT]           = {};   // last raw sample of the pin: at GND
    bool nShiftAtPress[ConsoleBus::NATIVE_COUNT]  = {};   // SHIFT was pressed at the first sample of this press

    // rotary encoder: in USB mode the rotary_encoder_ui_ALT usermod is stopped (see patch)
    // and rotation and button go out over MIDI
    bool encInit = false;
    bool encPrevA = false;
    unsigned long encLastMs = 0, encLastEdge = 0;
    bool swPressed = false, swCand = false;
    unsigned long swSince = 0;

    static const char _name[];
    static const char* btnName(uint8_t id) {
      return id == BTN_SHIFT ? "SHIFT" : (id == BTN_BACK ? "BACK" : "GO");
    }

    bool probeAt(int a) {
      Wire.beginTransmission((uint8_t)a);
      return Wire.endTransmission() == 0;
    }

    bool writeReg(uint8_t reg, uint8_t val) {
      Wire.beginTransmission((uint8_t)curAddr);
      Wire.write(reg);
      Wire.write(val);
      return Wire.endTransmission() == 0;
    }

    bool readPortA(uint8_t &v) {
      Wire.beginTransmission((uint8_t)curAddr);
      Wire.write((uint8_t)REG_GPIOA);
      if (Wire.endTransmission(false) != 0) return false;
      if (Wire.requestFrom((uint16_t)curAddr, (uint8_t)1, true) != 1) return false;
      v = (uint8_t)Wire.read();
      return true;
    }

    // all lines as inputs with pull-up, no interrupts, normal polarity
    bool initChip() {
      bool ok = true;
      ok &= writeReg(REG_IODIRA, 0xFF);
      ok &= writeReg(REG_IODIRB, 0xFF);
      ok &= writeReg(REG_IPOLA, 0x00);
      ok &= writeReg(REG_IPOLB, 0x00);
      ok &= writeReg(REG_GPINTENA, 0x00);
      ok &= writeReg(REG_GPINTENB, 0x00);
      ok &= writeReg(REG_GPPUA, 0xFF);
      ok &= writeReg(REG_GPPUB, 0xFF);
      return ok;
    }

    // lists the I2C devices present (only used for the Info page)
    void scanBus() {
      scanBuf[0] = '\0';
      size_t len = 0;
      uint8_t n = 0;
      for (uint8_t a = 0x08; a < 0x78 && n < 8; a++) {
        if (!probeAt(a)) continue;
        len += snprintf(scanBuf + len, sizeof(scanBuf) - len, n ? " %02X" : "%02X", a);
        n++;
      }
    }

    void findChip() {
      scanBus();
      autoAddr = false;
      curAddr = -1;
      if (probeAt(addr)) {
        curAddr = addr;
      } else {
        for (int a = 0x20; a <= 0x27; a++) {            // other possible MCP23017 addresses
          if (probeAt(a)) { curAddr = a; autoAddr = true; break; }
        }
      }
      present = (curAddr >= 0) && initChip();
      lastRetry = millis();
    }

    void scanEncoder(unsigned long now) {
      if (!encInit) {
        pinMode(ENCODER_DT_PIN, INPUT_PULLUP);
        pinMode(ENCODER_CLK_PIN, INPUT_PULLUP);
        pinMode(ENCODER_SW_PIN, INPUT_PULLUP);
        encPrevA = digitalRead(ENCODER_DT_PIN);
        encInit = true;
      }
      if (now == encLastMs) return;                       // samples at most once per millisecond
      encLastMs = now;

      // rotation: same logic as the ALT usermod (rising edge of DT, CLK decides the direction)
      bool a = digitalRead(ENCODER_DT_PIN);
      bool b = digitalRead(ENCODER_CLK_PIN);
      if (a && !encPrevA && (now - encLastEdge) >= 3) {   // 3 ms filter against bounce
        encLastEdge = now;
        ConsoleBus::cc(ConsoleBus::CC_ENCODER, b ? 127 : 1);   // relative: 1 = clockwise, 127 = counter-clockwise
      }
      encPrevA = a;

      // encoder button
      bool pressedNow = (digitalRead(ENCODER_SW_PIN) == LOW);
      if (pressedNow == swPressed) { swCand = pressedNow; return; }
      if (swCand != pressedNow) { swCand = pressedNow; swSince = now; return; }
      if (now - swSince >= (unsigned long)debounceMs) {
        swPressed = pressedNow;
        ConsoleBus::note(ConsoleBus::NOTE_ENCODER_SW, pressedNow);
      }
    }

    void initNative() {
      if (nInit) return;
      for (uint8_t i = 0; i < ConsoleBus::NATIVE_COUNT; i++) pinMode(ConsoleBus::nativePin(i), INPUT_PULLUP);
      nInit = true;
    }

    // Reads the pin of a native button (to GND) and, at the FIRST "pressed" sample of a press,
    // remembers whether SHIFT was pressed. Both scanNative() and handleButton() call it: whoever
    // comes first fixes the decision, the other finds it already made, so WLED and this usermod
    // always agree.
    bool macroLow(uint8_t i) {
      initNative();
      bool low = (digitalRead(ConsoleBus::nativePin(i)) == LOW);
      if (!low) nShiftAtPress[i] = false;
      else if (!nLow[i]) nShiftAtPress[i] = (i < 8) && pressed[BTN_SHIFT];
      nLow[i] = low;
      return low;
    }

    // index of the native button (0..8) that WLED calls "button b", -1 if it is not one of ours
    int nativeIndexOf(uint8_t b) const {
      if (b >= buttons.size()) return -1;
      int8_t pin = buttons[b].pin;
      for (uint8_t i = 0; i < ConsoleBus::NATIVE_COUNT; i++)
        if (ConsoleBus::nativePin(i) == pin) return i;
      return -1;
    }

    // next preset (dir = +1) or previous (-1) in the range stepFirst..stepLast, wrapping around.
    // If the last command had no effect (preset does not exist), the next press
    // starts from that number and skips ahead.
    void stepPreset(int dir) {
      int lo = stepFirst < 1 ? 1 : stepFirst;
      int hi = stepLast > 250 ? 250 : stepLast;
      if (hi < lo) return;
      int n = hi - lo + 1;
      int base = (lastTarget != 0 && currentPreset == lastCurAtApply) ? lastTarget : currentPreset;
      int next;
      if (base < lo || base > hi) next = (dir > 0) ? lo : hi;       // out of range (or no preset): first / last
      else                        next = lo + ((base - lo + dir + n) % n);
      lastCurAtApply = currentPreset;
      lastTarget = (uint8_t)next;
      applyPreset((uint8_t)next, CALL_MODE_BUTTON_PRESET);
    }

    void fireShiftMacro(uint8_t i) {
      if (i < 8 && shiftPreset[i] > 0 && shiftPreset[i] < 251)
        applyPreset((uint8_t)shiftPreset[i], CALL_MODE_BUTTON_PRESET);
    }

    void scanNative(unsigned long now) {
      initNative();
      for (uint8_t i = 0; i < ConsoleBus::NATIVE_COUNT; i++) {
        bool pressedNow = macroLow(i);
        if (pressedNow == nPressed[i]) { nCand[i] = pressedNow; continue; }
        if (nCand[i] != pressedNow) { nCand[i] = pressedNow; nSince[i] = now; continue; }
        if (now - nSince[i] >= (unsigned long)debounceMs) {
          nPressed[i] = pressedNow;
          ConsoleBus::note(ConsoleBus::noteForNative(i), pressedNow);          // USB mode only
          if (pressedNow && i < 8 && pressed[BTN_SHIFT]) shiftUsed = true;     // cancels SHIFT's mode change
          if (pressedNow && nShiftAtPress[i] && !ConsoleBus::usbActive()) fireShiftMacro(i);
        }
      }
    }

    void onButton(uint8_t id, bool isPressed) {
      lastId = id;
      lastPressed = isPressed;
      eventCount++;
      DEBUG_PRINTF("[console_io] %s %s\n", btnName(id), isPressed ? "pressed" : "released");
      const bool usb = ConsoleBus::usbActive();
      if (id == BTN_SHIFT) {
        ConsoleBus::bus().shift = isPressed;       // other usermods read it (SHIFT + color fader = palette)
        if (isPressed) { shiftSince = millis(); shiftFired = false; shiftUsed = false; ConsoleBus::bus().shiftUsed = false; }
        else if (shiftFired) { shiftFired = false; return; }   // release after a long press: no note
      }
      // standalone: BACK = previous preset (immediately), GO = next (on release, if the press was
      // short: held down it opens the network information, see loop())
      if (!usb && id == BTN_BACK && isPressed) stepPreset(-1);
      if (id == BTN_GO) {
        if (isPressed) { goSince = millis(); goFired = false; }
        else {
          bool wasLong = goFired;
          goFired = false;
          if (!wasLong && !usb) stepPreset(+1);
        }
      }
      static const uint8_t notes[NUM_BTN] = { ConsoleBus::NOTE_SHIFT, ConsoleBus::NOTE_BACK, ConsoleBus::NOTE_GO };
      ConsoleBus::note(notes[id], isPressed);          // USB mode only
    }

  public:
    void setup() override {
      present = false;
      noI2c = false;
      if (!enabled) return;
      if (i2c_sda < 0 || i2c_scl < 0) {
        noI2c = true;
        DEBUG_PRINTLN(F("[console_io] global I2C not configured"));
        return;
      }
      findChip();
    }

    void loop() override {
      if (!enabled) return;
      unsigned long now = millis();

      // macro and BLACKOUT buttons: this usermod always reads them. In USB mode it sends them as MIDI notes;
      // in standalone it only handles SHIFT + macro (other presses are left to WLED, see handleButton)
      if (now - lastScan >= POLL_MS) { lastScan = now; scanNative(now); }
      if (ConsoleBus::usbActive()) {
        scanEncoder(now);
      } else {
        encInit = false; swPressed = false; swCand = false;
      }

      if (noI2c) return;

      if (!present) {                                   // chip not found: retry every 5 s
        if (now - lastRetry >= RETRY_MS) findChip();
        return;
      }

      if (now - lastPoll < POLL_MS) return;
      lastPoll = now;

      uint8_t a;
      if (!readPortA(a)) {
        if (++failCount >= 5) { present = false; failCount = 0; lastRetry = now; ConsoleBus::bus().shift = false; }
        return;
      }
      failCount = 0;

      for (uint8_t id = 0; id < NUM_BTN; id++) {
        bool raw = !(a & (1 << id));                    // button to GND = pressed
        if (raw == pressed[id]) { cand[id] = raw; continue; }
        if (cand[id] != raw) { cand[id] = raw; candSince[id] = now; continue; }
        if (now - candSince[id] >= (unsigned long)debounceMs) {
          pressed[id] = raw;
          onButton(id, raw);
        }
      }

      // GO held down (standalone): network information on the display
      if (pressed[BTN_GO] && !goFired && !ConsoleBus::usbActive() && (now - goSince >= (unsigned long)goHoldMs)) {
        goFired = true;
        ConsoleBus::Bus& b = ConsoleBus::bus();
        if (b.showNetwork) b.showNetwork();
      }

      // SHIFT held down: switches mode (USB <-> standalone WLED), in both directions.
      // Cancelled if a macro was pressed during the hold, or another usermod used SHIFT as a modifier
      // (SHIFT + color fader): SHIFT is then a modifier, not a mode switch.
      if (ConsoleBus::bus().shiftUsed) {
        ConsoleBus::bus().shiftUsed = false;
        if (pressed[BTN_SHIFT]) shiftUsed = true;
      }
      if (pressed[BTN_SHIFT] && !shiftFired && !shiftUsed && (now - shiftSince >= (unsigned long)modeHoldMs)) {
        shiftFired = true;
        ConsoleBus::note(ConsoleBus::NOTE_SHIFT, false);       // in USB mode, closes the note before switching
        ConsoleBus::Bus& b = ConsoleBus::bus();
        if (b.toggleMode) b.toggleMode();
      }
    }

    // WLED calls this function for every configured button, BEFORE handling it.
    // We answer true ("I handle it", WLED does not look at it) in two cases:
    //  - USB mode: all buttons go out over MIDI;
    //  - standalone: macro 1..8 pressed while SHIFT is pressed (SHIFT + macro). The decision holds until
    //    the button is released, even if SHIFT is let go earlier: this way WLED sees neither the press
    //    nor the release and does not start the normal preset. BLACKOUT always stays with WLED.
    bool handleButton(uint8_t b) override {
      if (!enabled) return false;
      if (ConsoleBus::usbActive()) return true;
      int i = nativeIndexOf(b);
      if (i < 0 || i >= 8) return false;
      macroLow((uint8_t)i);
      return nLow[i] && nShiftAtPress[i];
    }

    void addToJsonInfo(JsonObject& root) override {
      if (!enabled) return;
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");
      JsonArray arr = user.createNestedArray(FPSTR(_name));
      char buf[72];
      if (noI2c) {
        snprintf(buf, sizeof(buf), "Global I2C not set");
      } else if (!present) {
        snprintf(buf, sizeof(buf), "MCP23017 not found. On the bus: %s", scanBuf[0] ? scanBuf : "no devices");
      } else {
        snprintf(buf, sizeof(buf), "SHIFT%s BACK%s GO%s @%02X%s",
                 pressed[BTN_SHIFT] ? "*" : "", pressed[BTN_BACK] ? "*" : "", pressed[BTN_GO] ? "*" : "",
                 curAddr, autoAddr ? " (auto)" : "");
      }
      arr.add(String(buf));
      arr.add(F("(* = pressed)"));
    }

    void addToConfig(JsonObject& root) override {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top["enabled"]     = enabled;
      top["i2c-address"] = addr;
      top["debounce-ms"] = debounceMs;
      top["mode-hold-ms"] = modeHoldMs;
      top["go-hold-ms"]   = goHoldMs;
      top["first-preset"] = stepFirst;       // BACK / GO step through the presets from first-preset to last-preset
      top["last-preset"]  = stepLast;
      char key[16];
      for (uint8_t n = 0; n < 8; n++) {      // SHIFT + macro n -> preset number (0 = no action)
        snprintf(key, sizeof(key), "shift-preset-%u", n + 1);
        top[key] = shiftPreset[n];
      }
    }

    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root[FPSTR(_name)];
      bool ok = !top.isNull();
      ok &= getJsonValue(top["enabled"],     enabled, true);
      ok &= getJsonValue(top["i2c-address"], addr, 0x27);
      ok &= getJsonValue(top["debounce-ms"], debounceMs, 15);
      ok &= getJsonValue(top["mode-hold-ms"], modeHoldMs, 3000);
      ok &= getJsonValue(top["go-hold-ms"],   goHoldMs, 3000);
      ok &= getJsonValue(top["first-preset"], stepFirst, 1);
      ok &= getJsonValue(top["last-preset"],  stepLast, 16);
      char key[16];
      for (uint8_t n = 0; n < 8; n++) {
        snprintf(key, sizeof(key), "shift-preset-%u", n + 1);
        ok &= getJsonValue(top[key], shiftPreset[n], 0);
        if (shiftPreset[n] < 0 || shiftPreset[n] > 250) shiftPreset[n] = 0;
      }
      return ok;
    }
};

const char ConsoleIO::_name[] PROGMEM = "Console-IO";

static ConsoleIO console_io;
REGISTER_USERMOD(console_io);
