// SPDX-License-Identifier: EUPL-1.2
// Copyright (c) 2026 E-Boats Systems (https://www.eboatsystems.com)

#include "wled.h"
#include "console_bus.h"
#include "enttec_pro.h"

#ifndef WLED_ENABLE_DMX
  #error "console_dmx requires  -D WLED_ENABLE_DMX  and  -D DMX_TX_PIN=17  in platformio_override.ini"
#endif

/*
 * console_dmx — the console's DMX, in two modes:
 *
 *  STANDALONE (WLED mode)
 *    - output : WLED's DMX driver (SparkFunDMX, non-blocking version), TX on GPIO17
 *    - input  : WLED's esp_dmx, RX on GPIO18 (fixed-direction RS485 module: no TX/EN pins).
 *      This usermod sets the input pins at every boot ("force-pins" option); the rest
 *      (DMX mode, address) is adjusted as usual in Config > Sync Interfaces.
 *
 *  USB MODE (PC connected)
 *    - the console's COM port becomes an "ENTTEC DMX USB Pro":
 *        PC -> console : DMX frames (label 6)  -> RS485 output (GPIO17)
 *        console -> PC : DMX frames received on GPIO18 (label 5), only when they change (+ refresh every 250 ms)
 *    - WLED's DMX is disabled: no frame enters the effects engine and the DMX output does not
 *      show the pixels.
 *
 * Note on USB: the COM port's TX buffer is only 64 bytes (CONFIG_TINYUSB_CDC_TX_BUFSIZE),
 * so a ~520-byte frame is written in ~9 USB packets and Print::write() waits for the PC.
 * To avoid stalling WLED's loop, all writes towards the PC go through a dedicated task.
 *
 * Required flags:  WLED_ENABLE_DMX, DMX_TX_PIN=17   (WLED_ENABLE_DMX_INPUT is already active on the S3)
 * Core patch: see patch_dmx.txt (dmx_input.cpp, wled.cpp, xml.cpp, SparkFunDMX.*).
 */

#ifndef CONSOLE_DMX_RX_PIN
  #define CONSOLE_DMX_RX_PIN 18
#endif
#ifndef CONSOLE_DMX_PORT
  #define CONSOLE_DMX_PORT 1          // UART1: UART2 is used by the output (SparkFunDMX)
#endif

using namespace EnttecPro;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task = nullptr;

// ---- frames received from the physical DMX (WLED receive task, core 0) -> USB task ----------------
static uint8_t       s_rx[FRAME_MAX];
static uint16_t      s_rxLen  = 0;
static bool          s_rxNew  = false;
static bool          s_rxLost = false;          // a frame was replaced before being sent

// ---- replies to the PC's commands (loop) -> USB task: queue of 4 packets ----------------------------
enum : uint8_t { REPLY_SLOTS = 4 };
static uint8_t       s_reply[REPLY_SLOTS][PACKET_MAX];
static uint16_t      s_replyLen[REPLY_SLOTS];
static uint8_t       s_replyHead = 0, s_replyCount = 0;

static volatile uint32_t s_toPc    = 0;         // DMX frames delivered to the PC
static volatile uint32_t s_dropped = 0;         // packets the PC did not read in time

static void onDmxFrame(const uint8_t* d, uint16_t len) {
  if (len > FRAME_MAX) len = FRAME_MAX;
  portENTER_CRITICAL(&s_mux);
  memcpy(s_rx, d, len);
  s_rxLen = len;
  if (s_rxNew) s_rxLost = true;
  s_rxNew = true;
  portEXIT_CRITICAL(&s_mux);
  if (s_task) xTaskNotifyGive(s_task);
}

static void queueReply(const uint8_t* pkt, uint16_t n) {
  portENTER_CRITICAL(&s_mux);
  if (s_replyCount < REPLY_SLOTS) {
    uint8_t slot = (uint8_t)((s_replyHead + s_replyCount) % REPLY_SLOTS);
    memcpy(s_reply[slot], pkt, n);
    s_replyLen[slot] = n;
    s_replyCount++;
  }
  portEXIT_CRITICAL(&s_mux);
  if (s_task) xTaskNotifyGive(s_task);
}

