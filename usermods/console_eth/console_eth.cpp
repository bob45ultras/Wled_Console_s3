// SPDX-License-Identifier: EUPL-1.2
// Copyright (c) 2026 E-Boats Systems

#include "wled.h"

/*
 * console_eth — shows the state of the W5500 Ethernet module on WLED's Info page.
 *
 * Why: in the Ethernet build UART0 (GPIO43/44) belongs to the W5500 (RST/INT), so there is no serial
 * log to look at during the first bring-up. This row tells at a glance what is wrong.
 *
 * Only active in the console_s3_eth environment (WLED_USE_ETHERNET_SPI, see eth_w5500.cpp);
 * in every other build this file compiles to nothing.
 */

#if defined(ARDUINO_ARCH_ESP32) && defined(WLED_USE_ETHERNET) && defined(WLED_USE_ETHERNET_SPI)

// diagnostics from eth_w5500.cpp
extern int     ethW5500Version, ethW5500Phy;
extern uint8_t ethW5500Raw[4];
extern int8_t  ethW5500MisoPd, ethW5500MisoPu;
extern int8_t  ethW5500Map[4];
extern char    ethW5500Pins[40];
extern uint8_t ethW5500Stage;

class ConsoleEth : public Usermod {
  private:
    static const char _name[];

  public:
    void setup() override {}
    void loop() override {}

    void addToJsonInfo(JsonObject& root) override {
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");
      JsonArray arr = user.createNestedArray(FPSTR(_name));

      char buf[160];
      if (ethernetType != WLED_ETH_W5500) {
        strcpy_P(buf, PSTR("disabled (Config > WiFi Setup)"));
      } else if (!ETH.started()) {
        if (ethW5500Stage == 1) {
          strcpy_P(buf, PSTR("not started: pins already in use"));
        } else if (ethW5500Version == 0x04) {
          strcpy_P(buf, PSTR("chip OK (v4) but driver init failed"));
        } else if (ethW5500Version == -1) {
          strcpy_P(buf, PSTR("SPI bus setup failed"));
        } else if (ethW5500Map[0] >= 0) {
          snprintf_P(buf, sizeof(buf), PSTR("wires differ: chip answers with SCLK=%d MOSI=%d MISO=%d CS=%d"),
                     ethW5500Map[0], ethW5500Map[1], ethW5500Map[2], ethW5500Map[3]);
        } else {
          snprintf_P(buf, sizeof(buf), PSTR("no answer: ver %02X%02X%02X%02X phy %02X, MISO idle %d/%d | %s"),
                     ethW5500Raw[0], ethW5500Raw[1], ethW5500Raw[2], ethW5500Raw[3], ethW5500Phy & 0xFF,
                     (int)ethW5500MisoPd, (int)ethW5500MisoPu, ethW5500Pins);
        }
      } else if (!ETH.linkUp()) {
        strcpy_P(buf, PSTR("link down (cable?)"));
      } else if (!ETH.hasIP()) {
        snprintf_P(buf, sizeof(buf), PSTR("link %u Mbps %s, waiting for IP"),
                   (unsigned)ETH.linkSpeed(), ETH.fullDuplex() ? "full" : "half");
      } else {
        snprintf_P(buf, sizeof(buf), PSTR("%u Mbps %s, %s"),
                   (unsigned)ETH.linkSpeed(), ETH.fullDuplex() ? "full" : "half", ETH.localIP().toString().c_str());
      }
      arr.add(String(buf));
      arr.add(F(""));
    }
};

const char ConsoleEth::_name[] PROGMEM = "Console-ETH";

static ConsoleEth console_eth;
REGISTER_USERMOD(console_eth);

#endif
