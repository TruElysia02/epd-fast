#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "planner.h"

// Independent byte-RLE decoder. Keyframes are absolute; other frames XOR the
// prior bitmap. Seeking decodes at most one second, never the entire video.
class AppleVideo {
 public:
  bool begin(const uint8_t* data, size_t size) {
    data_ = data; size_ = size;
    if (size < 20 || memcmp(data, "BA29", 4) || u16(4) != kWidth || u16(6) != kHeight) return false;
    fps_ = u16(8); keyInterval_ = u16(10); count_ = u32(12);
    if (!fps_ || fps_ > 30 || !keyInterval_ || keyInterval_ > 30 || !count_ || count_ > 100000) return false;
    payload_ = 16 + size_t(count_ + 1) * 4;
    if (payload_ > size || u32(16) != 0 || u32(16 + size_t(count_) * 4) != size - payload_) return false;
    decoded_ = -1;
    return true;
  }

  bool frame(uint32_t index, uint8_t* output) {
    if (index >= count_) return false;
    const uint32_t key = index / keyInterval_ * keyInterval_;
    uint32_t start = key;
    if (decoded_ >= int32_t(key) && decoded_ <= int32_t(index)) start = decoded_ + 1;
    for (uint32_t f = start; f <= index; ++f) {
      if (f % keyInterval_ == 0) memset(bitmap_, 0, sizeof(bitmap_));
      const uint32_t a = u32(16 + size_t(f) * 4), b = u32(16 + size_t(f + 1) * 4);
      if (a > b || b > size_ - payload_) return false;
      const uint8_t* src = data_ + payload_ + a;
      const uint8_t* end = data_ + payload_ + b;
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
  int32_t decoded_ = -1;
  uint8_t bitmap_[kFrameBytes]{};
};