static void sendToPc(Stream* p, const uint8_t* d, uint16_t n, bool isFrame) {
  size_t w = p->write(d, n);                    // waits for the PC (timeout set in console_usb)
  if (w == n) { if (isFrame) s_toPc = s_toPc + 1; }
  else s_dropped = s_dropped + 1;
}

// Single task that writes to the COM port: replies and received DMX frames
static void usbTxTask(void*) {
  static uint8_t pkt[PACKET_MAX + 8];
  static uint8_t frame[FRAME_MAX];
  static uint8_t last[FRAME_MAX];
  uint16_t lastLen = 0;
  uint32_t lastTx = 0;
  bool wasOpen = false;

  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
    ConsoleBus::Bus& b = ConsoleBus::bus();
    Stream* p = b.dmxPort;
    bool open = b.usb && p && b.dmxOpen && b.dmxOpen();
    if (open != wasOpen) { wasOpen = open; lastLen = 0; }   // port just opened: the PC immediately receives the state

    // 1) replies to commands
    for (;;) {
      uint16_t rl = 0;
      portENTER_CRITICAL(&s_mux);
      if (s_replyCount) {
        rl = s_replyLen[s_replyHead];
        memcpy(pkt, s_reply[s_replyHead], rl);
        s_replyHead = (uint8_t)((s_replyHead + 1) % REPLY_SLOTS);
        s_replyCount--;
      }
      portEXIT_CRITICAL(&s_mux);
      if (!rl) break;
      if (open) sendToPc(p, pkt, rl, false);
    }

    // 2) received DMX frame: to the PC only if it changed (or every 250 ms as a refresh)
    uint16_t len = 0; uint8_t status = 0;
    portENTER_CRITICAL(&s_mux);
    if (s_rxNew) {
      len = s_rxLen; memcpy(frame, s_rx, len);
      status = s_rxLost ? 0x02 : 0x00;          // bit1 = overrun
      s_rxNew = false; s_rxLost = false;
    }
    portEXIT_CRITICAL(&s_mux);
    if (len && open) {
      uint32_t now = millis();
      bool changed = (len != lastLen) || memcmp(frame, last, len) != 0;
      if (changed || (uint32_t)(now - lastTx) >= 250) {
        sendToPc(p, pkt, buildRxDmx(pkt, frame, len, status), true);
        memcpy(last, frame, len); lastLen = len; lastTx = now;
      }
    }
  }
}

class ConsoleDmx : public Usermod {
  private:
    bool enabled   = true;
    bool forcePins = true;                       // sets the DMX input pins at every boot
    int  inPin     = CONSOLE_DMX_RX_PIN;         // -1 = DMX input disabled
    int  inPort    = CONSOLE_DMX_PORT;

    bool bridgeOn = false;
    Parser parser;
    uint32_t pcFrames = 0;                       // DMX frames received from the PC
    uint8_t  outBuf[PACKET_MAX + 8];
    static const char _name[];

    void reply(uint16_t n) { queueReply(outBuf, n); }

    void handlePacket() {
      switch (parser.label()) {
        case LBL_TX_DMX:                                  // PC -> DMX output
          dmx.bridgeFrame(parser.data(), parser.length());
          pcFrames++;
          break;
        case LBL_PARAMS_GET: {                            // parameters request
          uint16_t user = parser.length() >= 2 ? (uint16_t)(parser.data()[0] | (parser.data()[1] << 8)) : 0;
          reply(buildParams(outBuf, user));
          break;
        }
        case LBL_SERIAL: {                                // serial number derived from the MAC
          uint32_t sn = (uint32_t)(ESP.getEfuseMac() % 100000000ULL);
          reply(buildSerial(outBuf, sn));
          break;
        }
        default: break;                                   // 4 (set params), 8 (on change), RDM...: ignored
      }
    }

