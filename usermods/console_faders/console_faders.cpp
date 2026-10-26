#include "wled.h"
#include "console_bus.h"

/*
 * console_faders — 4 analog faders (ADC1) -> WLED parameters in real time
 *
 *   Fader 1 (GPIO1) -> Brightness
 *   Fader 2 (GPIO2) -> Effect speed
 *   Fader 3 (GPIO4) -> Effect intensity
 *   Fader 4 (GPIO5) -> Primary color (hue, full saturation)
 *
 * Reads the ADC directly in loop() (no native "Analog Button", which is throttled).
 *
 * Each fader has two states:
 *   INACTIVE  - writes nothing to WLED. Whatever you change from the web UI, encoder, presets,
 *               apps etc. is never overwritten, even if the fader sits at a position that
 *               differs from the current value.
 *   ACTIVE    - becomes active when the fader moves by more than "touch" ADC counts
 *               (about 1.5% of the travel). From then on it drives WLED in real time.
 *               After "release-ms" milliseconds without movement it goes back to INACTIVE.
 * At boot every fader starts INACTIVE (no value jump at power-up).
 * When a fader takes control, the value jumps to the fader position.
 *
 * Calibration: the raw ADC values are shown on the WLED Info page
 * ("Console-Faders"). Note the value with the fader fully down and fully up,
 * then enter them in Config > Usermods as raw-min / raw-max.
 */

class ConsoleFaders : public Usermod {
  private:
    enum { NUM = 4 };
    enum : uint8_t { FN_BRI = 0, FN_SPEED = 1, FN_INTENSITY = 2, FN_COLOR = 3 };

    static const unsigned long SAMPLE_MS = 8;    // sampling period
    static const unsigned long APPLY_MS  = 25;   // max ~40 updates/s towards WLED

    // --- configuration (Config > Usermods) ---
    bool enabled = true;
    bool invert  = true;  // true = inverted faders (up = maximum value)
    int  pins[NUM] = {1, 2, 4, 5};
    int  rawMin = 40;     // ADC reading with the fader fully down
    int  rawMax = 4050;   // ADC reading with the fader fully up (the S3 ADC saturates at ~3.1V)
    int  rawMid = 0;      // ADC reading at MID travel; 0 = disabled (linear fader)
                          // needed for logarithmic-taper faders (taper "A")
    int  hyst   = 14;     // hysteresis in ADC counts, against jitter
    int  touchThr  = 60;   // movement (ADC counts) that "wakes up" an inactive fader
    int  releaseMs = 1500; // standing still for this long -> the fader goes back to inactive

    // --- runtime state ---
    bool     active[NUM]     = {false, false, false, false};
    bool     seeded[NUM]     = {false, false, false, false};
    int32_t  ema[NUM]        = {0, 0, 0, 0};      // moving average, scale x16
    uint16_t rawVal[NUM]     = {0, 0, 0, 0};
    int16_t  target[NUM]     = {0, 0, 0, 0};
    int16_t  appliedRaw[NUM] = {0, 0, 0, 0};
    int16_t  appliedVal[NUM] = {-1, -1, -1, -1};
    bool     touched[NUM]    = {false, false, false, false};
    int32_t  restRaw16[NUM]  = {0, 0, 0, 0};      // rest position (inactive), scale x16
    int16_t  lastMoveRaw[NUM]= {0, 0, 0, 0};
    unsigned long lastMoveMs[NUM] = {0, 0, 0, 0};
    unsigned long lastSample = 0;
    unsigned long lastApply  = 0;
    uint32_t seenEpoch = 0;                       // last USB activation already synchronized
    int16_t  lastCC[NUM] = {-1, -1, -1, -1};      // last MIDI value sent (no duplicate messages)

    static const char _name[];

    static uint32_t hueToColor(uint8_t v) {
      uint16_t hue = (uint16_t)v * 359 / 255;                 // 0..359
      uint8_t region = hue / 60;
      uint8_t rise = (uint8_t)(((hue % 60) * 255) / 60);
      uint8_t fall = 255 - rise;
      uint8_t r, g, b;
      switch (region) {
        case 0:  r = 255;  g = rise; b = 0;    break;
        case 1:  r = fall; g = 255;  b = 0;    break;
        case 2:  r = 0;    g = 255;  b = rise; break;
        case 3:  r = 0;    g = fall; b = 255;  break;
        case 4:  r = rise; g = 0;    b = 255;  break;
        default: r = 255;  g = 0;    b = fall; break;
      }
      return RGBW32(r, g, b, 0);
    }

