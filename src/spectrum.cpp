#include "spectrum.h"
#include <esp_dsp.h>
#include <esp_heap_caps.h>

static const int N = 2048;          // FFT size: 21.5 Hz a bin at 44.1 kHz
static const int RING = 16384;      // mono samples kept
static const int LAG = 4096;        // samples still in the I2S DMA buffers, not yet heard
static int16_t* ring = nullptr;
static float* fft = nullptr;        // N complex values
static float* win = nullptr;        // Hann window
static volatile uint32_t head = 0;  // samples written, ever
static volatile bool on = false;

// The audio library hands over every block it decodes, interleaved L/R as
// 32-bit left-justified samples, before volume. Mix to mono, 16 bits.
static volatile unsigned long heardAt = 0;
void audio_process_raw_samples(int32_t* buff, int16_t words) {
  heardAt = millis();
  if (!on || !ring) return;
  uint32_t h = head;
  for (int i = 0; i + 1 < words; i += 2) ring[h++ & (RING - 1)] = (int16_t)((buff[i] >> 17) + (buff[i + 1] >> 17));
  head = h;
}

unsigned long spectrum::heard() { return heardAt; }

void spectrum::enable(bool want) {
  if (want && !ring) {
    ring = (int16_t*)heap_caps_calloc(RING, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    fft = (float*)heap_caps_malloc(N * 2 * sizeof(float), MALLOC_CAP_SPIRAM);
    win = (float*)heap_caps_malloc(N * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!ring || !fft || !win || dsps_fft2r_init_fc32(nullptr, N) != ESP_OK) { Serial.println("[spectrum] could not start"); return; }
    for (int i = 0; i < N; i++) win[i] = 0.5f * (1 - cosf(2 * PI * i / (N - 1)));
  }
  on = want && ring;
}

bool spectrum::read(float* bands, int n, uint32_t rate) {
  if (!on || head < (uint32_t)(N + LAG)) return false;
  if (millis() - heardAt > 400) return false;   // nothing coming in: silence, not the last sound over and over
  uint32_t end = head - LAG;
  for (int i = 0; i < N; i++) {
    fft[2 * i] = ring[(end - N + i) & (RING - 1)] * win[i];
    fft[2 * i + 1] = 0;
  }
  dsps_fft2r_fc32(fft, N);
  dsps_bit_rev_fc32(fft, N);
  // As the site does it: each band takes its loudest bin, in dB, onto 0..1,
  // with a slight tilt upward because programme audio rolls off. Broadcast
  // music measures about 95..130 dB on this scale (a full-scale sine, 144).
  float nyq = (rate ? rate : 44100) / 2.0f, lo = 40, hi = min(16000.0f, nyq);
  for (int b = 0; b < n; b++) {
    float f0 = lo * powf(hi / lo, (float)b / n), f1 = lo * powf(hi / lo, (float)(b + 1) / n);
    int b0 = max(1, (int)(f0 / nyq * (N / 2))), b1 = min(N / 2 - 1, max(b0 + 1, (int)ceilf(f1 / nyq * (N / 2))));
    float peak = 0;
    for (int k = b0; k < b1; k++) { float p = fft[2 * k] * fft[2 * k] + fft[2 * k + 1] * fft[2 * k + 1]; if (p > peak) peak = p; }
    float db = 10 * log10f(peak + 1);
    bands[b] = constrain((db - 90) / 42 * (1 + 0.35f * b / n), 0.0f, 1.0f);
  }
  return true;
}