  public:
    void setup() override {
      if (!enabled) return;
#ifdef WLED_ENABLE_DMX_INPUT
      if (forcePins) {                                    // runs before dmxInput.init() in WLED::setup()
        dmxInputReceivePin  = inPin;
        dmxInputTransmitPin = -1;                         // fixed-direction RS485: no TX/EN pin on the input
        dmxInputEnablePin   = -1;
        dmxInputPort        = inPort;
      }
#endif
      // core 0 (like the DMX receive), priority 1: the USB write may wait for the PC without touching the loop
      xTaskCreatePinnedToCore(usbTxTask, "DMX_USB", 4096, nullptr, 1, &s_task, 0);
      ConsoleBus::bus().dmxRx = onDmxFrame;
    }

    void loop() override {
      if (!enabled) return;
      ConsoleBus::Bus& b = ConsoleBus::bus();
      bool usb = b.usb;

      if (usb != bridgeOn) {                              // entering/leaving USB mode
        bridgeOn = usb;
        dmx.bridgeMode(usb);
        parser.reset();
        portENTER_CRITICAL(&s_mux);
        s_rxNew = false; s_rxLost = false; s_replyCount = 0;
        portEXIT_CRITICAL(&s_mux);
      }

      Stream* p = b.dmxPort;
      if (!p) return;

      if (!usb) {                                         // standalone: the COM port is not needed, it is drained
        for (int i = 0; i < 512 && p->available() > 0; i++) p->read();
        return;
      }

      // PC -> console
      for (int i = 0; i < 4096 && p->available() > 0; i++) {
        int c = p->read();
        if (c < 0) break;
        if (parser.feed((uint8_t)c)) handlePacket();
      }
    }

    void addToJsonInfo(JsonObject& root) override {
      if (!enabled) return;
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");
      JsonArray arr = user.createNestedArray(FPSTR(_name));
      char buf[96];
      if (bridgeOn) {
        snprintf(buf, sizeof(buf), "ENTTEC USB: PC>DMX %lu, DMX>PC %lu, dropped %lu",
                 (unsigned long)pcFrames, (unsigned long)s_toPc, (unsigned long)s_dropped);
      } else {
#ifdef WLED_ENABLE_DMX_INPUT
        snprintf(buf, sizeof(buf), "standalone, input %s, output %lu frames",
                 inPin < 0 ? "off" : (dmxInput.isConnected() ? "receiving" : "no signal"),
                 (unsigned long)dmx.framesSent());
#else
        snprintf(buf, sizeof(buf), "standalone, output %lu frames", (unsigned long)dmx.framesSent());
#endif
      }
      arr.add(String(buf));
      arr.add(F(""));
    }

    void addToConfig(JsonObject& root) override {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top["enabled"]     = enabled;
      top["force-pins"]  = forcePins;     // true: the DMX input pins are enforced from here (below)
      top["dmx-in-pin"]  = inPin;         // RX GPIO of the input RS485 module; -1 = input disabled
      top["dmx-in-port"] = inPort;        // esp_dmx UART: 1 (2 belongs to the output)
    }

    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root[FPSTR(_name)];
      bool ok = !top.isNull();
      ok &= getJsonValue(top["enabled"],     enabled,   true);
      ok &= getJsonValue(top["force-pins"],  forcePins, true);
      ok &= getJsonValue(top["dmx-in-pin"],  inPin,     CONSOLE_DMX_RX_PIN);
      ok &= getJsonValue(top["dmx-in-port"], inPort,    CONSOLE_DMX_PORT);
      if (inPort < 1 || inPort > 2) inPort = CONSOLE_DMX_PORT;
      return ok;
    }
};

const char ConsoleDmx::_name[] PROGMEM = "Console-DMX";

static ConsoleDmx console_dmx;
REGISTER_USERMOD(console_dmx);
