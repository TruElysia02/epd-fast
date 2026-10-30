#include "planner.h"
#include <cassert>
#include <cstdio>
#include <random>

static Planner planner, direct, live, shadow;
static uint8_t image[kFrameBytes], current[kFrameBytes], future[kFrameBytes];

static unsigned code(const Planner& p, size_t pixel = 0) {
  const uint8_t mask = 0x80u >> (pixel % 8);
  return ((p.black[pixel / 8] & mask) ? 2u : 0u) |
         ((p.white[pixel / 8] & mask) ? 1u : 0u);
}

static void configure(unsigned black, unsigned white, unsigned tail,
                      uint32_t nowMs = 0, bool initialWhite = true) {
  assert(planner.setStableTail(0));
  assert(planner.setWeakTail(0));
  assert(planner.setReversalPercent(0));
  memset(image, initialWhite ? 0xff : 0, sizeof(image));
  planner.reset(DriveMode::Budget, image);
  assert(planner.setPulses(black, white));
  assert(planner.setStableTail(tail));
  planner.reset(DriveMode::Budget, image);
  assert(!planner.plan(image, false, nowMs));
  planner.accept(image);
}

static bool step(uint32_t nowMs, bool maintenance = false) {
  const bool driven = planner.plan(image, maintenance, nowMs);
  if (driven) planner.commit(image, maintenance);
  else if (!maintenance) planner.accept(image);
  return driven;
}

static void sameMasks(const Planner& a, const Planner& b) {
  assert(!memcmp(a.black, b.black, kFrameBytes));
  assert(!memcmp(a.white, b.white, kFrameBytes));
}

static void sameHistory(const Planner& a, const Planner& b) {
  assert(a.stableTailPulsesDelivered() == b.stableTailPulsesDelivered());
  assert(a.hasStableTailPending() == b.hasStableTailPending());
  for (size_t p = 0; p < kPixels; ++p) assert(a.netAt(p) == b.netAt(p));
}

static void mutate(uint8_t* frame, std::mt19937& random, bool dense) {
  if (dense) for (size_t i = 0; i < kFrameBytes; ++i) frame[i] = random();
  else for (unsigned bit = 0; bit < 20; ++bit)
    frame[random() % kFrameBytes] ^= uint8_t(1u << (random() % 8));
}

