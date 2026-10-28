#pragma once
#include <stdint.h>

class Stream;   // Arduino: the COM port of the DMX bridge is seen as a Stream

/*
 * console_bus.h — state shared between the console usermods
 * Copy it to WLED\wled00\ (core folder: it is on the include path of every usermod).
 *
 *   console_usb    : creates the USB device, decides whether the console is in "USB mode" and
 *                    registers the functions that send MIDI
 *   console_io     : MCP23017 buttons + native WLED buttons
 *   console_faders : faders
 *   console_dmx    : standalone DMX (pins) + ENTTEC DMX USB Pro bridge in USB mode
 *
 * In USB mode (PC connected and active) the controls NO LONGER drive WLED:
 * they only send MIDI. Outside USB mode the note()/cc() functions do nothing.
 */
namespace ConsoleBus {

// ---- MIDI map (everything on channel 1) ----------------------------------------------
enum : uint8_t {
  CC_FADER_BASE   = 16,   // CC16..19 = faders 1..4, value 0..127
  CC_ENCODER      = 30,   // encoder rotation, relative: 1 = clockwise, 127 = counter-clockwise
  NOTE_MACRO_BASE = 36,   // notes 36..43 = macro buttons 1..8
  NOTE_BLACKOUT   = 44,   // BLACKOUT (GPIO3)
  NOTE_SHIFT      = 48,   // MCP23017 PA0
  NOTE_BACK       = 49,   // MCP23017 PA1
  NOTE_GO         = 50,   // MCP23017 PA2
  NOTE_ENCODER_SW = 51    // encoder button
};

struct Bus {
  volatile bool     usb      = false;     // true: PC connected and active -> USB mode
  volatile uint32_t usbEpoch = 0;         // +1 at every activation of USB mode (to resynchronize the PC)
  volatile bool     shift     = false;    // SHIFT (MCP23017 PA0) is pressed, debounced; written by console_io
  volatile bool     shiftUsed = false;    // a usermod used SHIFT as a modifier (e.g. SHIFT + color fader):
                                          // console_io then cancels the "hold SHIFT = switch mode" action
  void (*sendNote)(uint8_t note, bool on)       = nullptr;
  void (*sendCC)(uint8_t cc, uint8_t value)     = nullptr;
  void (*toggleMode)()                          = nullptr;   // manual switch USB <-> standalone (SHIFT held down)
  void (*showNetwork)()                         = nullptr;   // shows the network information on the display (standalone only)

  // ---- DMX <-> USB bridge (ENTTEC DMX USB Pro) ----
  Stream* dmxPort                               = nullptr;   // USB COM port (created by console_usb)
  bool  (*dmxOpen)()                            = nullptr;   // true if the PC has opened the COM port (DTR)
  // called by the DMX receive task (core 0, from dmx_input.cpp) with a complete frame
  // (data[0] = start code, then the slots) while USB mode is active
  void (*dmxRx)(const uint8_t* data, uint16_t len) = nullptr;
};

inline Bus& bus() { static Bus b; return b; }
inline bool usbActive() { return bus().usb; }

inline void note(uint8_t n, bool on)   { Bus& b = bus(); if (b.usb && b.sendNote) b.sendNote(n, on); }
inline void cc(uint8_t c, uint8_t v)   { Bus& b = bus(); if (b.usb && b.sendCC)   b.sendCC(c, v); }

// Native buttons wired directly to GPIOs: 8 macros + BLACKOUT.
// In USB mode console_io reads them and sends them as MIDI notes.
enum : uint8_t { NATIVE_COUNT = 9 };
inline int8_t nativePin(uint8_t i) {
  static const int8_t pins[NATIVE_COUNT] = {16, 38, 39, 40, 41, 42, 45, 46, 3};   // the last one is BLACKOUT
  return pins[i];
}
inline uint8_t noteForNative(uint8_t i) {
  return (uint8_t)(i < 8 ? NOTE_MACRO_BASE + i : NOTE_BLACKOUT);
}

}  // namespace ConsoleBus
