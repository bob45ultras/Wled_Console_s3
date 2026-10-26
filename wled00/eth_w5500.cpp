#include "wled.h"
#include "fcn_declare.h"
#include "wled_ethernet.h"

/*
 * eth_w5500.cpp — SPI Ethernet (WIZnet W5500) for WLED main on ESP32-S3.
 *
 * WLED's own Ethernet code (network.cpp) only knows RMII PHYs (LAN8720 & co.), which the S3 does not
 * have. This file replaces it when the build defines both
 *
 *     -D WLED_USE_ETHERNET        -D WLED_USE_ETHERNET_SPI
 *
 * and provides what the rest of WLED expects from the Ethernet support: initEthernet(), the table of
 * reserved pins, and the ethernetType setting (0 = none, 1 = W5500). The driver itself is the one
 * built into the Arduino core (ETH.begin with ETH_PHY_W5500), so link/IP events, hostname, DHCP and
 * static IP are handled by WLED exactly as for any other Ethernet board.
 *
 * Pins (build flags, see platformio_override.ini):
 *   WLED_W5500_SCLK, WLED_W5500_MOSI, WLED_W5500_MISO, WLED_W5500_CS, WLED_W5500_RST   mandatory
 *   WLED_W5500_INT      interrupt pin; -1 (default) = the driver polls every 10 ms
 *   WLED_W5500_SPI_MHZ  SPI clock, default 20
 *
 * RST/INT on the UART0 pins (GPIO43/44 on the S3): allowed. UART0 is released first (Serial.end()),
 * so neither the serial log nor the boot-time Serial.begin() state can fight for the pins afterwards.
 * The ROM bootloader still prints on GPIO43 for the first ~100 ms after reset: the W5500 is reset
 * by the driver after that, so those few glitches on its RST input are harmless.
 */

#if defined(ARDUINO_ARCH_ESP32) && defined(WLED_USE_ETHERNET) && defined(WLED_USE_ETHERNET_SPI)

#include <algorithm>
#include "driver/gpio.h"
#include "driver/spi_master.h"

#if !defined(WLED_W5500_SCLK) || !defined(WLED_W5500_MOSI) || !defined(WLED_W5500_MISO) || !defined(WLED_W5500_CS) || !defined(WLED_W5500_RST)
  #error "W5500 needs  -D WLED_W5500_SCLK / _MOSI / _MISO / _CS / _RST  (and optionally _INT, _SPI_MHZ) in platformio_override.ini"
#endif
#ifndef WLED_W5500_INT
  #define WLED_W5500_INT -1
#endif
#ifndef WLED_W5500_SPI_MHZ
  #define WLED_W5500_SPI_MHZ 20
#endif

#ifdef WLED_ETH_DEFAULT
  static_assert(((WLED_ETH_DEFAULT) >= WLED_ETH_NONE) && ((WLED_ETH_DEFAULT) < WLED_NUM_ETH_TYPES), "WLED_ETH_DEFAULT is out of range.");
#endif

// Pins shown as reserved in the settings pages (same role as the RMII list of the classic ESP32).
// With no INT pin the CS pin is listed twice rather than a -1 entry.
const managed_pin_type esp32_nonconfigurable_ethernet_pins[WLED_ETH_RSVD_PINS_COUNT] = {
  { WLED_W5500_SCLK, true  },
  { WLED_W5500_MOSI, true  },
  { WLED_W5500_MISO, false },
  { WLED_W5500_CS,   true  },
  { (WLED_W5500_INT >= 0) ? WLED_W5500_INT : WLED_W5500_CS, false },
  { WLED_W5500_RST,  true  },
};

