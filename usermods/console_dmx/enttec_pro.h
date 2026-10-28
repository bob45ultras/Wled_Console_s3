// SPDX-License-Identifier: EUPL-1.2
// Copyright (c) 2026 E-Boats Systems

#pragma once
#include <stdint.h>
#include <string.h>

/*
 * enttec_pro.h — "ENTTEC DMX USB Pro" protocol (logic only, no dependency on Arduino:
 * it can also be compiled and tested on a PC).
 *
 * Packet:  0x7E | label | length LSB | length MSB | data[length] | 0xE7
 *
 * Labels handled
 *   3  PC -> console : parameters request (data: user-config size LSB, MSB)
 *      console -> PC : reply (firmware LSB, MSB, break, MAB, rate, user-config...)
 *   4  PC -> console : set parameters                (accepted and ignored)
 *   5  console -> PC : received DMX packet           (status, start code, slots...)
 *   6  PC -> console : send DMX frame                (start code, slots...)
 *   8  PC -> console : "receive DMX only on change"  (accepted; whole frames are sent anyway)
 *  10  PC -> console : serial number
 *      console -> PC : 4 BCD bytes, LSB first
 */
namespace EnttecPro {

enum : uint8_t {
  SOM = 0x7E,
  EOM = 0xE7,
  LBL_PARAMS_GET   = 3,
  LBL_PARAMS_SET   = 4,
  LBL_RX_DMX       = 5,
  LBL_TX_DMX       = 6,
  LBL_RX_ON_CHANGE = 8,
  LBL_SERIAL       = 10
};

constexpr uint16_t MAX_DATA = 600;          // maximum size of the data field (ENTTEC specification)
constexpr uint16_t FRAME_MAX = 513;         // start code + 512 slots
constexpr uint16_t PACKET_MAX = MAX_DATA + 5;

// ---- State-machine parser: fed one byte at a time ---------------------------------------
class Parser {
 public:
  void reset() { st = S_SOM; }

  // true when a complete, correct packet is ready (label/length/data valid until the next feed)
  bool feed(uint8_t b) {
    switch (st) {
      case S_SOM:
        if (b == SOM) st = S_LABEL;
        break;
      case S_LABEL:
        lbl = b; st = S_LEN_L;
        break;
      case S_LEN_L:
        len = b; st = S_LEN_H;
        break;
      case S_LEN_H:
        len |= (uint16_t)b << 8;
        if (len > MAX_DATA) { st = S_SOM; break; }          // absurd length: resynchronize
        got = 0;
        st = len ? S_DATA : S_EOM;
        break;
      case S_DATA:
        buf[got++] = b;
        if (got >= len) st = S_EOM;
        break;
      case S_EOM:
        st = S_SOM;
        if (b == EOM) return true;
        // wrong terminator: if it is a new SOM, restart from there, otherwise discard
        if (b == SOM) st = S_LABEL;
        break;
    }
    return false;
  }

  uint8_t        label()  const { return lbl; }
  uint16_t       length() const { return len; }
  const uint8_t* data()   const { return buf; }

 private:
  enum : uint8_t { S_SOM, S_LABEL, S_LEN_L, S_LEN_H, S_DATA, S_EOM };
  uint8_t  st = S_SOM;
  uint8_t  lbl = 0;
  uint16_t len = 0, got = 0;
  uint8_t  buf[MAX_DATA];
};

// ---- Packet building (out must have at least len + 5 bytes) -----------------------------------
inline uint16_t build(uint8_t* out, uint8_t label, const uint8_t* data, uint16_t len) {
  out[0] = SOM; out[1] = label; out[2] = (uint8_t)(len & 0xFF); out[3] = (uint8_t)(len >> 8);
  if (len) memcpy(out + 4, data, len);
  out[4 + len] = EOM;
  return (uint16_t)(len + 5);
}

// Label 5: status (bit0 = queue full, bit1 = overrun), then the frame (frame[0] = start code)
inline uint16_t buildRxDmx(uint8_t* out, const uint8_t* frame, uint16_t len, uint8_t status) {
  if (len > FRAME_MAX) len = FRAME_MAX;
  uint16_t dl = (uint16_t)(len + 1);
  out[0] = SOM; out[1] = LBL_RX_DMX; out[2] = (uint8_t)(dl & 0xFF); out[3] = (uint8_t)(dl >> 8);
  out[4] = status;
  if (len) memcpy(out + 5, frame, len);
  out[4 + dl] = EOM;
  return (uint16_t)(dl + 5);
}

// Label 3: widget parameters. Firmware 1.44 (like a real DMX USB Pro),
// break = 10 x 10.67us (~107 us), MAB = 2 x 10.67us (~21 us), 40 packets/s.
inline uint16_t buildParams(uint8_t* out, uint16_t userSize) {
  if (userSize > MAX_DATA - 5) userSize = MAX_DATA - 5;
  uint8_t d[MAX_DATA];
  d[0] = 44; d[1] = 1;        // firmware LSB, MSB  (1.44)
  d[2] = 10;                  // break time
  d[3] = 2;                   // MAB time
  d[4] = 40;                  // output rate
  memset(d + 5, 0, userSize);
  return build(out, LBL_PARAMS_GET, d, (uint16_t)(5 + userSize));
}

// Label 10: serial number with 8 decimal digits, 4 BCD bytes, LSB first
inline uint16_t buildSerial(uint8_t* out, uint32_t number) {
  number %= 100000000UL;
  uint8_t d[4];
  for (int i = 0; i < 4; i++) {
    uint8_t lo = number % 10; number /= 10;
    uint8_t hi = number % 10; number /= 10;
    d[i] = (uint8_t)((hi << 4) | lo);
  }
  return build(out, LBL_SERIAL, d, 4);
}

}  // namespace EnttecPro
