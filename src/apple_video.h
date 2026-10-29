#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "planner.h"
#if defined(ESP_PLATFORM)
#include <miniz.h>  // ESP32-C6 ROM inflater; keep its working state off the task stack.
#else
#include "../lib/miniz/miniz_tinfl.h"
#endif

// BA29 uses byte RLE; BAZ9 uses one checked zlib stream per native XOR bitmap.
// Keyframes are absolute. Seeking decodes at most 60 frames, independent of the content rate.
class AppleVideo {
 public:
  bool begin(const uint8_t* data, size_t size) {
    ready_ = false;
    decoded_ = -1;
    if (!data || size < 20) return false;
    data_ = data; size_ = size;
    deflate_ = !memcmp(data, "BAZ9", 4);
    if ((!deflate_ && memcmp(data, "BA29", 4)) || u16(4) != kWidth || u16(6) != kHeight) return false;
    fps_ = u16(8); keyInterval_ = u16(10); count_ = u32(12);
    if (!fps_ || fps_ > 120 || !keyInterval_ || keyInterval_ > 60 || !count_ || count_ > 100000) return false;
    payload_ = 16 + size_t(count_ + 1) * 4;
    if (payload_ > size || u32(16) != 0 || u32(16 + size_t(count_) * 4) != size - payload_) return false;
    ready_ = true;
    return true;
  }

  bool frame(uint32_t index, uint8_t* output) {
    if (!ready_ || !output || index >= count_) return false;
    const uint32_t key = index / keyInterval_ * keyInterval_;
    uint32_t start = key;
    if (decoded_ >= int32_t(key) && decoded_ <= int32_t(index)) start = decoded_ + 1;
    for (uint32_t f = start; f <= index; ++f) {
      if (f % keyInterval_ == 0) memset(bitmap_, 0, sizeof(bitmap_));
      const uint32_t a = u32(16 + size_t(f) * 4), b = u32(16 + size_t(f + 1) * 4);
      if (a > b || b > size_ - payload_) return false;
      const uint8_t* src = data_ + payload_ + a;
      const uint8_t* end = data_ + payload_ + b;
      if (deflate_) {
        size_t inputSize = b - a, outputSize = sizeof(delta_);
        tinfl_init(&inflate_);
        const auto result = tinfl_decompress(&inflate_, src, &inputSize, delta_, delta_, &outputSize,
            TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        if (result != TINFL_STATUS_DONE || inputSize != b - a || outputSize != sizeof(delta_)) {
          decoded_ = -1;
          return false;
        }
        for (size_t j = 0; j < kFrameBytes; ++j) bitmap_[j] ^= delta_[j];
        decoded_ = f;
        continue;
      }
      size_t out = 0;
      while (src < end) {
        const uint8_t control = *src++;
        const size_t length = (control & 127) + 1;
        if (out + length > kFrameBytes) return false;
        if (control & 128) {
          if (src == end) return false;
          const uint8_t value = *src++;
          for (size_t j = 0; j < length; ++j) bitmap_[out++] ^= value;
        } else {
          if (size_t(end - src) < length) return false;
          for (size_t j = 0; j < length; ++j) bitmap_[out++] ^= *src++;
        }
      }
      if (out != kFrameBytes) return false;
      decoded_ = f;
    }
    memcpy(output, bitmap_, sizeof(bitmap_));
    return true;
  }
  uint32_t count() const { return count_; }
  uint16_t fps() const { return fps_; }
  uint32_t durationMs() const { return count_ * 1000 / fps_; }
 private:
  uint16_t u16(size_t offset) const { return uint16_t(data_[offset]) | uint16_t(data_[offset + 1]) << 8; }
  uint32_t u32(size_t offset) const {
    return uint32_t(data_[offset]) | uint32_t(data_[offset + 1]) << 8 |
           uint32_t(data_[offset + 2]) << 16 | uint32_t(data_[offset + 3]) << 24;
  }
  const uint8_t* data_ = nullptr;
  size_t size_ = 0, payload_ = 0;
  uint32_t count_ = 0;
  uint16_t fps_ = 0, keyInterval_ = 0;
  bool ready_ = false, deflate_ = false;
  int32_t decoded_ = -1;
  uint8_t bitmap_[kFrameBytes]{};
  uint8_t delta_[kFrameBytes]{};
  tinfl_decompressor inflate_{};
};
