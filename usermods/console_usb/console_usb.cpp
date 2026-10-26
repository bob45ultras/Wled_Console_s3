#include "wled.h"
#include "console_bus.h"
#include "USB.h"
#include "USBMIDI.h"
#include "USBCDC.h"

// WLED 4-line display (four_line_display_ALT usermod), if present in the build.
// Its U8x8 object and its data are "private": we need them to draw the USB logo,
// so the class declaration is read with private -> public (for this file only).
#if __has_include("usermod_v2_four_line_display.h") && !defined(CONSOLE_NO_4LD)
  #define CONSOLE_HAS_4LD 1       // to exclude it (if it gives compile errors): -D CONSOLE_NO_4LD
  #define private public
  #include "usermod_v2_four_line_display.h"
  #undef private
#endif

/*
 * console_usb — the console as a composite USB device: MIDI + one serial port.
 *
 * Configuration tested in the usb_test project (midi_only environment): Windows shows
 * "TinyUSB MIDI" and a COM port. The COM port is the ENTTEC DMX bridge, handled by console_dmx.
 *
 * USB mode: active when the PC has configured the device and it is not suspended,
 * stable for at least 300 ms. While active the controls only send MIDI (see console_bus.h).
 *
 * Requires in platformio_override.ini:  ARDUINO_USB_MODE=0  and  ARDUINO_USB_CDC_ON_BOOT=0.
 * The WLED log therefore goes out on the UART (the devkit's "UART" port), not on the native USB.
 */

static USBMIDI MidiDev;
static USBCDC  PortDmx(0);                 // COM port: ENTTEC DMX bridge (used by console_dmx through console_bus.h)

static volatile bool s_mounted   = false;  // the PC has configured the device
static volatile bool s_suspended = false;  // the PC is suspended
static volatile uint32_t s_sent  = 0;      // MIDI messages sent (diagnostics)
static bool s_manualStandalone = false;    // chosen by holding SHIFT: ignores the PC until repeated
static bool s_forceStandalone  = false;    // copy of the "force-standalone" configuration
enum : uint8_t { MSG_NONE = 0, MSG_STANDALONE, MSG_NOPC };
static uint8_t s_pendingMsg = MSG_NONE;    // short message to show on the display

static void usbEvent(void*, esp_event_base_t base, int32_t id, void*) {
  if (base != ARDUINO_USB_EVENTS) return;
  switch (id) {
    case ARDUINO_USB_STARTED_EVENT: s_mounted = true;  s_suspended = false; break;
    case ARDUINO_USB_STOPPED_EVENT: s_mounted = false;                      break;
    case ARDUINO_USB_SUSPEND_EVENT: s_suspended = true;                     break;
    case ARDUINO_USB_RESUME_EVENT:  s_suspended = false;                    break;
    default: break;
  }
}

#ifdef CONSOLE_HAS_4LD
// 32x32 USB logo = 4x4 U8x8 tiles of 8 bytes (one byte per column, bit0 = top pixel), in tile rows
static const uint8_t usbLogo[128] PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0xC0, 0xE0, 0xE0, 0xF0, 0xE0, 0xE0,
  0xC0, 0x80, 0x80, 0xC0, 0xE0, 0xF0, 0xBC, 0x9C,
  0x8C, 0x8C, 0x9F, 0x9F, 0xBF, 0x9F, 0x9F, 0x84,
  0xFE, 0xFC, 0xF8, 0xF0, 0xE0, 0xC0, 0x80, 0x80,
  0x00, 0x01, 0x07, 0x0F, 0x0F, 0x1F, 0x0F, 0x0F,
  0x07, 0x01, 0x01, 0x01, 0x01, 0x03, 0x07, 0x0F,
  0x1D, 0x39, 0x31, 0xF9, 0xF9, 0xF9, 0xF9, 0xF9,
  0x7F, 0x3F, 0x1F, 0x0F, 0x07, 0x03, 0x01, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static FourLineDisplayUsermod* fld() {
  return (FourLineDisplayUsermod*) UsermodManager::lookup(USERMOD_ID_FOUR_LINE_DISP);
}