// Diagnostics shown by the console_eth usermod on the Info page
int     ethW5500Version = -2;   // -2 = not probed, -1 = SPI setup failed, else the value read from VERSIONR (0x04 = W5500)
int     ethW5500Phy     = -2;   // PHYCFGR (0x2E), a W5500 answers 0xB8..0xBF; -2 = not read
uint8_t ethW5500Raw[4]  = { 0, 0, 0, 0 };  // the four bytes clocked back by the version read (the last one is the register)
int8_t  ethW5500MisoPd  = -1;   // level of the MISO pin with the pull-down / pull-up while the chip is idle:
int8_t  ethW5500MisoPu  = -1;   //   0/1 = floating (normal), 0/0 = held low, 1/1 = held high
char    ethW5500Pins[40] = "";  // per-wire electrical check: "SCLK ok MOSI ok CS ok RST ok MISO ok" (L = stuck low, H = stuck high, x = coupled)
int8_t  ethW5500Map[4]  = { -1, -1, -1, -1 };  // SCLK, MOSI, MISO, CS that made the chip answer when the configured ones did not
uint8_t ethW5500Stage   = 0;    // 0 = not run, 1 = pin allocation failed, 2 = ETH.begin() failed, 3 = started

// One raw read of a W5500 common register over SPI2 with the given pins (mode 0, 1 MHz).
// Frame: address (2 bytes), control byte 0x00 (common register block, read, variable length), then the data byte.
// Returns the register value, or -1 when the SPI driver could not be set up. The bus is released again afterwards:
// ETH.begin() sets it up with DMA.
static int w5500ReadReg(int sclk, int mosi, int miso, int cs, uint16_t addr, uint8_t* raw = nullptr)
{
  spi_bus_config_t bus;
  memset(&bus, 0, sizeof(bus));
  bus.mosi_io_num = mosi;
  bus.miso_io_num = miso;
  bus.sclk_io_num = sclk;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 16;
  esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED);
  bool ownBus = (err == ESP_OK);                              // ESP_ERR_INVALID_STATE: someone else already set the bus up, use it
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return -1;

  int result = -1;
  spi_device_interface_config_t dev;
  memset(&dev, 0, sizeof(dev));
  dev.mode = 0;
  dev.clock_speed_hz = 1000000;                               // slow and safe for a wiring check
  dev.spics_io_num = cs;
  dev.queue_size = 1;
  spi_device_handle_t h = nullptr;
  if (spi_bus_add_device(SPI2_HOST, &dev, &h) == ESP_OK) {
    uint8_t tx[4] = { (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF), 0x00, 0x00 };
    uint8_t rx[4] = { 0, 0, 0, 0 };
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 32;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    if (spi_device_polling_transmit(h, &t) == ESP_OK) result = rx[3];
    if (raw) memcpy(raw, rx, 4);
    spi_bus_remove_device(h);
  }
  if (ownBus) spi_bus_free(SPI2_HOST);
  return result;
}

