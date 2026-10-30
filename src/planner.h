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
static_assert(kCleanScans <= 2047, "Stable-tail history reserves 12 signed bits");

enum class DriveMode { Budget, Net };

// A pulse-count model, not a measurement of panel charge or reflectance.
// Planning is side-effect free for charge; commit ONLY after a successful scan.
class Planner {
 public:
  void reset(DriveMode mode, const uint8_t* shown) {
    mode_ = mode;
    if (!blackPulses_) blackPulses_ = kPulseLimit;
    if (!whitePulses_) whitePulses_ = kPulseLimit;
    updateHistoryScale();
    memcpy(target_, shown, kFrameBytes);
    if (reversalPercent_ && mode_ == DriveMode::Budget) {
      for (size_t p = 0; p < kPixels; ++p)
        remaining_[p] = (shown[p / 8] & (0x80u >> (p % 8))) ? 0 : historyMaximum_;
    } else memset(remaining_, 0, sizeof(remaining_));
    memset(net_, 0, sizeof(net_));
    memset(active_, mode == DriveMode::Net ? 0xff : 0, sizeof(active_));
    stableClockValid_ = false;
    stableTailPulsesDelivered_ = 0;
  }

  bool setPulses(uint8_t black, uint8_t white) {
    if (!black || black > 32 || !white || white > 32 ||
        (reversalPercent_ && !supportsHistory(black, white))) return false;
    blackPulses_ = black; whitePulses_ = white;
    updateHistoryScale();
    return true;
  }

  static bool supportsHistory(uint8_t black, uint8_t white) {
    return black && black <= 32 && white && white <= 32 &&
           unsigned(black / gcd(black, white)) * white <= 255;
  }

  // Call reset after changing history mode, so its byte state is seeded from
  // the full-refresh reference rather than interpreted as legacy budgets.
  bool setReversalPercent(uint16_t value) {
    if (value != 0 && value != 100 && value != 150 && value != 200) return false;
    if (value && (weakTail_ || stableTail_)) return false;
    const uint8_t black = blackPulses_ ? blackPulses_ : kPulseLimit;
    const uint8_t white = whitePulses_ ? whitePulses_ : kPulseLimit;
    if (value && !supportsHistory(black, white)) return false;
    reversalPercent_ = value;
    updateHistoryScale();
    return true;
  }
  uint16_t reversalPercent() const { return reversalPercent_; }

  // Experimental: LUT state 11 must first be verified as black-directed.
  // The half-strength ratio below is a bookkeeping assumption, not measured
  // optical strength. Call reset after changing units; disable before Net mode.
  bool setWeakTail(uint8_t strongEquivalent) {
    if (strongEquivalent > 4 ||
        (strongEquivalent && (reversalPercent_ || stableTail_ || mode_ == DriveMode::Net))) return false;
    weakTail_ = strongEquivalent;
    return true;
  }
  uint8_t weakTail() const { return weakTail_; }

  // Reset from a full-refresh reference after changing this mode. The packed
  // net history is private to stable tail: 12 signed count bits + 4 age bits.
  bool setStableTail(uint8_t pulses) {
    if ((pulses != 0 && pulses != 2 && pulses != 4) ||
        (pulses && (weakTail_ || reversalPercent_ || mode_ == DriveMode::Net))) return false;
    stableTail_ = pulses;
    return true;
  }
  uint8_t stableTail() const { return stableTail_; }
  uint32_t stableTailPulsesDelivered() const { return stableTailPulsesDelivered_; }
  bool hasStableTailPending() const {
    if (!stableTail_ || mode_ != DriveMode::Budget) return false;
    for (uint8_t value : active_) if (value) return true;
    return false;
  }

  bool plan(const uint8_t* next, bool settle = false, uint32_t nowMs = 0) {
    if (stableTail_ && mode_ == DriveMode::Budget) {
      stagedTimeMs_ = nowMs;
      return planStableTail(next, settle);
    }
    if (weakTail_ && mode_ == DriveMode::Budget)
      return settle ? planWeakTail<true>(next) : planWeakTail<false>(next);
    if (reversalPercent_ && mode_ == DriveMode::Budget)
      return settle ? planHistory<true>(next) : planHistory<false>(next);
    if (settle) return planPixels<true, false>(next);
    return mode_ == DriveMode::Net ? planPixels<false, true>(next) :
                                    planPixels<false, false>(next);
  }

