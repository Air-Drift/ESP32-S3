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
#define BAT_SCALE     (3.0f / 0.9945f)

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

#elif defined(BOARD_GUITION_KNOB)
// ======================================================================
// Guition JC3636K518C: a round touch screen set in a knob. Waveshare sells
// the same board as the ESP32-S3-Knob-Touch-LCD-1.8. Pins from its
// schematic, and from a port that runs on this board.
// ======================================================================
#define BOARD_NAME   "Guition JC3636K518C"
#define HAS_DISPLAY  1

// A PCM5100A to the 3.5 mm jack: plain I2S, no codec setup, no amplifier.
// Both chips are wired to it through a CH445P switch, and GPIO 0 picks which
// one it hears (see player.cpp). That makes GPIO 0 an output here, not a
// button.
#define PIN_I2S_BCLK  39
#define PIN_I2S_LRC   40
#define PIN_I2S_DOUT  41
#define PIN_I2S_MCLK  -1
#define PIN_PA_EN     -1
#define PIN_DAC_SEL   0
#define DAC_SEL_S3    HIGH   // checked on the board: high gives the S3 the jack

// No buttons. The screen opens the menu with a long press instead.
#define PIN_BTN_PLAY  -1
#define PIN_BTN_NEXT  -1
#define PIN_BTN_PREV  -1
#define LONG_PRESS_MENU 1

// The knob is two switches rather than a quadrature encoder: one direction
// pulses A, the other pulses B (see controls.cpp). 10K pull-ups on the board.
#define PIN_ENC_A     8
#define PIN_ENC_B     7
#define KNOB_TWO_SWITCH 1

// An on/off switch, so nothing to hold.
#define PIN_PWR_HOLD  -1
// The battery through a 10K/10K divider. As on the 1.85, charging is not
// wired to any pin.
#define PIN_BAT_ADC   1
#define BAT_SCALE     2.0f

// The one I2C bus: the touch, and a DRV2605 vibration driver (0x5A) that
// Air Drift does not use.
#define PIN_SYS_SDA   11
#define PIN_SYS_SCL   12

// ST77916 panel over QSPI, the same panel and init as the 1.85's, with its
// reset on a pin of its own rather than an expander.
#define PIN_LCD_CS    14
#define PIN_LCD_SCK   13
#define PIN_LCD_D0    15
#define PIN_LCD_D1    16
#define PIN_LCD_D2    17
#define PIN_LCD_D3    18
#define PIN_LCD_BL    47
#define PIN_LCD_RST   21
#define LCD_SIZE      360

// CST816 touch, on the bus above, with its own reset.
#define PIN_TP_SDA    11
#define PIN_TP_SCL    12
#define PIN_TP_INT    9
#define PIN_TP_RST    10
#define CST816_ADDR   0x15

#elif defined(BOARD_GUITION_W518)
// ======================================================================
// Guition JC3636W518C: a round touch screen with a 3.5 mm jack, no knob and
// no battery. Pins from clowrey's ESPHome config for the board.
// ======================================================================
#define BOARD_NAME   "Guition JC3636W518C"
#define HAS_DISPLAY  1
#define PANEL_INIT   guition_w518_st77916_init   // its own glass: see boards/guition_w518_init.h

// An I2S DAC that needs no setup, to the jack. GPIO 48 is the amplifier's
// mute, held high for sound as the ESPHome config does.
#define PIN_I2S_BCLK  18
#define PIN_I2S_LRC   16
#define PIN_I2S_DOUT  17
#define PIN_I2S_MCLK  21
#define PIN_PA_EN     48

// No buttons (BOOT is in a pinhole) and no knob: the screen does it all, and
// a long press opens the menu.
#define PIN_BTN_PLAY  -1
#define PIN_BTN_NEXT  -1
#define PIN_BTN_PREV  -1
#define PIN_ENC_A     -1
#define PIN_ENC_B     -1
#define LONG_PRESS_MENU 1

// USB power only: nothing to hold, no battery to read.
#define PIN_PWR_HOLD  -1

// The one I2C bus is the touch's.
#define PIN_SYS_SDA   7
#define PIN_SYS_SCL   8

// ST77916 panel over QSPI, resets on plain pins.
#define PIN_LCD_CS    10
#define PIN_LCD_SCK   9
#define PIN_LCD_D0    11
#define PIN_LCD_D1    12
#define PIN_LCD_D2    13
#define PIN_LCD_D3    14
#define PIN_LCD_BL    15
#define PIN_LCD_RST   47
#define LCD_SIZE      360

// CST816 touch, with its own reset.
#define PIN_TP_SDA    7
#define PIN_TP_SCL    8
#define PIN_TP_INT    41
#define PIN_TP_RST    40
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