// Electrical check of the five wires, done before anything talks to the chip. SCLK, MOSI, CS and RST are driven one
// at a time high against the others low, then low against the others high, and every pin is read back: a pin that
// cannot follow is stuck (short to GND / 3.3 V), one that follows another is bridged to it. MISO is only read
// (it is the chip's output), with a pull-down in the first phase and a pull-up in the second.
static void w5500PinTest()
{
  const gpio_num_t out[4] = { (gpio_num_t)WLED_W5500_SCLK, (gpio_num_t)WLED_W5500_MOSI, (gpio_num_t)WLED_W5500_CS, (gpio_num_t)WLED_W5500_RST };
  const gpio_num_t miso = (gpio_num_t)WLED_W5500_MISO;
  const char* const names[5] = { "SCLK", "MOSI", "CS", "RST", "MISO" };
  char state[5] = { 'o', 'o', 'o', 'o', 'o' };               // o = ok, L = stuck low, H = stuck high, x = follows another pin

  for (int i = 0; i < 4; i++) { gpio_reset_pin(out[i]); gpio_set_direction(out[i], GPIO_MODE_INPUT_OUTPUT); }
  gpio_reset_pin(miso);
  gpio_set_direction(miso, GPIO_MODE_INPUT);

  for (int phase = 0; phase < 2; phase++) {                    // phase 0: one pin high, the rest low; phase 1: the opposite
    gpio_set_pull_mode(miso, phase == 0 ? GPIO_PULLDOWN_ONLY : GPIO_PULLUP_ONLY);
    for (int p = 0; p < 4; p++) {
      for (int i = 0; i < 4; i++) gpio_set_level(out[i], (i == p) == (phase == 0));
      delay(1);
      for (int i = 0; i < 4; i++) {
        int expected = (i == p) == (phase == 0);
        int level = gpio_get_level(out[i]);
        if (level != expected) {
          char found = (i == p) ? (expected ? 'L' : 'H') : 'x';
          if (state[i] == 'o' || found != 'x') state[i] = found;      // a stuck level is more telling than "follows another pin"
        }
      }
      bool csHigh = ((p == 2) == (phase == 0));                    // only look at MISO while the chip is deselected
      if (csHigh && gpio_get_level(miso) != (phase == 1)) state[4] = phase == 0 ? 'H' : 'L';   // it must follow the pull
    }
  }
  gpio_set_pull_mode(miso, GPIO_FLOATING);
  char* w = ethW5500Pins;
  for (int i = 0; i < 5; i++) {
    w += snprintf(w, sizeof(ethW5500Pins) - (w - ethW5500Pins), "%s%s %s", i ? " " : "", names[i], state[i] == 'o' ? "ok" : (state[i] == 'L' ? "L" : (state[i] == 'H' ? "H" : "x")));
  }
}

// Resets the chip and reads VERSIONR (0x0039): a healthy W5500 answers 0x04. Fills the diagnostics above.
// When the configured pins get no answer, the four SPI wires are tried in every other role assignment, so a
// swapped or misplaced wire shows up as "the chip answers with SCLK=.. MOSI=.. MISO=.. CS=..".
static int w5500ReadVersion()
{
  w5500PinTest();                                             // also takes GPIO43 away from UART0 if RST sits there

  gpio_reset_pin((gpio_num_t)WLED_W5500_CS);                  // chip deselected during the reset and the idle test
  gpio_set_direction((gpio_num_t)WLED_W5500_CS, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)WLED_W5500_CS, 1);
  gpio_reset_pin((gpio_num_t)WLED_W5500_RST);
  gpio_set_direction((gpio_num_t)WLED_W5500_RST, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)WLED_W5500_RST, 0);
  delay(5);                                                   // datasheet: at least 500 us low
  gpio_set_level((gpio_num_t)WLED_W5500_RST, 1);
  delay(100);                                                 // PLL lock after reset

  // Idle level of MISO: the chip leaves it high-impedance while it is not selected, so it must follow the pulls
  gpio_reset_pin((gpio_num_t)WLED_W5500_MISO);
  gpio_set_direction((gpio_num_t)WLED_W5500_MISO, GPIO_MODE_INPUT);
  gpio_set_pull_mode((gpio_num_t)WLED_W5500_MISO, GPIO_PULLDOWN_ONLY);
  delay(2);
  ethW5500MisoPd = gpio_get_level((gpio_num_t)WLED_W5500_MISO);
  gpio_set_pull_mode((gpio_num_t)WLED_W5500_MISO, GPIO_PULLUP_ONLY);
  delay(2);
  ethW5500MisoPu = gpio_get_level((gpio_num_t)WLED_W5500_MISO);
  gpio_set_pull_mode((gpio_num_t)WLED_W5500_MISO, GPIO_FLOATING);

  int version = w5500ReadReg(WLED_W5500_SCLK, WLED_W5500_MOSI, WLED_W5500_MISO, WLED_W5500_CS, 0x0039, ethW5500Raw);
  ethW5500Phy = w5500ReadReg(WLED_W5500_SCLK, WLED_W5500_MOSI, WLED_W5500_MISO, WLED_W5500_CS, 0x002E);

  if (version != 0x04) {
    const int wires[4] = { WLED_W5500_SCLK, WLED_W5500_MOSI, WLED_W5500_MISO, WLED_W5500_CS };
    int idx[4] = { 0, 1, 2, 3 };
    do {
      if (idx[0] == 0 && idx[1] == 1 && idx[2] == 2 && idx[3] == 3) continue;   // the configured one, already tried
      if (w5500ReadReg(wires[idx[0]], wires[idx[1]], wires[idx[2]], wires[idx[3]], 0x0039) == 0x04) {
        for (int k = 0; k < 4; k++) ethW5500Map[k] = (int8_t)wires[idx[k]];
        break;
      }
    } while (std::next_permutation(idx, idx + 4));
  }
  return version;
}