  // Generate masks against a predicted committed state without consuming or
  // replacing its budgets. A discarded preview leaves delivered state intact.
  bool preview(const uint8_t* next, bool settle = false, uint32_t nowMs = 0) {
    if (stableTail_ && mode_ == DriveMode::Budget) {
      stagedTimeMs_ = nowMs;
      return planStableTail(next, settle);
    }
    if (weakTail_ && mode_ == DriveMode::Budget)
      return settle ? planWeakTail<true>(next) : planWeakTail<false, false>(next);
    if (reversalPercent_ && mode_ == DriveMode::Budget)
      return settle ? planHistory<true>(next) : planHistory<false>(next);
    if (settle) return planPixels<true, false>(next);
    return mode_ == DriveMode::Net ? planPixels<false, true>(next) :
                                    planPixels<false, false, false>(next);
  }

  // Called only when adopting the corresponding preview for the next scan.
  void applyPreview(const uint8_t* next, bool settle = false) {
    if (settle || mode_ == DriveMode::Net || reversalPercent_ || stableTail_) return;
    for (size_t i = 0; i < kFrameBytes; ++i) {
      uint8_t changed = next[i] ^ target_[i];
      while (changed) {
        const uint8_t mask = changed & -changed;
        changed &= uint8_t(changed - 1);
        const uint8_t budget = (next[i] & mask) ? whitePulses_ : blackPulses_;
        remaining_[i * 8 + pixelBit(mask)] = weakTail_ ? 2 * budget : budget;
      }
    }
  }

  void commit(const uint8_t* next, bool settle = false) {
    if (stableTail_ && mode_ == DriveMode::Budget) {
      updateStableTail(next, settle, true);
    } else if (weakTail_ && mode_ == DriveMode::Budget) {
      if (settle) commitWeakTail<true>();
      else commitWeakTail<false>();
    } else if (reversalPercent_ && mode_ == DriveMode::Budget) {
      if (settle) commitHistory<true>(next);
      else commitHistory<false>(next);
    } else if (mode_ == DriveMode::Net) commitPixels<false, true>(next);
    else if (settle) commitPixels<true, false>(next);
    else commitPixels<false, false>(next);
    if (!settle) memcpy(target_, next, kFrameBytes);
  }

  // Even when no scan was needed, accept the latest desired bitmap.
  void accept(const uint8_t* next) {
    if (stableTail_ && mode_ == DriveMode::Budget) updateStableTail(next, false, false);
    memcpy(target_, next, kFrameBytes);
  }
  int netAt(size_t pixel) const {
    return stableTail_ && mode_ == DriveMode::Budget ? stableNet(pixel) : net_[pixel];
  }
  uint8_t black[kFrameBytes]{};
  uint8_t white[kFrameBytes]{};

 private:
  static constexpr uint8_t kStableCount = 0x3f;
  static constexpr uint8_t kStableCandidate = 0x40;
  static constexpr uint8_t kStableDelivering = 0x80;
  static constexpr uint8_t kStableDone = 0xc0;
  static constexpr uint32_t kStableTickMs = 16;
  static constexpr uint8_t kStableAgeThreshold = 12;

  int stableNet(size_t pixel) const {
    const unsigned value = uint16_t(net_[pixel]) & 0x0fffu;
    return value & 0x0800u ? int(value) - 4096 : int(value);
  }
  uint8_t stableAge(size_t pixel, uint32_t elapsedTicks) const {
    const unsigned age = (uint16_t(net_[pixel]) >> 12) + elapsedTicks;
    return age < 15 ? uint8_t(age) : 15;
  }
  void storeStableHistory(size_t pixel, int net, uint8_t age) {
    net_[pixel] = int16_t((uint16_t(age) << 12) | (unsigned(net) & 0x0fffu));
  }
  uint32_t stableElapsedTicks() const {
    // Predicted pipeline times may be slightly ahead of a later direct plan.
    // Do not interpret that small backwards correction as a millis wrap.
    const int32_t elapsed = int32_t(stagedTimeMs_ - stableClockMs_);
    return stableClockValid_ && elapsed > 0 ? uint32_t(elapsed) / kStableTickMs : 0;
  }
  void advanceStableClock(uint32_t ticks) {
    if (!stableClockValid_) {
      stableClockMs_ = stagedTimeMs_;
      stableClockValid_ = true;
    } else stableClockMs_ += ticks * kStableTickMs;
  }