// Fixed screen: title + logo. Uses the usermod's overlay, which suspends its redrawing of
// the WLED values until it expires or is cancelled. Returns true if it was drawn.
static bool showUsbScreen() {
  FourLineDisplayUsermod* d = fld();
  if (!d || d->type == NONE || !d->enabled || !d->u8x8) return false;
  U8X8* u = d->u8x8;
  const unsigned long HOLD_MS = 12UL * 3600UL * 1000UL;          // 12 hours, then it is redrawn
  uint8_t cols = u->getCols(), rows = u->getRows();
  bool tall = rows >= 8;                                          // 128x64: 8 text rows

  d->overlay(tall ? "MIDI-USB" : nullptr, (long)HOLD_MS, 255);   // clears the screen, title centered at the top
  if (d->overlayUntil == 0 || (long)(d->overlayUntil - millis()) < (long)(HOLD_MS / 2)) return false;   // overlay not drawn (display busy)

  u->setFont(u8x8_font_chroma48medium8_r);
  if (tall) {
    uint8_t col = cols > 4 ? (cols - 4) / 2 : 0;
    for (uint8_t r = 0; r < 4; r++) u->drawTile(col, 2 + r, 4, (uint8_t*)&usbLogo[r * 32]);
    u->drawString(0, 7, "controls via USB");
  } else {                                                        // 128x32: logo on the left, text on the right
    for (uint8_t r = 0; r < 4; r++) u->drawTile(1, r, 4, (uint8_t*)&usbLogo[r * 32]);
    u->drawString(6, 1, "MIDI-USB");
    u->drawString(6, 2, "to PC");
  }
  return true;
}

// Network information (SSID, IP) like the default "NETWORK INFO" screen of the usermod
static void showNetworkImpl() {
  FourLineDisplayUsermod* d = fld();
  if (!d || d->type == NONE || !d->enabled) return;
  d->networkOverlay("NETWORK INFO", 7000);
}

// Short confirmation messages for the manual mode change
static void showMessage(uint8_t msg) {
  FourLineDisplayUsermod* d = fld();
  if (!d || d->type == NONE || !d->enabled) return;
  if (msg == MSG_STANDALONE) d->overlay("STANDALONE", "controls on WLED", 1800);
  else if (msg == MSG_NOPC)  d->overlay("NO PC", "connect USB", 1800);
}

// Back to the normal WLED screen
static void hideUsbScreen() {
  FourLineDisplayUsermod* d = fld();
  if (!d || d->type == NONE || !d->enabled) return;
  d->overlayUntil = 0;
  d->redraw(true);
}
#else
static bool showUsbScreen() { return true; }
static void hideUsbScreen() {}
static void showNetworkImpl() {}
static void showMessage(uint8_t) {}
#endif

// Manual switch USB <-> standalone (called by console_io when SHIFT is held down)
static void toggleModeImpl() {
  if (s_forceStandalone) return;                           // forced by configuration
  if (!s_mounted) { s_pendingMsg = MSG_NOPC; return; }     // no PC connected: nothing to switch
  s_manualStandalone = !s_manualStandalone;
  if (s_manualStandalone) s_pendingMsg = MSG_STANDALONE;   // going back to USB, the USB screen is already the confirmation
}

// The COM port is open in the program on the PC (DTR line active): the DMX bridge needs it to know whether anyone is listening
static bool dmxOpenImpl() { return (bool)PortDmx; }

static void sendNoteImpl(uint8_t note, bool on) {
  if (on) MidiDev.noteOn(note, 100, 1); else MidiDev.noteOff(note, 0, 1);
  s_sent = s_sent + 1;
}
static void sendCCImpl(uint8_t cc, uint8_t value) {
  MidiDev.controlChange(cc, value, 1);
  s_sent = s_sent + 1;
}

class ConsoleUSB : public Usermod {
  private:
    bool enabled         = true;
    bool forceStandalone = false;   // ignores the PC: useful if USB is connected only for development
    bool bootloaderReset = false;   // true: the COM port may reboot the board into download mode (see setup)
    bool cond = false;             // instantaneous "PC connected" condition
    unsigned long condSince = 0;
    bool active = false;
    bool screenShown = false;
    unsigned long screenTry = 0, screenAt = 0;
    static const unsigned long HOLD_MS = 300;
    static const char _name[];

