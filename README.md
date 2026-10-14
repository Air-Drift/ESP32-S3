# Air Drift for ESP32-S3

Internet radios built on the ESP32-S3 that play the stations from
[airdrift.stream](https://airdrift.stream). The firmware reads the same static
station lists the site serves, plays a station over I2S, shows the stream's
own "now playing", and is driven from a web page it serves itself, from a
serial console, and from whatever controls the board has.

One codebase, two boards:

| Build | Board | What it is |
| --- | --- | --- |
| `waveshare-185` | Waveshare ESP32-S3-Touch-LCD-1.85 / 1.85C | **The self-contained unit.** Round 360×360 touch screen, codec, amplifier and speaker on one board. Nothing to wire. |
| `devkit` | ESP32-S3-DevKitC-1 (N16R8) | **The custom build.** A dev board on a breadboard with a PCM5102A DAC, a TPA3116 amplifier, two speakers, a volume knob and buttons. Soldering involved. |

```bash
pio run -e waveshare-185 -t upload   # the Waveshare unit
pio run -e devkit -t upload          # the custom build
pio device monitor                   # serial console, 115200
```

## What it does

- **Lists from the site**, not from a third party: popular, trending, one
  country, one genre, each a single file from `airdrift.stream/data/v1/`.
  A genre or country fetch is the `.head.json` page, a few dozen KB.
- **Search** by name and **shared links**: paste any
  `https://airdrift.stream/listen/<id>` link and it plays, resolved through
  Radio Browser like the site does.
- **Now playing** from the stream's ICY title, the same source the site's
  panel reads.
- **Play reports** to Radio Browser on every play, as the site does, so the
  stations the radio plays count in the rankings. Off with `REPORT_PLAYS`.
- **Favourites**: a star on the page and on the screen saves the playing
  station to flash; a Favourites list on both plays them back.
- **Remembers** the last station and volume, and resumes after a reboot.
- **Reconnects** a dropped stream on its own.
- **A Device tab** on its web page: board and chip, memory with low-water
  marks, WiFi signal and addresses, CPU load per core, chip temperature,
  the audio pipeline and the task list, refreshed every few seconds.
- **WiFi setup** by captive portal: with no network stored it opens an access
  point called *Air Drift Setup*; join it and pick your network. Or type
  `wifi <ssid> <password>` on the serial console.

Codecs: MP3, AAC/AAC+, and HLS (m3u8), plus FLAC, Opus and Vorbis via the
decoder library. Streams play at their own sample rate.

## The Waveshare unit

Waveshare's [ESP32-S3-Touch-LCD-1.85](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.85)
is an ESP32-S3R8 (8 MB octal PSRAM) with 16 MB flash, a round 1.85" 360×360
ST77916 screen over QSPI, a CST816 touch controller, a TCA9554 I/O expander
that holds the screen's and touch's reset lines, a speaker amplifier, a
microphone, a TF slot, an IMU and an RTC. Plug a speaker into its header and
it is a radio.

There are two revisions, and the firmware tells them apart at boot by what
answers on the I2C bus:

| | DAC | Amplifier enable | Touch bus |
| --- | --- | --- | --- |
| 1.85 | PCM5101, plain I2S | always on | its own (GPIO 1/3) |
| 1.85C | ES8311 codec at 0x18, needs register setup and MCLK | GPIO 15 | the system bus (GPIO 10/11) |

The screen shows the station, the stream title, the codec and a volume ring
around the edge, in the site's own palette with its logo. Touch:

| Gesture | Does |
| --- | --- |
| Swipe left / right | next / previous station in the loaded list |
| Swipe up / down | volume |
| Tap the ◀◀ ▶ ▶▶ row at the bottom | previous, play/pause, next |
| Hold the logo | the connect screen: QR code and address of the web page, WiFi, factory reset |
| Tap the logo | back to the home screen |

The BOOT button is play/pause too. **Stations** opens the loaded list, or
the Popular list if none is loaded; **Save** keeps the playing station as a
favourite, and a star at the end of each list row does the same while
browsing. The list screen switches between Favourites and the loaded list.

How the screen is driven: ESP-IDF's `esp_lcd` SPI panel IO in quad mode,
the way Waveshare's own demo does it (the Arduino_GFX QSPI bus garbles this
panel). Drawing goes into an Arduino_GFX canvas in PSRAM for its fonts and
shapes, and the frame is pushed in row chunks through internal DMA buffers
whenever something changes. The panel init table is Waveshare's, in
`src/boards/waveshare185_init.h`.

## The custom build

An ESP32-S3 dev board, a PCM5102A DAC, a TPA3116D2 amplifier and two small
speakers, all fed from one USB-C PD charger.