    static void writeSeg(Segment& seg, uint8_t fn, uint8_t v) {
      switch (fn) {
        case FN_SPEED:     seg.speed = v;                    break;
        case FN_INTENSITY: seg.intensity = v;                break;
        case FN_COLOR:     seg.colors[0] = hueToColor(v);    break;
        default: break;
      }
    }

    void applyValue(uint8_t fn, uint8_t v) {
      if (fn == FN_BRI) {
        bri = v ? v : 1;          // the fader never switches off: use BLACKOUT / power to turn off
        return;
      }
      uint8_t n = strip.getSegmentsNum();
      bool any = false;
      for (uint8_t s = 0; s < n; s++) {
        Segment& seg = strip.getSegment(s);
        if (!seg.isActive() || !seg.isSelected()) continue;
        writeSeg(seg, fn, v);
        any = true;
      }
      if (!any) writeSeg(strip.getMainSegment(), fn, v);
    }

    // ADC reading -> position 0..255. With rawMid set it uses two linear sections
    // (min->mid = first half of the travel, mid->max = second half), which is how
    // audio-taper potentiometers are built.
    int mapPos(int raw) const {
      int lo = rawMin;
      int hi = (rawMax > lo + 100) ? rawMax : lo + 100;
      int v;
      if (rawMid > lo && rawMid < hi) {
        if (raw <= rawMid) v = (raw - lo) * 127 / (rawMid - lo);
        else               v = 127 + (raw - rawMid) * 128 / (hi - rawMid);
      } else {
        v = (raw - lo) * 255 / (hi - lo);
      }
      if (v < 0) v = 0;
      if (v > 255) v = 255;
      return v;
    }

    bool moved(uint8_t i) const {
      if (abs((int)rawVal[i] - (int)appliedRaw[i]) >= hyst) return true;
      // makes sure the end stops (0 / 255) are always reached
      return (target[i] != appliedVal[i]) && (target[i] == 0 || target[i] == 255);
    }

  public:
    void setup() override {
      if (!enabled) return;
      for (uint8_t i = 0; i < NUM; i++) {
        active[i] = false;
        seeded[i] = false;
        touched[i] = false;
        if (pins[i] < 0) continue;
        if (!PinManager::allocatePin(pins[i], false, PinOwner::UM_Unspecified)) {
          DEBUG_PRINTF("[console_faders] GPIO%d already in use, fader %u disabled\n", pins[i], i + 1);
          continue;
        }
        pinMode(pins[i], INPUT);
        analogSetPinAttenuation(pins[i], ADC_11db);
        active[i] = true;
      }
    }

