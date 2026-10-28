/******************************************************************************
SparkFunDMX.h  —  "console" version (replaces WLED's)

Destination:  WLED-main\wled00\src\dependencies\dmx\SparkFunDMX.h

Same interface used by WLED (initWrite / write / update), but:
  - update() NO LONGER blocks WLED's loop (the original stalled ~24 ms on every pass):
    transmission is done by a dedicated task that refreshes the DMX line at ~40 Hz;
  - the TX pin is configurable:  -D DMX_TX_PIN=17   (the original was fixed on GPIO2);
  - "bridge" mode: used by the USB bridge (ENTTEC). When active, WLED's write()/update()
    are ignored and the DMX line is driven by bridgeFrame().

Original: SparkFun ESP32 LED to DMX Shield library, Andy England @ SparkFun Electronics, 7/22/2019,
released under the MIT License (http://opensource.org/licenses/MIT), distributed as-is, no warranty.
Modifications: Copyright (c) 2026 E-Boats Systems (https://www.eboatsystems.com), released under the same MIT License.
******************************************************************************/

#include <inttypes.h>

#ifndef SparkFunDMX_h
#define SparkFunDMX_h

#define DMX_SEND_ONLY // transmit only (reception is done by esp_dmx, see dmx_input.cpp)

#ifndef DMX_TX_PIN
  #define DMX_TX_PIN 2   // WLED's original default; the console uses -D DMX_TX_PIN=17
#endif

class SparkFunDMX {
public:
  void initWrite(int maxChan);                 // creates the UART2 driver + transmit task
  void write(int channel, uint8_t value);      // channel 1..512 (like the original)
  void update();                               // publishes the buffer to the task: does not block

  // ---- USB bridge (ENTTEC) ----
  void     bridgeMode(bool on);                // true: the line is driven by the PC
  bool     bridgeActive() const;
  void     bridgeFrame(const uint8_t* frame, uint16_t len);  // frame[0] = start code, then the slots; max len 513
  uint32_t framesSent() const;                 // counter of transmitted DMX frames (diagnostics)
};

#endif