| Part | Notes |
| --- | --- |
| ESP32-S3 dev board **with PSRAM** | The decoder refuses to start without PSRAM. N16R8 (16 MB flash, 8 MB octal PSRAM) is the common one; N8R2 also works with the `devkit-quad` build. |
| PCM5102A DAC (GY-PCM5102) | I2S in, stereo line out. |
| TPA3116D2 amplifier board | Takes the DAC's line out; 12–24 V supply; its own volume pots. |
| Two 4 Ω speakers | 78 mm, 15 W. |
| USB-C PD trigger board, set to 20 V | Pulls 20 V from a 30 W PD charger for the amplifier. |
| MP1584EN buck converter, trimmed to 5 V | Feeds the ESP32 from the same 20 V. |
| EC11 rotary encoder with push switch | volume, and play/pause on the push. |
| 2 tact switches | next and previous. |
| Later: 3.5" ILI9488 touch display | see Roadmap. |

A MAX98357A (DAC and 3 W mono amp in one, speaker wired straight to it) is
the simpler alternative: same three I2S wires, no amplifier board.

### Wiring

Defaults in `include/config.h`; any free GPIO works on the S3, but avoid the
strapping pins (0, 3, 45, 46) and the USB pins (19, 20).

| PCM5102A pin | ESP32-S3 pin | `config.h` |
| --- | --- | --- |
| BCK | GPIO 5 | `PIN_I2S_BCLK` |
| LCK | GPIO 6 | `PIN_I2S_LRC` |
| DIN | GPIO 7 | `PIN_I2S_DOUT` |
| SCK | GND | |
| VIN | 5V (3V3 also works) | |
| GND | GND | |

The GY-PCM5102 has four solder jumpers on the back and stays muted until
they are set: 1 = L, 2 = L, **3 = H**, 4 = L. Many ship unset.

DAC line out (L, GND, R) to the amplifier's input header, speakers on the
amplifier's L and R terminals, and the amplifier's power terminals on the
20 V from the PD trigger. The buck converter takes the same 20 V down to 5 V
for the ESP32's 5V pin; set it with a meter before connecting the board.
All grounds are common.

| Control | ESP32-S3 pin | `config.h` |
| --- | --- | --- |
| Encoder A | GPIO 15 | `PIN_ENC_A` |
| Encoder B | GPIO 16 | `PIN_ENC_B` |
| Encoder common (middle of the three) | GND | |
| Encoder push switch | GPIO 17 | `PIN_BTN_PLAY` (play / pause) |
| Encoder switch, other side | GND | |
| Next button | GPIO 18 to GND | `PIN_BTN_NEXT` |
| Previous button | GPIO 4 to GND | `PIN_BTN_PREV` |

Internal pull-ups, no resistors. GPIO 8–14 are kept free for the SPI
display and its I2C touch controller.

## Build and flash