    void loop() override {
      if (!enabled) return;
      unsigned long now = millis();
      if (now - lastSample < SAMPLE_MS) return;
      lastSample = now;

      bool pending = false;
      for (uint8_t i = 0; i < NUM; i++) {
        if (!active[i]) continue;

        uint32_t acc = 0;
        for (uint8_t k = 0; k < 4; k++) acc += analogRead(pins[i]);
        int32_t sample16 = (int32_t)(acc * 4);                 // average of 4 readings, scale x16

        if (!seeded[i]) ema[i] = sample16;
        else            ema[i] += (sample16 - ema[i]) / 4;     // low-pass filter

        rawVal[i] = (uint16_t)(ema[i] / 16);
        int v = mapPos((int)rawVal[i]);
        if (invert) v = 255 - v;
        target[i] = (int16_t)v;

        if (!seeded[i]) {                                      // first sample: no jump at startup
          appliedRaw[i]  = (int16_t)rawVal[i];
          appliedVal[i]  = (int16_t)v;
          restRaw16[i]   = (int32_t)rawVal[i] * 16;
          lastMoveRaw[i] = (int16_t)rawVal[i];
          lastMoveMs[i]  = now;
          touched[i]     = false;
          seeded[i]      = true;
          continue;
        }

        if (!touched[i]) {
          // INACTIVE: the fader does not touch WLED. It only wakes up with a deliberate movement.
          int restRaw = (int)(restRaw16[i] / 16);
          int thr = (touchThr > hyst + 10) ? touchThr : hyst + 10;
          if (abs((int)rawVal[i] - restRaw) < thr) {
            // slow drift (temperature, noise): the rest position follows it slowly
            restRaw16[i] += ((int32_t)rawVal[i] * 16 - restRaw16[i]) / 256;
            continue;
          }
          // takes control: from here on the fader drives WLED until it stands still
          touched[i]     = true;
          appliedRaw[i]  = (int16_t)restRaw;                   // so that moved() is true right away
          lastMoveRaw[i] = (int16_t)rawVal[i];
          lastMoveMs[i]  = now;
        } else {
          if (abs((int)rawVal[i] - (int)lastMoveRaw[i]) >= hyst) {
            lastMoveRaw[i] = (int16_t)rawVal[i];
            lastMoveMs[i]  = now;
          } else if (now - lastMoveMs[i] >= (unsigned long)releaseMs) {
            // still for long enough: goes back to inactive, WLED is free again
            touched[i]   = false;
            restRaw16[i] = (int32_t)rawVal[i] * 16;
            continue;
          }
        }
        if (moved(i)) pending = true;
      }

      // when the PC connects, immediately send the position of every fader (so the program aligns itself)
      uint32_t ep = ConsoleBus::bus().usbEpoch;
      if (ep != seenEpoch) {
        seenEpoch = ep;
        if (ConsoleBus::usbActive()) {
          for (uint8_t i = 0; i < NUM; i++)
            if (active[i] && seeded[i]) {
              lastCC[i] = (int16_t)(target[i] >> 1);
              ConsoleBus::cc((uint8_t)(ConsoleBus::CC_FADER_BASE + i), (uint8_t)lastCC[i]);
            }
        }
      }

      if (pending && (now - lastApply >= APPLY_MS)) {
        bool usb = ConsoleBus::usbActive();
        bool wledChanged = false;
        for (uint8_t i = 0; i < NUM; i++) {
          if (!active[i] || !seeded[i] || !touched[i] || !moved(i)) continue;
          if (usb) {
            int16_t v7 = (int16_t)(target[i] >> 1);                                              // 0..255 -> 0..127
            if (v7 != lastCC[i]) { lastCC[i] = v7; ConsoleBus::cc((uint8_t)(ConsoleBus::CC_FADER_BASE + i), (uint8_t)v7); }
          } else {
            applyValue(i, (uint8_t)target[i]);
            wledChanged = true;
          }
          appliedRaw[i] = (int16_t)rawVal[i];
          appliedVal[i] = target[i];
        }
        lastApply = now;
        if (wledChanged) {
          // without stateChanged, WLED only updates the web UI / WebSocket / MQTT / sync and clears the
          // current preset when the brightness changes: speed, intensity and color (written into the
          // segments) were only visible on the display, which reads the globals updated by stateUpdated().
          stateChanged = true;
          stateUpdated(CALL_MODE_DIRECT_CHANGE);
        }
      }
    }

    void addToJsonInfo(JsonObject& root) override {
      if (!enabled) return;
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");
      JsonArray arr = user.createNestedArray(FPSTR(_name));
      char buf[40];
      snprintf(buf, sizeof(buf), "%u%s %u%s %u%s %u%s",
               rawVal[0], touched[0] ? "*" : "", rawVal[1], touched[1] ? "*" : "",
               rawVal[2], touched[2] ? "*" : "", rawVal[3], touched[3] ? "*" : "");
      arr.add(String(buf));
      arr.add(F("raw (* = active)"));
    }

    void addToConfig(JsonObject& root) override {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top["enabled"]        = enabled;
      top["invert"]         = invert;
      top["pin-brightness"] = pins[0];
      top["pin-speed"]      = pins[1];
      top["pin-intensity"]  = pins[2];
      top["pin-color"]      = pins[3];
      top["raw-min"]        = rawMin;
      top["raw-max"]        = rawMax;
      top["raw-mid"]        = rawMid;
      top["hysteresis"]     = hyst;
      top["touch"]          = touchThr;
      top["release-ms"]     = releaseMs;
    }

    // pins changed from the UI take effect after a reboot
    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root[FPSTR(_name)];
      bool ok = !top.isNull();
      ok &= getJsonValue(top["enabled"],        enabled, true);
      ok &= getJsonValue(top["invert"],         invert, true);
      ok &= getJsonValue(top["pin-brightness"], pins[0], 1);
      ok &= getJsonValue(top["pin-speed"],      pins[1], 2);
      ok &= getJsonValue(top["pin-intensity"],  pins[2], 4);
      ok &= getJsonValue(top["pin-color"],      pins[3], 5);
      ok &= getJsonValue(top["raw-min"],        rawMin, 40);
      ok &= getJsonValue(top["raw-max"],        rawMax, 4050);
      ok &= getJsonValue(top["raw-mid"],        rawMid, 0);
      ok &= getJsonValue(top["hysteresis"],     hyst, 14);
      ok &= getJsonValue(top["touch"],          touchThr, 60);
      ok &= getJsonValue(top["release-ms"],     releaseMs, 1500);
      return ok;
    }
};

const char ConsoleFaders::_name[] PROGMEM = "Console-Faders";

static ConsoleFaders console_faders;
REGISTER_USERMOD(console_faders);