int main() {
  static_assert(sizeof(Planner) < 133000, "Stable tail must reuse existing pixel storage");
  assert(planner.stableTail() == 0);
  assert(!planner.setStableTail(1));
  assert(!planner.setStableTail(3));
  assert(!planner.setStableTail(5));
  assert(planner.setWeakTail(1));
  assert(!planner.setStableTail(2));
  assert(planner.setWeakTail(0));
  assert(planner.setReversalPercent(150));
  assert(!planner.setStableTail(2));
  assert(planner.setReversalPercent(0));
  assert(planner.setStableTail(2));
  assert(!planner.setWeakTail(1));
  assert(!planner.setReversalPercent(100));
  assert(planner.setStableTail(0));
  planner.reset(DriveMode::Net, image);
  assert(!planner.setStableTail(2));

  for (unsigned tail : {2u, 4u}) {
    configure(3, 3, tail);
    assert(!step(20000)); // Initial unchanged pixels are never candidates.
    assert(!planner.hasStableTailPending());
    assert(planner.stableTailPulsesDelivered() == 0);

    configure(3, 3, tail);
    image[kFrameBytes - 1] &= 0xfe; // Last pixel also exercises native row 295.
    for (uint32_t time : {7u, 17u, 27u}) {
      assert(step(time)); assert(code(planner, kPixels - 1) == 2);
      assert(planner.stableTailPulsesDelivered() == 0);
    }
    assert(planner.netAt(kPixels - 1) == 3);
    assert(planner.hasStableTailPending());
    // Age follows real milliseconds, not the number of calls or idle loops.
    for (unsigned n = 0; n < 100; ++n) assert(!step(100));
    assert(!step(191));
    for (unsigned n = 0; n < tail; ++n) {
      assert(step(192 + n * 10));
      assert(code(planner, kPixels - 1) == 2);
      assert(planner.stableTailPulsesDelivered() == n + 1);
    }
    assert(planner.netAt(kPixels - 1) == int(3 + tail));
    assert(!planner.hasStableTailPending());
    for (uint32_t time : {1000u, 10000u, 20000u}) assert(!step(time));
    assert(planner.stableTailPulsesDelivered() == tail);
  }

  // A slow ordinary budget must finish before even an already-old pixel can
  // start its extra pulses; a long delay saturates age instead of wrapping it.
  configure(32, 32, 2); image[0] &= 0x7f;
  for (unsigned n = 0; n < 32; ++n) {
    assert(step(1 + n * 1000));
    assert(planner.stableTailPulsesDelivered() == 0);
  }
  assert(step(32001)); assert(step(32002));
  assert(planner.netAt(0) == 34);
  assert(planner.stableTailPulsesDelivered() == 2);
  assert(!step(40000));

  // A reversal cancels the previous direction's unfinished tail and starts a
  // new ordinary budget/age. Only the new target may receive later pulses.
  configure(1, 1, 4); image[0] &= 0x7f;
  assert(step(1)); assert(step(192));
  assert(planner.stableTailPulsesDelivered() == 1);
  image[0] |= 0x80;
  assert(step(193)); assert(code(planner) == 1);
  assert(planner.stableTailPulsesDelivered() == 1);
  assert(!step(383));
  for (unsigned n = 0; n < 4; ++n) {
    assert(step(384 + n)); assert(code(planner) == 1);
  }
  assert(planner.stableTailPulsesDelivered() == 5);
  assert(!step(1000));

  // Maintenance is a delivered net correction, not a consumed ordinary or
  // tail pulse. Its elapsed wall time still ages waiting candidates.
  configure(1, 3, 2);
  image[0] &= 0x7f; assert(step(1));
  image[0] |= 0x80; assert(step(2)); assert(step(3)); assert(step(4));
  image[0] &= 0x7f; assert(step(5));
  assert(planner.netAt(0) == -1);
  assert(step(192, true)); assert(planner.netAt(0) == 0);
  assert(planner.stableTailPulsesDelivered() == 0);
  assert(step(193)); assert(step(194));
  assert(planner.stableTailPulsesDelivered() == 2);
  assert(!planner.hasStableTailPending());

  // The 12-bit signed count is independent of all four age bits, including
  // their saturated value. Inject known successful strong scan masks to
  // exercise both extrema allowed before the mandatory 2000-scan clean.
  for (bool positive : {false, true}) {
    configure(1, 1, 2, 0, positive);
    image[0] ^= 0x80;
    assert(step(1));
    for (unsigned n = 2; n <= kCleanScans; ++n) {
      planner.plan(image, true, n * 8);
      memset(planner.black, 0, kFrameBytes); memset(planner.white, 0, kFrameBytes);
      (positive ? planner.black : planner.white)[0] = 0x80;
      planner.commit(image, true);
      assert(planner.netAt(0) == (positive ? int(n) : -int(n)));
    }
    assert(planner.stableTailPulsesDelivered() == 0);
  }

  // Preview and a failed/uncommitted plan may change masks, but must not age
  // the delivered state, create a tail budget, or increment delivered counts.
  configure(1, 1, 2); image[0] &= 0x7f; assert(step(1));
  assert(planner.preview(image, false, 1000));
  assert(planner.netAt(0) == 1 && planner.stableTailPulsesDelivered() == 0);
  assert(!step(100)); // Discard the future preview.
  assert(planner.plan(image, false, 1000)); // Simulate failed scan: no commit.
  assert(!step(191));
  assert(step(192));
  assert(planner.stableTailPulsesDelivered() == 1);

  // Quantization can credit at most the remainder of the current 16ms tick:
  // change at 15ms receives its first tail at 192ms, 177ms later.
  configure(1, 1, 2); image[0] &= 0x7f; assert(step(15));
  assert(!step(191)); assert(step(192));

  // unsigned milliseconds wrap is a short forward interval, not a huge age.
  const uint32_t start = UINT32_MAX - 100;
  configure(1, 1, 2, start); image[0] &= 0x7f;
  assert(step(start + 1));
  assert(!step(start + 191)); assert(step(start + 192));
  assert(planner.stableTailPulsesDelivered() == 1);

  // Direct and pipeline execution must agree across preview adoption,
  // cancellation, maintenance changes, sparse/dense changes and idle waits.
  std::mt19937 random(9241);
  for (unsigned tail : {2u, 4u}) {
    memset(current, 0xff, sizeof(current));
    assert(direct.setPulses(3, 11)); assert(live.setPulses(3, 11));
    assert(direct.setStableTail(tail)); assert(live.setStableTail(tail));
    direct.reset(DriveMode::Budget, current); live.reset(DriveMode::Budget, current);
    uint32_t now = 0;
    for (unsigned n = 0; n < 400; ++n) {
      now += 3 + random() % 40;
      if (n % 7 == 0) mutate(current, random, n % 91 == 0);
      const bool maintenance = n % 19 == 0;
      const bool driven = direct.plan(current, maintenance, now);
      assert(live.plan(current, maintenance, now) == driven);
      sameMasks(direct, live); sameHistory(direct, live);
      shadow = live;
      if (driven) shadow.commit(current, maintenance);
      else if (!maintenance) shadow.accept(current);
      memcpy(future, current, kFrameBytes);
      if (n % 5 == 0) mutate(future, random, n % 67 == 0);
      uint32_t futureMs = now + 3 + random() % 40;
      bool nextMaintenance = n % 23 == 0;
      bool futureDriven = shadow.preview(future, nextMaintenance, futureMs);
      sameHistory(direct, live); // Speculation never commits the live state.
      if (driven) direct.commit(current, maintenance);
      else if (!maintenance) direct.accept(current);
      live = shadow;
      sameHistory(direct, live);
      if (n % 3 == 0) {
        memcpy(future, current, kFrameBytes);
        mutate(future, random, n % 53 == 0);
        futureMs = now + 1; // Earlier than the discarded speculative time.
        nextMaintenance = !nextMaintenance;
        futureDriven = live.plan(future, nextMaintenance, futureMs);
      } else live.applyPreview(future, nextMaintenance);
      assert(direct.plan(future, nextMaintenance, futureMs) == futureDriven);
      sameMasks(direct, live);
      if (futureDriven) { direct.commit(future, nextMaintenance); live.commit(future, nextMaintenance); }
      else if (!nextMaintenance) { direct.accept(future); live.accept(future); }
      sameHistory(direct, live);
      memcpy(current, future, kFrameBytes);
      now = futureMs;
    }
  }
  planner.reset(DriveMode::Budget, image);
  assert(planner.stableTailPulsesDelivered() == 0);
  assert(!planner.hasStableTailPending());
  std::printf("PASS: timed bounded stable tail, reversals, maintenance, packed history, wrap, pipeline; planner_bytes=%u\n",
              unsigned(sizeof(Planner)));
}
