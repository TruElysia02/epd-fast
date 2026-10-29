#include "apple_video.h"
#include <cassert>
#include <fstream>
#include <iterator>
#include <random>
#include <vector>
#include <cstdio>
#include <chrono>

static AppleVideo video;
static uint8_t output[kFrameBytes];
static std::vector<uint8_t> read(const char* path) {
  std::ifstream f(path, std::ios::binary);
  assert(f.good());
  return {std::istreambuf_iterator<char>(f), {}};
}
int main(int argc, char** argv) {
  assert(argc == 1 || argc == 3);
  const auto data = read(argc == 3 ? argv[1] : "assets/badapple.bin");
  const auto hashes = read(argc == 3 ? argv[2] : "assets/badapple.hashes");
  assert(!video.frame(0, output));
  assert(video.begin(data.data(), data.size()));
  assert(hashes.size() == video.count() * 4);
  uint64_t decodeNs = 0;
  auto check = [&](uint32_t index) {
    const auto start = std::chrono::steady_clock::now();
    assert(video.frame(index, output));
    decodeNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    uint32_t checksum = 2166136261u;
    for (uint8_t value : output) checksum = (checksum ^ value) * 16777619u;
    uint32_t expected = 0;
    for (unsigned b = 0; b < 4; ++b) expected |= uint32_t(hashes[index * 4 + b]) << (b * 8);
    assert(checksum == expected);
  };
  for (uint32_t i = 0; i < video.count(); ++i) check(i);
  printf("Sequential decode: %.2f us/frame on this desktop, frames=%lu fps=%u\n",
         double(decodeNs) / video.count() / 1000, (unsigned long)video.count(), video.fps());
  std::mt19937 random(29);
  for (int i = 0; i < 200; ++i) check(random() % video.count());
  assert(!video.frame(video.count(), output));
  assert(!video.begin(data.data(), 16));
  assert(!video.frame(0, output));
  assert(!video.begin(nullptr, data.size()));
  // Frame-rate metadata is independent of the existing byte-RLE/zlib format.
  // The same keyframe payload must remain readable at both old and new rates.
  for (unsigned fps : {20u, 60u, 120u}) {
    auto rate = data; rate[8] = fps; rate[9] = 0;
    assert(video.begin(rate.data(), rate.size()));
    assert(video.fps() == fps);
    assert(video.durationMs() == video.count() * 1000 / fps);
    for (uint32_t index : {0u, 59u, 60u, 119u})
      if (index < video.count()) check(index);
  }
  auto invalid = data; invalid[8] = 0; invalid[9] = 0;
  assert(!video.begin(invalid.data(), invalid.size()));
  invalid = data; invalid[8] = 121; invalid[9] = 0;
  assert(!video.begin(invalid.data(), invalid.size()));
  invalid = data; invalid[10] = 61;
  assert(!video.begin(invalid.data(), invalid.size()));
  auto bad = data; bad[16 + 4] = 0xff; bad[16 + 5] = 0xff;
  assert(video.begin(bad.data(), bad.size()));
  assert(!video.frame(0, output));
  if (!memcmp(data.data(), "BAZ9", 4)) {
    // A changed zlib checksum must fail, even if the deflate payload is valid.
    bad = data;
    const size_t count = uint32_t(data[12]) | uint32_t(data[13]) << 8 |
        uint32_t(data[14]) << 16 | uint32_t(data[15]) << 24;
    const size_t firstEnd = uint32_t(data[20]) | uint32_t(data[21]) << 8 |
        uint32_t(data[22]) << 16 | uint32_t(data[23]) << 24;
    bad[16 + (count + 1) * 4 + firstEnd - 1] ^= 1;
    assert(video.begin(bad.data(), bad.size()));
    assert(!video.frame(0, output));
  }
  puts("PASS: all frames match source-derived hashes; 200 seeks; bounds and corruption checks");
}
