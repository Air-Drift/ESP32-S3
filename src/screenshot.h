#pragma once
#include <Arduino.h>
// The screen's framebuffer as a BMP: 16-bit RGB565 with bit masks, rows
// bottom-up, so the pixels go out as they are. 66 bytes of header.
static const size_t BMP_HEADER = 66;
inline void bmpHeader(uint8_t* h, uint32_t w, uint32_t hgt) {
  uint32_t img = w * hgt * 2, total = BMP_HEADER + img;
  auto u16 = [&](int at, uint16_t v) { h[at] = v; h[at + 1] = v >> 8; };
  auto u32 = [&](int at, uint32_t v) { h[at] = v; h[at + 1] = v >> 8; h[at + 2] = v >> 16; h[at + 3] = v >> 24; };
  memset(h, 0, BMP_HEADER);
  h[0] = 'B'; h[1] = 'M'; u32(2, total); u32(10, BMP_HEADER);
  u32(14, 40); u32(18, w); u32(22, hgt); u16(26, 1); u16(28, 16); u32(30, 3); u32(34, img);
  u32(38, 2835); u32(42, 2835);
  u32(54, 0xF800); u32(58, 0x07E0); u32(62, 0x001F);
}