  // Age is saturated, not a wrapping 4-bit timestamp. A target change resets
  // its age in the current global tick; the 12-tick gate is about 177..192ms.
  // Planning and preview change only masks and the staged wall-clock sample.
  bool planStableTail(const uint8_t* next, bool maintenance) {
    const uint32_t ticks = stableElapsedTicks();
    bool any = false;
    for (size_t i = 0; i < kFrameBytes; ++i) {
      const uint8_t desired = next[i], changed = desired ^ target_[i];
      uint8_t todo = maintenance ? 0xff : (active_[i] | changed);
      uint8_t toBlack = 0, toWhite = 0;
      while (todo) {
        const uint8_t mask = todo & -todo;
        todo &= uint8_t(todo - 1);
        const size_t p = i * 8 + pixelBit(mask);
        const bool wantWhite = desired & mask;
        bool drive;
        if (maintenance) drive = wantWhite ? stableNet(p) > 0 : stableNet(p) < 0;
        else if (changed & mask) drive = true;
        else {
          const uint8_t state = remaining_[p] & 0xc0u;
          const uint8_t count = remaining_[p] & kStableCount;
          drive = count || (state == kStableCandidate && stableAge(p, ticks) >= kStableAgeThreshold);
        }
        if (drive) (wantWhite ? toWhite : toBlack) |= mask;
      }
      black[i] = toBlack; white[i] = toWhite;
      any |= (toBlack | toWhite) != 0;
    }
    return any;
  }

  void updateStableTail(const uint8_t* next, bool maintenance, bool delivered) {
    const uint32_t ticks = stableElapsedTicks();
    for (size_t i = 0; i < kFrameBytes; ++i) {
      const uint8_t changed = maintenance ? 0 : next[i] ^ target_[i];
      const uint8_t toBlack = delivered ? black[i] : 0;
      const uint8_t toWhite = delivered ? white[i] : 0;
      uint8_t todo = active_[i] | changed | toBlack | toWhite;
      uint8_t active = 0;
      while (todo) {
        const uint8_t mask = todo & -todo;
        todo &= uint8_t(todo - 1);
        const size_t p = i * 8 + pixelBit(mask);
        int net = stableNet(p);
        uint8_t age = stableAge(p, ticks);
        uint8_t value = remaining_[p];
        if (changed & mask) {
          value = kStableCandidate | ((next[i] & mask) ? whitePulses_ : blackPulses_);
          age = 0;
        }
        uint8_t state = value & 0xc0u, count = value & kStableCount;
        if ((toBlack | toWhite) & mask) {
          net += (toBlack & mask) ? 1 : -1;
          if (!maintenance) {
            if (state == kStableCandidate && count) --count;
            else if (state == kStableCandidate || state == kStableDelivering) {
              if (state == kStableCandidate) count = stableTail_;
              --count;
              ++stableTailPulsesDelivered_;
              state = count ? kStableDelivering : kStableDone;
            }
          }
        }
        remaining_[p] = state | count;
        storeStableHistory(p, net, age);
        if (state == kStableCandidate || (state == kStableDelivering && count)) active |= mask;
      }
      active_[i] = active;
    }
    advanceStableClock(ticks);
  }

  template<bool Settle, bool UpdateBudget = true>
  bool planWeakTail(const uint8_t* next) {
    bool any = false;
    for (size_t i = 0; i < kFrameBytes; ++i) {
      const uint8_t desired = next[i], changed = desired ^ target_[i];
      uint8_t todo = Settle ? 0xff : (active_[i] | changed);
      uint8_t toBlack = 0, toWhite = 0;
      while (todo) {
        const uint8_t mask = todo & -todo;
        todo &= uint8_t(todo - 1);
        const size_t p = i * 8 + pixelBit(mask);
        const bool wantWhite = desired & mask;
        uint8_t remaining = remaining_[p];
        bool drive;
        if constexpr (Settle) {
          // Periodic same-direction compensation remains one strong pulse.
          drive = wantWhite ? net_[p] > 0 : net_[p] < 0;
        } else {
          if (changed & mask) {
            remaining = 2 * (wantWhite ? whitePulses_ : blackPulses_);
            if constexpr (UpdateBudget) remaining_[p] = remaining;
          }
          drive = remaining != 0;
        }
        if (!drive) continue;
        if (wantWhite) toWhite |= mask;
        else {
          toBlack |= mask;
          if constexpr (!Settle) {
            if (remaining <= 2 * weakTail_) toWhite |= mask; // 11: weak black.
          }
        }
      }
      black[i] = toBlack; white[i] = toWhite;
      any |= (toBlack | toWhite) != 0;
    }
    return any;
  }

