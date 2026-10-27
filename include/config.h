// Air Drift for ESP32-S3: pins and defaults, per board. The build env picks
// the board (platformio.ini); this is the file to edit when wiring changes.
#pragma once

#define AD_VERSION    "0.5.1"
#define AD_SITE       "https://airdrift.stream"
#define AD_USER_AGENT "AirDrift-ESP32/" AD_VERSION " (+https://airdrift.stream)"
// Where releases live: version.txt says what the newest is, and
// airdrift-<env>.bin is its firmware for this board (see .github/workflows/release.yml).
#define UPDATE_BASE   "https://github.com/Air-Drift/ESP32-S3/releases/latest/download/"
#ifndef AD_ENV
#define AD_ENV        "devkit"
#endif

#if defined(BOARD_WAVESHARE_185)
// ======================================================================
// Waveshare ESP32-S3-Touch-LCD-1.85: everything is on the board.
// ======================================================================
#define BOARD_NAME   "Waveshare ESP32-S3-Touch-LCD-1.85"
#define HAS_DISPLAY  1

// Speaker. The 1.85 has a PCM5101 DAC that just takes I2S; the 1.85C has an
// ES8311 codec (I2C 0x18) that wants MCLK and register setup, and an
// amplifier-enable pin. The firmware probes for the codec and does both.
#define PIN_I2S_BCLK  48
#define PIN_I2S_LRC   38
#define PIN_I2S_DOUT  47
#define PIN_I2S_MCLK  2
#define PIN_PA_EN     15
#define ES8311_ADDR   0x18

// The BOOT button doubles as play/pause. No encoder: the screen does volume.
#define PIN_BTN_PLAY  0
#define PIN_BTN_NEXT  -1
#define PIN_BTN_PREV  -1
#define PIN_ENC_A     -1
#define PIN_ENC_B     -1

// Held high, the 1.85 stays on when running from its battery (the 1.85C has
// a slide switch instead, and GPIO 7 is free on its header).
#define PIN_PWR_HOLD  7
// The battery through a 200K/100K divider. Charging and USB are not wired to
// any pin; the charge LED is the charger chip's own.
#define PIN_BAT_ADC   8

// System I2C: TCA9554 expander (0x20), IMU, RTC.
#define PIN_SYS_SDA   11
#define PIN_SYS_SCL   10
#define TCA9554_ADDR  0x20
#define EXIO_TP_RST   0   // expander bit: touch reset (EXIO1)
#define EXIO_LCD_RST  1   // expander bit: panel reset (EXIO2)

// ST77916 panel over QSPI.
#define PIN_LCD_CS    21
#define PIN_LCD_SCK   40
#define PIN_LCD_D0    46
#define PIN_LCD_D1    45
#define PIN_LCD_D2    42
#define PIN_LCD_D3    41
#define PIN_LCD_BL    5
#define LCD_SIZE      360

// CST816 touch on its own I2C bus.
#define PIN_TP_SDA    1
#define PIN_TP_SCL    3
#define PIN_TP_INT    4
#define CST816_ADDR   0x15

#else
// ======================================================================
// The custom build: an ESP32-S3 DevKitC on a breadboard.
// ======================================================================
#define BOARD_NAME   "ESP32-S3 DevKitC, custom build"
#define HAS_DISPLAY  0

// I2S DAC: PCM5102A (line out to an amplifier) or MAX98357A (speaker).
// Three wires. Any free GPIO works on the S3; these avoid the strapping pins.
#define PIN_I2S_BCLK  5   // BCK
#define PIN_I2S_LRC   6   // LCK / WS
#define PIN_I2S_DOUT  7   // DIN

// Buttons, wired from the pin to GND (internal pull-ups). -1 = not fitted.
// All on the same header side as the I2S pins on a DevKitC-1, and clear of
// GPIO 8-14, which the SPI display and its touch controller will use.
#define PIN_BTN_PLAY  17  // play / pause: the encoder's own push switch
#define PIN_BTN_NEXT  18
#define PIN_BTN_PREV  4

// Rotary encoder (EC11) for volume: A and B, common to GND. -1 = not fitted.
#define PIN_ENC_A     15
#define PIN_ENC_B     16

#define PIN_PWR_HOLD  -1
#define PIN_I2S_MCLK  -1
#define PIN_PA_EN     -1
#endif

// ---- Behaviour, both boards
#define DEFAULT_VOLUME 12         // 0..21
#define MAX_STATIONS   60         // stations kept in RAM from any one list
#define AUTO_RESUME    true       // play the last station again after a reboot
#define REPORT_PLAYS   true       // tell Radio Browser a station was played, as the site does
#define HOSTNAME       "airdrift" // reachable as http://airdrift.local
#define SETUP_AP_NAME  "Air Drift Setup"