  public:
    void setup() override {
      if (!enabled) return;
      USB.VID(0x303A);
      USB.PID(0x4003);                        // same PID as the midi_only environment (tested)
      USB.productName("Console MIDI+DMX");
      USB.manufacturerName("Console WLED");
      USB.onEvent(usbEvent);
      // Large RX: an ENTTEC frame from the PC weighs ~520 bytes and WLED's loop may stay busy for a few ms.
      // TX: the USB buffer is 64 bytes (not changeable), so a frame towards the PC takes ~9 packets
      // and write() waits for the PC. A dedicated task in console_dmx does it; the timeout (50 ms) prevents
      // it from staying blocked if the program on the PC has the port open but does not read.
      PortDmx.setRxBufferSize(2048);
      PortDmx.setTxTimeoutMs(50);
      // The Arduino core reboots the board into download mode (board "dead" until reset) when the COM port
      // sees esptool's DTR/RTS line sequence or an open at 1200 baud. A DMX program that opens/closes
      // the port can trigger it: it must be disabled for a DMX interface. It can be re-enabled with "bootloader-reset" in
      // Config > Usermods > Console-USB (only needed if you upload firmware over the native USB).
      PortDmx.enableReboot(bootloaderReset);
      PortDmx.begin();
      MidiDev.begin();
      USB.begin();

      ConsoleBus::Bus& b = ConsoleBus::bus();
      b.sendNote   = sendNoteImpl;
      b.sendCC     = sendCCImpl;
      b.toggleMode = toggleModeImpl;
      b.showNetwork = showNetworkImpl;
      b.dmxPort     = &PortDmx;          // the ENTTEC bridge (console_dmx) uses this port
      b.dmxOpen     = dmxOpenImpl;
    }

    void loop() override {
      if (!enabled) return;
      unsigned long now = millis();
      s_forceStandalone = forceStandalone;
      if (!s_mounted) s_manualStandalone = false;             // PC disconnected: the manual switch is reset
      bool want = !forceStandalone && !s_manualStandalone && s_mounted && !s_suspended;
      if (want != cond) { cond = want; condSince = now; }
      bool should = want && (now - condSince >= HOLD_MS);   // activates after 300 ms, deactivates immediately
      if (should != active) {
        active = should;
        ConsoleBus::Bus& b = ConsoleBus::bus();
        b.usb = active;
        if (active) b.usbEpoch = b.usbEpoch + 1;
      }

      // "MIDI-USB" screen on the display while USB mode is active
      if (active) {
        bool refresh = screenShown && (now - screenAt > 6UL * 3600UL * 1000UL);   // redraw every 6 hours
        if ((!screenShown || refresh) && now - screenTry >= 500) {
          screenTry = now;
          if (showUsbScreen()) { screenShown = true; screenAt = now; }
        }
      } else if (screenShown) {
        hideUsbScreen();
        screenShown = false;
      }

      // confirmation message after the manual switch (only once the USB screen has been removed)
      if (s_pendingMsg != MSG_NONE && !screenShown && (s_pendingMsg == MSG_NOPC || !active)) {
        showMessage(s_pendingMsg);
        s_pendingMsg = MSG_NONE;
      }
    }

    void addToJsonInfo(JsonObject& root) override {
      if (!enabled) return;
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");
      JsonArray arr = user.createNestedArray(FPSTR(_name));
      char buf[64];
      const char* st = forceStandalone ? "standalone (forced)"
                     : (s_manualStandalone ? "standalone (manual)"
                     : (active ? "USB mode (MIDI)"
                     : (s_mounted && s_suspended ? "PC suspended"
                     : (s_mounted ? "PC connected, waiting" : "not connected"))));
      snprintf(buf, sizeof(buf), "%s, MIDI sent: %lu", st, (unsigned long)s_sent);
      arr.add(String(buf));
      arr.add(F(""));
    }

    void addToConfig(JsonObject& root) override {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top["enabled"]          = enabled;
      top["force-standalone"] = forceStandalone;
      top["bootloader-reset"] = bootloaderReset;   // a reboot is needed after changing it
    }

    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root[FPSTR(_name)];
      bool ok = !top.isNull();
      ok &= getJsonValue(top["enabled"],          enabled, true);
      ok &= getJsonValue(top["force-standalone"], forceStandalone, false);
      ok &= getJsonValue(top["bootloader-reset"], bootloaderReset, false);
      return ok;
    }
};

const char ConsoleUSB::_name[] PROGMEM = "Console-USB";

static ConsoleUSB console_usb;
REGISTER_USERMOD(console_usb);