  template<bool Settle>
  void commitWeakTail() {
    for (size_t i = 0; i < kFrameBytes; ++i) {
      const uint8_t toBlack = black[i], toWhite = white[i];
      uint8_t todo = toBlack | toWhite;
      uint8_t active = active_[i] & uint8_t(~todo);
      while (todo) {
        const uint8_t mask = todo & -todo;
        todo &= uint8_t(todo - 1);
        const size_t p = i * 8 + pixelBit(mask);
        const bool black = toBlack & mask;
        const unsigned dose = black && (toWhite & mask) ? 1 : 2;
        net_[p] += black ? int(dose) : -int(dose);
        if constexpr (!Settle)
          remaining_[p] = remaining_[p] > dose ? remaining_[p] - dose : 0;
        if (remaining_[p]) active |= mask;
      }
      active_[i] = active;
    }
  }

  static uint8_t gcd(uint8_t a, uint8_t b) {
    while (b) { const uint8_t remainder = a % b; a = b; b = remainder; }
    return a;
  }

  void updateHistoryScale() {
    const uint8_t black = blackPulses_ ? blackPulses_ : kPulseLimit;
    const uint8_t white = whitePulses_ ? whitePulses_ : kPulseLimit;
    const unsigned maximum = unsigned(black / gcd(black, white)) * white;
    // Unsupported combinations remain usable with ordinary budgets only.
    historyMaximum_ = maximum <= 255 ? uint8_t(maximum) : 0;
    blackStep_ = historyMaximum_ / black;
    whiteStep_ = historyMaximum_ / white;
  }

  uint8_t retargetPosition(uint8_t position, bool wantWhite, bool changed) const {
    if (!changed || reversalPercent_ == 100 || position == 0 || position == historyMaximum_)
      return position;
    unsigned distance = wantWhite ? position : historyMaximum_ - position;
    distance = (distance * reversalPercent_ + 99) / 100;
    if (distance > historyMaximum_) distance = historyMaximum_;
    return wantWhite ? uint8_t(distance) : uint8_t(historyMaximum_ - distance);
  }

  template<bool Settle>
  bool planHistory(const uint8_t* next) {
    bool any = false;
    for (size_t i = 0; i < kFrameBytes; ++i) {
      const uint8_t desired = next[i], changed = desired ^ target_[i];
      uint8_t todo = Settle ? 0xff : (active_[i] | changed);
      uint8_t toBlack = 0, toWhite = 0;
      while (todo) {
        const uint8_t mask = todo & -todo;
        todo &= uint8_t(todo - 1);
        const size_t p = i * 8 + pixelBit(mask);
        const bool wantWhite = desired & mask;
        bool drive;
        if constexpr (Settle) drive = wantWhite ? net_[p] > 0 : net_[p] < 0;
        else {
          const uint8_t position = retargetPosition(remaining_[p], wantWhite, changed & mask);
          drive = wantWhite ? position != 0 : position != historyMaximum_;
        }
        if (drive) (wantWhite ? toWhite : toBlack) |= mask;
      }
      black[i] = toBlack; white[i] = toWhite;
      any |= (toBlack | toWhite) != 0;
    }
    return any;
  }