bool initEthernet()
{
  static bool successfullyConfiguredEthernet = false;

  if (successfullyConfiguredEthernet) return false;
  if (ethernetType == WLED_ETH_NONE) return false;
  if (ethernetType != WLED_ETH_W5500) {
    DEBUG_PRINTF_P(PSTR("initE: Ignoring attempt for invalid ethernetType (%d)\n"), ethernetType);
    return false;
  }
  DEBUG_PRINTLN(F("initE: Attempting W5500 (SPI) config"));

  // The pins to allocate: the INT entry only when INT is used (no duplicate pin in the array)
  managed_pin_type pinsToAllocate[WLED_ETH_RSVD_PINS_COUNT];
  byte nPins = 0;
  for (unsigned i = 0; i < WLED_ETH_RSVD_PINS_COUNT; i++) {
    if (i == 4 && WLED_W5500_INT < 0) continue;
    pinsToAllocate[nPins++] = esp32_nonconfigurable_ethernet_pins[i];
  }

  // Release UART0 first if RST or INT sit on its pins (Serial was started in WLED::setup()).
  // With the native USB in OTG mode (no CDC on boot) Serial is UART0; with CDC on boot it is USB and stays.
  #if !ARDUINO_USB_CDC_ON_BOOT
  for (byte i = 0; i < nPins; i++) {
    if (pinsToAllocate[i].pin == hardwareRX || pinsToAllocate[i].pin == hardwareTX) { Serial.end(); break; }
  }
  #endif

  if (!PinManager::allocateMultiplePins(pinsToAllocate, nPins, PinOwner::Ethernet)) {
    DEBUG_PRINTLN(F("initE: Failed to allocate ethernet pins"));
    ethW5500Stage = 1;
    return false;
  }

  ethW5500Version = w5500ReadVersion();             // diagnostics only: the result does not stop the start-up
  DEBUG_PRINTF_P(PSTR("initE: W5500 version register = %d (0x04 expected)\n"), ethW5500Version);

  if (!ETH.begin(ETH_PHY_W5500, 1,                 // PHY address is not used by the W5500
                 WLED_W5500_CS, WLED_W5500_INT, WLED_W5500_RST,
                 SPI2_HOST, WLED_W5500_SCLK, WLED_W5500_MISO, WLED_W5500_MOSI,
                 WLED_W5500_SPI_MHZ)) {
    DEBUG_PRINTLN(F("initE: ETH.begin() failed"));
    ethW5500Stage = 2;
    PinManager::deallocateMultiplePins(pinsToAllocate, nPins, PinOwner::Ethernet);
    return false;
  }

  // https://github.com/wled/WLED/issues/5247
  if (multiWiFi.size() && multiWiFi[0].staticIP != IPAddress() && multiWiFi[0].staticGW != IPAddress()) {
    ETH.config(multiWiFi[0].staticIP, multiWiFi[0].staticGW, multiWiFi[0].staticSN, dnsAddress);
  } else {
    ETH.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
  }

  successfullyConfiguredEthernet = true;
  ethW5500Stage = 3;
  DEBUG_PRINTLN(F("initE: *** W5500 Ethernet successfully configured! ***"));
  return true;
}

#endif
