#include "apple_video.h"
#include <cassert>
#include <fstream>
#include <iterator>
#include <random>
#include <vector>
#include <cstdio>

static AppleVideo video;
static uint8_t output[kFrameBytes];
static std::vector<uint8_t> read(const char* path) {
  std::ifstream f(path, std::ios::binary);
  assert(f.good());
  return {std::istreambuf_iterator<char>(f), {}};
}
int main() {
  const auto data = read("assets/badapple.bin");
  const auto hashes = read("assets/badapple.hashes");
  assert(video.begin(data.data(), data.size()));
  assert(hashes.size() == video.count() * 4);
  auto check = [&](uint32_t index) {
    assert(video.frame(index, output));
    uint32_t checksum = 2166136261u;
    for (uint8_t value : output) checksum = (checksum ^ value) * 16777619u;
    uint32_t expected = 0;
    for (unsigned b = 0; b < 4; ++b) expected |= uint32_t(hashes[index * 4 + b]) << (b * 8);
    assert(checksum == expected);
  };
  for (uint32_t i = 0; i < video.count(); ++i) check(i);
  std::mt19937 random(29);
  for (int i = 0; i < 200; ++i) check(random() % video.count());
  assert(!video.frame(video.count(), output));
  assert(!video.begin(data.data(), 16));
  auto bad = data; bad[16 + 4] = 0xff; bad[16 + 5] = 0xff;
  assert(video.begin(bad.data(), bad.size()));
  assert(!video.frame(0, output));
  puts("PASS: all frames match source-derived hashes; 200 seeks; bounds and corruption checks");
}