[PlatformIO](https://platformio.org) does everything: `pip install platformio`.
The first build downloads the toolchain and Arduino core (a few hundred MB).
Built on the [pioarduino](https://github.com/pioarduino/platform-espressif32)
platform so the Arduino core is 3.x, which the audio library needs.

| Env | Board |
| --- | --- |
| `waveshare-185` | Waveshare ESP32-S3-Touch-LCD-1.85 / 1.85C |
| `devkit` (default) | DevKitC with octal PSRAM: N16R8, N8R8 |
| `devkit-quad` | DevKitC with quad PSRAM: N8R2, N4R2 |

To find out which PSRAM a DevKitC has:

```bash
pio pkg exec -- esptool.py --port /dev/ttyACM0 flash_id
```

and read the `Embedded PSRAM` line. With the wrong env the radio boots but
prints `no PSRAM`.

Linux: the serial port needs permissions once. Copy
`udev/99-airdrift-esp32.rules` to `/etc/udev/rules.d/` and run
`sudo udevadm control --reload-rules && sudo udevadm trigger`. Or add
yourself to the `dialout` group and log in again.

If `pio run` ever fails with a Python `AttributeError` or `TypeError` from
inside PlatformIO itself, run it again: PlatformIO 6.2 has an intermittent
fault in its library builder. The image it writes can also come out with a
bad checksum; `scripts/flash.sh` rebuilds the image from the ELF and
flashes it with esptool directly, which is what the Waveshare unit was
developed with.

## First boot

**The Waveshare unit** walks you through it on its screen:

1. With no WiFi stored it shows a QR code. Scan it with a phone to join the
   *Air Drift Setup* network (or join it from WiFi settings), and the setup
   page opens by itself, like hotel WiFi. Pick your network, enter the
   password, done.
2. Once online it offers two ways in: **Pick a station** opens the Popular
   list on the screen; **Use my phone** shows a QR code for the radio's own
   web page, with the address in words (`airdrift.local`).
3. Hold the logo at any time for the connect screen: the page's QR code, the
   IP and hostname, the WiFi network and signal, the firmware version, and
   **Factory reset**. Tap it twice and the radio forgets its WiFi, favourites
   and last station and starts over at step 1, for moving house or handing
   it on.

**The custom build** has no screen yet, so:

1. Open the serial monitor. The radio prints its state and `type help`.
2. Give it WiFi: join the *Air Drift Setup* network from a phone and pick your
   network, or type `wifi <ssid> <password>` (quote a name with spaces).
3. It prints its address. Open **http://airdrift.local** (or the IP) on a
   phone or laptop on the same network and pick a station.

With a bare dev board and no DAC wired, everything except the sound works: the
lists load, the stream opens, the titles print.

## Updating the firmware

Open the radio's web page, **Device** tab, **Firmware**. Choose a build
(`firmware.bin`, which PlatformIO leaves in `.pio/build/<env>/`) and press
**Update**: the radio writes it to its spare app partition, checks it, and
restarts on it; the page reconnects when it is back. From a shell,
`scripts/ota.sh <env> [host]` builds and sends one in a single step. The
screen says "Updating firmware" while it is written; keep the power on.
Over USB, `scripts/flash.sh` still works and also resets the boot selector
back to the first partition.

## Serial console

```
wifi <ssid> <password>   join a network
forget                   erase WiFi and restart into the setup portal
factory                  erase WiFi, favourites and the last station, then restart
popular | trending       load that list from airdrift.stream
country <CC>             load a country, e.g. country GB
genre <name>             load a genre, e.g. genre jazz
search <words>           search stations by name
list                     show the loaded list
play <n>                 play station n from the list
play <air drift link>    play a shared https://airdrift.stream/listen/... link
play <stream url>        play any stream
next | prev | pause | stop
vol <0-21>
resample on|off          48 kHz output through the library's resampler (off: it falls behind)
core 0|1                 which CPU core decodes
debug on|off             show the decoder's own messages
status | help | reboot
```

## Web page and API

The page at `http://airdrift.local` is plain HTML served from flash, with
station logos loaded by the browser from their own hosts, never by the radio.

| Call | Does |
| --- | --- |
| `GET /api/state` | playing, station, title, codec, volume, list source |
| `GET /api/device` | board, chip, memory, WiFi, CPU load per core, audio pipeline, task list (the page's Device tab) |
| `POST /api/update` | the firmware image as a multipart file field (`curl -F firmware=@firmware.bin`); the radio writes, checks and restarts on it |
| `GET /api/list` | the loaded list |
| `GET /api/load?source=favourites\|popular\|trending\|country:US\|tag:jazz` | load a list (favourites are local) |
| `GET /api/fav` | save or unsave the playing station |
| `GET /api/search?q=words` | search by name |
| `GET /api/play?i=3` | play the n-th station of the list |
| `GET /api/play?link=https://airdrift.stream/listen/<id>` | play a shared link, or any stream URL |
| `GET /api/stop`, `/api/pause`, `/api/next`, `/api/prev` | transport |
| `GET /api/volume?v=0..21` | volume |

Anything on the LAN can drive it, so a Home Assistant `rest_command` or a
shortcut on a phone is a one-liner.

## How it fits Air Drift

| Site | Radio |
| --- | --- |
| `data/v1/popular.json`, `trending.json` | the Popular and Trending lists |
| `data/v1/country/XX.head.json` | one country, first page of HTTPS stations |
| `data/v1/index.json` + `data/v1/tag/<file>.head.json` | one genre |
| live search on Radio Browser | search |
| `/listen/<id>` links | paste to play |
| play report (`/json/url/<id>`) | sent on every play |
| ICY stream title | "now playing" |
| `css/app.css` palette, `icons/icon.svg` | the screen's colours and logo |

The radio keeps at most `MAX_STATIONS` (60) of any list in RAM, and fetches
only when asked, so it is one request per list and none while playing.

## Layout

```
platformio.ini              boards, envs, libraries
include/config.h            pins and defaults, per board
src/main.cpp                boot and the main loop
src/net.*                   WiFi, setup portal, mDNS
src/directory.*             the station lists, search, links, play reports
src/player.*                the decoder and I2S output, on its own task
src/display.*               the Waveshare screen and touch (empty on the devkit)
src/boards/es8311.h         the 1.85C's codec bring-up
src/boards/waveshare185_init.h  the panel's init table
src/webui.*                 the web page and JSON API
src/console.*               serial commands
src/controls.*              buttons and encoder
udev/                       serial port permissions for Linux
scripts/flash.sh            build, re-image and flash with esptool
```

Libraries: [ESP32-audioI2S](https://github.com/schreibfaul1/ESP32-audioI2S)
(decoding and I2S), [ArduinoJson](https://arduinojson.org),
[WiFiManager](https://github.com/tzapu/WiFiManager), and for the screen
[Arduino_GFX](https://github.com/moononournation/Arduino_GFX) (canvas and
fonts) with [U8g2](https://github.com/olikraus/u8g2)'s fonts.

## Roadmap

- A station list on the Waveshare screen, not only next/previous.
- The custom build's 3.5" ILI9488 touch display (SPI, FT6236 touch).
- A case for the custom build, and a board layout once the parts are settled.
- A "play on my radio" hand-off from the site, if the site ever gains a way
  to reach a device on the LAN.

## Licence

MIT, like Air Drift.
