// ES8311 codec on the Waveshare ESP32-S3-Touch-LCD-1.85C: the speaker DAC.
// Just enough to bring it up as an I2S slave at 48 kHz, 16-bit, with its MCLK
// from the pin, DAC at 0 dB; the radio's volume is applied in software.
// Register values follow Espressif's es8311 driver for 12.288 MHz / 48 kHz.
#pragma once
#include <Arduino.h>
#include <Wire.h>

static bool es8311Write(uint8_t addr, uint8_t reg, uint8_t v) {
  Wire.beginTransmission(addr);
  Wire.write(reg); Wire.write(v);
  return Wire.endTransmission() == 0;
}

static bool es8311Init(uint8_t addr = 0x18) {
  Wire.beginTransmission(addr);
  if (Wire.endTransmission() != 0) return false;
  const uint8_t seq[][2] = {
    {0x00, 0x1F},             // reset
    {0x00, 0x00},
    {0x00, 0x80},             // power on, slave
    {0x01, 0x3F},             // all clocks on, MCLK from the MCLK pin
    {0x02, 0x00},             // pre-divider 1, multiplier 1
    {0x03, 0x10},             // single speed, ADC OSR
    {0x04, 0x10},             // DAC OSR
    {0x05, 0x00},             // ADC/DAC dividers 1
    {0x06, 0x03},             // BCLK divider 4
    {0x07, 0x00},             // LRCK divider 256 (0x00FF + 1)
    {0x08, 0xFF},
    {0x09, 0x10},             // SDP in: I2S, 32-bit slots (what the decoder sends)
    {0x0A, 0x10},             // SDP out: I2S, 32-bit
    {0x0D, 0x01},             // power up analogue
    {0x0E, 0x02},             // enable analogue PGA, ADC modulator
    {0x12, 0x00},             // DAC power on
    {0x13, 0x10},             // headphone/line drive
    {0x1C, 0x6A},             // ADC equalizer bypass
    {0x37, 0x08},             // DAC ramp rate
    {0x32, 0xBF},             // DAC volume 0 dB
    {0x31, 0x00},             // DAC unmute
  };
  for (auto& r : seq) {
    if (!es8311Write(addr, r[0], r[1])) return false;
    if (r[0] == 0x00 && r[1] == 0x1F) delay(20);
  }
  return true;
}
