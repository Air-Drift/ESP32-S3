# Air Drift name and logo

The code in this repository is released under the [MIT licence](LICENSE). That
licence covers the code. It does not cover the Air Drift brand, which is set
out here.

## What the brand is

- the name **Air Drift**, and "Air Drift for ESP32-S3"
- the Air Drift logo: five vertical bars on a rounded green tile
- the "airdrift" wordmark, with "drift" in green
- the files that carry them: `src/icons.h`, the logo drawn by `src/display.cpp`
  and `src/webui.cpp`, `docs/icon-192.png`, `docs/banner.png` and
  `docs/banner.html`

These are trademarks of the Air Drift project
([airdrift.stream](https://airdrift.stream)). All rights in them are reserved.

## What you may do

- Build this firmware from this repository, unmodified, and run it on your own
  radios with its branding as it comes.
- Say that something works with, or is based on, Air Drift for ESP32-S3, and
  link to this repository or to airdrift.stream.
- Show the logo, unaltered, when writing about or reviewing the project.

## What you may not do

- Distribute a modified version under the Air Drift name or with the Air Drift
  logo, whether as source, as a firmware image, or as a finished radio for
  sale. A fork that is published needs its own name and its own mark.
- Use the name or logo for another product, service, app or website.
- Use them in a way that suggests the Air Drift project made, endorses or
  supports something it did not.
- Alter the logo, or combine it with another mark.

## Publishing a fork

Replace the icons in `src/icons.h`, the logo and wordmark in `src/display.cpp`
(`logoTile` and `brand`) and in the page and setup portal in `src/webui.cpp`
and `src/net.cpp`, the name in `include/config.h` (`SETUP_AP_NAME`,
`HOSTNAME`), and `docs/banner.png`. The colours, layout and fonts are not part
of the brand and may stay.

## Other people's marks

Waveshare and ESP32 are trademarks of their owners, used here only to say what
this firmware runs on. This project is not affiliated with or endorsed by
Waveshare or Espressif. Nothing here grants any right in their marks.

## Questions

For anything not covered here, ask first: hello@airdrift.stream.
