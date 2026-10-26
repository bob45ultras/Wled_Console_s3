/******************************************************************************
SparkFunDMX.cpp  —  "console" version (replaces WLED's)

Destination:  WLED-main\wled00\src\dependencies\dmx\SparkFunDMX.cpp

What changes compared to the original:
  1. It does NOT block WLED's loop. The original called Serial.begin/end + delay(1) + flush()
     on every update(), i.e. ~24 ms of stall on every loop pass: the encoder (read by polling)
     lost steps and the effects frame rate collapsed. Here the line is driven by a
     dedicated FreeRTOS task; update() only copies the buffer (microseconds).
  2. Configurable TX pin (-D DMX_TX_PIN=17) instead of a fixed GPIO2.
  3. Uses the ESP-IDF UART driver directly on UART2 (no HardwareSerial begin/end per
     frame: the line stays high between one frame and the next).
  4. write() ignores channels > 512 (the original wrote past the end of the buffer).
  5. "Bridge" mode for the USB bridge (ENTTEC): see SparkFunDMX.h.

DMX512 frame timing:
  BREAK  : one 0x00 byte at 83333 baud (8N2) = 9 bits at 0 = 108 us, followed by 2 stop bits = MAB 24 us
  DATA   : start code + slots at 250000 baud, 8N2
  A full frame (513 bytes) lasts ~23 ms; the period is kept at >= 25 ms (<= 40 Hz).

Base: SparkFun ESP32 LED to DMX Shield, Andy England @ SparkFun Electronics, MIT License.
******************************************************************************/

#if !defined(ESP8266) && !defined(CONFIG_IDF_TARGET_ESP32C3) && !defined(CONFIG_IDF_TARGET_ESP32C5) && !defined(CONFIG_IDF_TARGET_ESP32C6) && !defined(CONFIG_IDF_TARGET_ESP32C61) && !defined(CONFIG_IDF_TARGET_ESP32S2)

#include "SparkFunDMX.h"
#include <Arduino.h>
#include <string.h>
#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DMX_PORT        UART_NUM_2      // UART1 is reserved for esp_dmx (input, see dmx_input.cpp)
#define DMXSPEED        250000
#define BREAKSPEED      83333
#define DMX_MAX_SLOTS   512
#define DMX_FRAME_MAX   (DMX_MAX_SLOTS + 1)       // start code + 512 slots
#define DMX_MIN_FRAME   26                        // start code + 25 slots (ENTTEC minimum)
#define DMX_PERIOD_US   25000                     // minimum period between two frames (<= 40 Hz)

static uint8_t  s_shadow[DMX_FRAME_MAX] = { 0 };  // buffer written by WLED
static uint8_t  s_tx[DMX_FRAME_MAX]     = { 0 };  // last published frame (read by the task)
static uint16_t s_txLen = DMX_FRAME_MAX;
static volatile bool     s_bridge = false;
static volatile uint32_t s_frames = 0;
static TaskHandle_t      s_task   = nullptr;
static portMUX_TYPE      s_mux    = portMUX_INITIALIZER_UNLOCKED;

static void dmxTxTask(void*) {
  static uint8_t frame[DMX_FRAME_MAX];
  const uint8_t zero = 0;

  for (;;) {
    int64_t t0 = esp_timer_get_time();

    uint16_t n;
    portENTER_CRITICAL(&s_mux);
    n = s_txLen;
    memcpy(frame, s_tx, n);
    portEXIT_CRITICAL(&s_mux);

    // BREAK + MAB
    uart_wait_tx_done(DMX_PORT, pdMS_TO_TICKS(50));
    uart_set_baudrate(DMX_PORT, BREAKSPEED);
    uart_write_bytes(DMX_PORT, (const char*)&zero, 1);
    uart_wait_tx_done(DMX_PORT, pdMS_TO_TICKS(10));

    // start code + slots
    uart_set_baudrate(DMX_PORT, DMXSPEED);
    uart_write_bytes(DMX_PORT, (const char*)frame, n);
    uart_wait_tx_done(DMX_PORT, pdMS_TO_TICKS(50));
    s_frames = s_frames + 1;

    // keeps the minimum period (short frames = more frequent, but never above 40 Hz)
    int64_t elapsed = esp_timer_get_time() - t0;
    int64_t rest = DMX_PERIOD_US - elapsed;
    vTaskDelay(rest > 1000 ? pdMS_TO_TICKS((uint32_t)(rest / 1000)) : 1);
  }
}

void SparkFunDMX::initWrite(int chanQuant) {
  if (s_task) return;                                   // already started
  if (chanQuant <= 0 || chanQuant > DMX_MAX_SLOTS) chanQuant = DMX_MAX_SLOTS;
  s_txLen = (uint16_t)(chanQuant + 1);

  uart_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.baud_rate  = DMXSPEED;
  cfg.data_bits  = UART_DATA_8_BITS;
  cfg.parity     = UART_PARITY_DISABLE;
  cfg.stop_bits  = UART_STOP_BITS_2;
  cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
#if ESP_IDF_VERSION_MAJOR >= 5
  cfg.source_clk = UART_SCLK_DEFAULT;
#else
  cfg.source_clk = UART_SCLK_APB;
#endif

  // rx buffer > hardware FIFO (128 bytes) required by the driver; tx buffer 0 = blocking write in the task
  esp_err_t e = uart_driver_install(DMX_PORT, 256, 0, 0, NULL, 0);
  if (e == ESP_OK) e = uart_param_config(DMX_PORT, &cfg);
  if (e == ESP_OK) e = uart_set_pin(DMX_PORT, DMX_TX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  if (e != ESP_OK) {
    log_e("DMX out: UART2 init failed (%d)", (int)e);
    return;
  }

  // priority 2, core 0: like the DMX receive task. WLED's loop (core 1) is not touched.
  xTaskCreatePinnedToCore(dmxTxTask, "DMX_TX", 3072, nullptr, 2, &s_task, 0);
  if (!s_task) log_e("DMX out: task not created");
}

void SparkFunDMX::write(int Channel, uint8_t value) {
  if (s_bridge) return;                                 // the line belongs to the PC
  if (Channel < 0 || Channel > DMX_MAX_SLOTS) return;
  s_shadow[0] = 0;                                      // start code
  s_shadow[Channel] = value;
}

void SparkFunDMX::update() {
  if (s_bridge) return;
  portENTER_CRITICAL(&s_mux);
  memcpy(s_tx, s_shadow, DMX_FRAME_MAX);
  s_txLen = DMX_FRAME_MAX;
  portEXIT_CRITICAL(&s_mux);
}

void SparkFunDMX::bridgeMode(bool on) { s_bridge = on; }
bool SparkFunDMX::bridgeActive() const { return s_bridge; }
uint32_t SparkFunDMX::framesSent() const { return s_frames; }

void SparkFunDMX::bridgeFrame(const uint8_t* frame, uint16_t len) {
  if (!s_bridge || !frame || len == 0) return;
  if (len > DMX_FRAME_MAX) len = DMX_FRAME_MAX;
  portENTER_CRITICAL(&s_mux);
  memcpy(s_tx, frame, len);
  if (len < DMX_MIN_FRAME) {                            // frame too short: pad with zeros
    memset(s_tx + len, 0, DMX_MIN_FRAME - len);
    len = DMX_MIN_FRAME;
  }
  s_txLen = len;
  portEXIT_CRITICAL(&s_mux);
}

#endif
