#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Controller coordinates, before the canvas rotation to landscape.
constexpr size_t kWidth = 128;
constexpr size_t kHeight = 296;
constexpr size_t kPixels = kWidth * kHeight;
constexpr size_t kFrameBytes = kPixels / 8;
constexpr uint32_t kCleanScans = 2000;
constexpr int kPulseLimit = 7;

enum class DriveMode { Budget, Net };

// A pulse-count model, not a measurement of panel charge or reflectance.
// Planning is side-effect free for charge; commit ONLY after a successful scan.
class Planner {
 public:
  void reset(DriveMode mode, const uint8_t* shown) {
    mode_ = mode;
    if (!blackPulses_) blackPulses_ = kPulseLimit;
    if (!whitePulses_) whitePulses_ = kPulseLimit;
    memcpy(target_, shown, kFrameBytes);
    memset(remaining_, 0, sizeof(remaining_));
    memset(net_, 0, sizeof(net_));
    memset(active_, mode == DriveMode::Net ? 0xff : 0, sizeof(active_));
  }

  void setPulses(uint8_t black, uint8_t white) { blackPulses_ = black; whitePulses_ = white; }

  bool plan(const uint8_t* next, bool settle = false) {
    memset(black, 0, sizeof(black));
    memset(white, 0, sizeof(white));
    bool any = false;
    for (size_t i = 0; i < kFrameBytes; ++i) {
      const uint8_t changed = next[i] ^ target_[i];
      const uint8_t todo = settle ? 0xff : (active_[i] | changed);
      if (!todo) continue; // Most bytes of a moving UI are stationary.
      for (unsigned b = 0; b < 8; ++b) {
        const uint8_t mask = 0x80u >> b;
        if (!(todo & mask)) continue;
        const size_t p = i * 8 + b;
        const bool wantWhite = next[i] & mask;
        bool drive = false;
        if (settle) {
          // Repay only in the direction of the displayed target.
          drive = wantWhite ? net_[p] > 0 : net_[p] < 0;
        } else if (mode_ == DriveMode::Net) {
          const int value = net_[p] + (wantWhite ? -1 : 1);
          drive = value >= -whitePulses_ && value <= blackPulses_;
        } else {
          if (changed & mask) remaining_[p] = wantWhite ? whitePulses_ : blackPulses_;
          drive = remaining_[p] != 0;
        }
        if (drive) {
          (wantWhite ? white[i] : black[i]) |= mask;
          any = true;
        }
      }
    }
    return any;
  }

  void commit(const uint8_t* next, bool settle = false) {
    for (size_t i = 0; i < kFrameBytes; ++i) {
      const uint8_t driven = black[i] | white[i];
      if (!driven) continue;
      for (unsigned b = 0; b < 8; ++b) {
        const uint8_t mask = 0x80u >> b;
        if (!(driven & mask)) continue;
        const size_t p = i * 8 + b;
        if ((black[i] | white[i]) & mask) {
          net_[p] += (black[i] & mask) ? 1 : -1;
          if (!settle && mode_ == DriveMode::Budget && remaining_[p]) --remaining_[p];
          const bool needsMore = mode_ == DriveMode::Budget ? remaining_[p] != 0 :
              ((next[i] & mask) ? net_[p] > -whitePulses_ : net_[p] < blackPulses_);
          if (needsMore) active_[i] |= mask;
          else active_[i] &= uint8_t(~mask);
        }
      }
    }
    if (!settle) memcpy(target_, next, kFrameBytes);
  }

  // Even when no scan was needed, accept the latest desired bitmap.
  void accept(const uint8_t* next) { memcpy(target_, next, kFrameBytes); }
  int netAt(size_t pixel) const { return net_[pixel]; }
  uint8_t black[kFrameBytes]{};
  uint8_t white[kFrameBytes]{};

 private:
  DriveMode mode_ = DriveMode::Budget;
  uint8_t blackPulses_ = 0, whitePulses_ = 0;
  uint8_t active_[kFrameBytes]{};
  uint8_t target_[kFrameBytes]{};
  uint8_t remaining_[kPixels]{};
  // At most 2000 scans between full cleans: int16_t avoids the upstream
  // int8_t history wrapping on asymmetric high-frequency test patterns.
  int16_t net_[kPixels]{};
};

struct MaskRows {
  uint16_t first = kHeight, last = 0;
};

inline MaskRows maskRows(const uint8_t* black, const uint8_t* white) {
  MaskRows rows;
  for (size_t i = 0; i < kFrameBytes; ++i) {
    if (!(black[i] | white[i])) continue;
    const uint16_t row = i / (kWidth / 8);
    if (rows.first == kHeight) rows.first = row;
    rows.last = row;
  }
  return rows;
}

inline MaskRows uploadRows(MaskRows current, MaskRows previous) {
  if (previous.first < kHeight) {
    if (previous.first < current.first) current.first = previous.first;
    if (previous.last > current.last) current.last = previous.last;
  }
  if (current.first == kHeight) current.first = current.last = 0;
  return current;
}