  template<bool Settle>
  void commitHistory(const uint8_t* next) {
    for (size_t i = 0; i < kFrameBytes; ++i) {
      const uint8_t toBlack = black[i], toWhite = white[i];
      uint8_t todo = toBlack | toWhite;
      uint8_t active = active_[i] & uint8_t(~todo);
      while (todo) {
        const uint8_t mask = todo & -todo;
        todo &= uint8_t(todo - 1);
        const size_t p = i * 8 + pixelBit(mask);
        const bool wantWhite = next[i] & mask;
        unsigned position = remaining_[p];
        if constexpr (!Settle)
          position = retargetPosition(position, wantWhite, (next[i] ^ target_[i]) & mask);
        if (toBlack & mask) {
          net_[p] += 1;
          position += blackStep_;
          if (position > historyMaximum_) position = historyMaximum_;
        } else {
          net_[p] -= 1;
          position = position > whiteStep_ ? position - whiteStep_ : 0;
        }
        remaining_[p] = uint8_t(position);
        if (wantWhite ? position != 0 : position != historyMaximum_) active |= mask;
      }
      active_[i] = active;
    }
  }

  // Index an isolated bit without a libgcc clz/ctz call on the C6's RV32IMAC.
  // Eight bytes of read-only data, indexed by a three-bit de Bruijn hash.
  static unsigned pixelBit(uint8_t bit) {
    static constexpr uint8_t index[8] = {7, 6, 5, 3, 0, 4, 1, 2};
    return index[(unsigned(bit) * 0x17u >> 5) & 7];
  }

  template<bool Settle, bool Net, bool UpdateBudget = true>
  bool planPixels(const uint8_t* next) {
    bool any = false;
    for (size_t i = 0; i < kFrameBytes; ++i) {
      const uint8_t desired = next[i];
      const uint8_t changed = desired ^ target_[i];
      uint8_t todo = Settle ? 0xff : (active_[i] | changed);
      uint8_t toBlack = 0, toWhite = 0;
      while (todo) {
        const uint8_t mask = todo & -todo;
        todo &= uint8_t(todo - 1);
        const size_t p = i * 8 + pixelBit(mask);
        const bool wantWhite = desired & mask;
        bool drive;
        if constexpr (Settle) {
          // Repay only in the direction of the displayed target.
          drive = wantWhite ? net_[p] > 0 : net_[p] < 0;
        } else if constexpr (Net) {
          const int value = net_[p] + (wantWhite ? -1 : 1);
          drive = value >= -whitePulses_ && value <= blackPulses_;
        } else {
          uint8_t remaining = remaining_[p];
          if (changed & mask) {
            remaining = wantWhite ? whitePulses_ : blackPulses_;
            if constexpr (UpdateBudget) remaining_[p] = remaining;
          }
          drive = remaining != 0;
        }
        if (drive) (wantWhite ? toWhite : toBlack) |= mask;
      }
      black[i] = toBlack;
      white[i] = toWhite;
      any |= (toBlack | toWhite) != 0;
    }
    return any;
  }

  template<bool Settle, bool Net>
  void commitPixels(const uint8_t* next) {
    for (size_t i = 0; i < kFrameBytes; ++i) {
      const uint8_t toBlack = black[i], toWhite = white[i];
      uint8_t todo = toBlack | toWhite;
      uint8_t active = active_[i] & uint8_t(~todo);
      while (todo) {
        const uint8_t mask = todo & -todo;
        todo &= uint8_t(todo - 1);
        const size_t p = i * 8 + pixelBit(mask);
        const int value = net_[p] + ((toBlack & mask) ? 1 : -1);
        net_[p] = value;
        if constexpr (!Settle && !Net) {
          if (remaining_[p]) --remaining_[p];
        }
        bool needsMore;
        if constexpr (Net) {
          needsMore = (next[i] & mask) ? value > -whitePulses_ : value < blackPulses_;
        } else needsMore = remaining_[p] != 0;
        if (needsMore) active |= mask;
      }
      active_[i] = active;
    }
  }

  DriveMode mode_ = DriveMode::Budget;
  uint8_t blackPulses_ = 0, whitePulses_ = 0;
  uint8_t weakTail_ = 0;
  uint8_t stableTail_ = 0;
  uint16_t reversalPercent_ = 0;
  uint8_t historyMaximum_ = 0, blackStep_ = 0, whiteStep_ = 0;
  bool stableClockValid_ = false;
  uint32_t stableClockMs_ = 0, stagedTimeMs_ = 0, stableTailPulsesDelivered_ = 0;
  uint8_t active_[kFrameBytes]{};
  uint8_t target_[kFrameBytes]{};
  // With weak tail: half-strong pulse budgets; with both modes off: ordinary
  // pulse budgets. With history on: estimated
  // blackness in 0..LCM(B,W), where each full black/white drive has B/W steps.
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
