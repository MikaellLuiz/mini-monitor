#pragma once
// PNG minimo (RGB 8 bits, deflate sem compressao): so para os screenshots do simulador.

#include <stdint.h>
#include <stdio.h>

#include <vector>

namespace png_detail {

inline uint32_t crc32(const uint8_t *p, size_t n, uint32_t c = 0xFFFFFFFFu) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t k = i;
      for (int j = 0; j < 8; j++) k = (k & 1) ? 0xEDB88320u ^ (k >> 1) : k >> 1;
      table[i] = k;
    }
    ready = true;
  }
  for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c;
}

inline void put32(std::vector<uint8_t> &v, uint32_t x) {
  v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}

inline void chunk(FILE *f, const char *type, const std::vector<uint8_t> &data) {
  std::vector<uint8_t> buf;
  put32(buf, (uint32_t)data.size());
  buf.insert(buf.end(), type, type + 4);
  buf.insert(buf.end(), data.begin(), data.end());
  uint32_t c = crc32(buf.data() + 4, buf.size() - 4) ^ 0xFFFFFFFFu;
  put32(buf, c);
  fwrite(buf.data(), 1, buf.size(), f);
}

}  // namespace png_detail

// rgb: w*h*3 bytes, linha a linha de cima para baixo.
inline bool png_write(const char *path, int w, int h, const uint8_t *rgb) {
  using namespace png_detail;
  std::vector<uint8_t> raw;
  raw.reserve((size_t)(w * 3 + 1) * h);
  for (int y = 0; y < h; y++) {
    raw.push_back(0);  // filtro "nenhum"
    raw.insert(raw.end(), rgb + (size_t)y * w * 3, rgb + (size_t)(y + 1) * w * 3);
  }

  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t byte : raw) { a = (a + byte) % 65521; b = (b + a) % 65521; }
  for (size_t off = 0; off < raw.size(); off += 65535) {
    size_t len = raw.size() - off < 65535 ? raw.size() - off : 65535;
    z.push_back(off + len >= raw.size() ? 1 : 0);
    z.push_back(len & 0xFF); z.push_back(len >> 8);
    z.push_back(~len & 0xFF); z.push_back((~len >> 8) & 0xFF);
    z.insert(z.end(), raw.begin() + off, raw.begin() + off + len);
  }
  put32(z, (b << 16) | a);

  FILE *f = fopen(path, "wb");
  if (!f) return false;
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  fwrite(sig, 1, 8, f);
  std::vector<uint8_t> ihdr;
  put32(ihdr, w); put32(ihdr, h);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
  chunk(f, "IHDR", ihdr);
  chunk(f, "IDAT", z);
  chunk(f, "IEND", {});
  fclose(f);
  return true;
}
