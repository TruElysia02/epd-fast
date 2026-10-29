#include "planner.h"
#include <cassert>
#include <cstdio>
#include <random>

static Planner planner, direct, live, shadow;
static uint8_t image[kFrameBytes], current[kFrameBytes], future[kFrameBytes], replacement[kFrameBytes];

static void configure(unsigned black, unsigned white, unsigned percent) {
  assert(planner.setReversalPercent(0));
  assert(planner.setPulses(black, white));
  assert(planner.setReversalPercent(percent));
  memset(image, 0xff, sizeof(image));
  planner.reset(DriveMode::Budget, image);
}

static void target(bool white) {
  if (white) image[0] |= 0x80;
  else image[0] &= 0x7f;
}

static void pulse(bool white) {
  target(white);
  assert(planner.plan(image));
  assert((white ? planner.white[0] : planner.black[0]) & 0x80);
  planner.commit(image);
}

static unsigned finish(bool white) {
  target(white);
  unsigned count = 0;
  while (planner.plan(image)) {
    assert(++count <= 64);
    planner.commit(image);
  }
  return count;
}

static void sameMasks() {
  assert(!memcmp(direct.black, live.black, kFrameBytes));
  assert(!memcmp(direct.white, live.white, kFrameBytes));
}

static void sameDeliveredHistory() {
  for (size_t p = 0; p < kPixels; ++p) assert(direct.netAt(p) == live.netAt(p));
}

static void mutate(uint8_t* frame, std::mt19937& random, bool dense) {
  if (dense) for (size_t i = 0; i < kFrameBytes; ++i) frame[i] = random();
  else for (unsigned bit = 0; bit < 20; ++bit)
    frame[random() % kFrameBytes] ^= uint8_t(1u << (random() % 8));
}

int main() {
  static_assert(sizeof(Planner) < 133000, "History must not add per-pixel RAM");
  assert(planner.reversalPercent() == 0);
  assert(Planner::supportsHistory(7, 11));
  assert(Planner::supportsHistory(15, 17)); // Largest representable LCM, 255.
  assert(!Planner::supportsHistory(31, 32));
  assert(!Planner::supportsHistory(0, 11));

  // Full transitions retain exact pulse counts for every supported setting.
  for (unsigned black = 1; black <= 32; ++black)
    for (unsigned white = 1; white <= 32; ++white) {
      if (!Planner::supportsHistory(black, white)) continue;
      configure(black, white, 150);
      assert(finish(false) == black);
      assert(finish(true) == white);
      assert(planner.netAt(0) == int(black) - int(white));
    }

  for (unsigned percent : {100u, 150u, 200u}) {
    configure(11, 11, percent);
    pulse(false);
    assert(finish(true) == (percent == 100 ? 1u : 2u));
    // A complete transition does not receive reversal amplification.
    configure(11, 11, percent);
    assert(finish(false) == 11);
    assert(finish(true) == 11);
    assert(planner.netAt(0) == 0);
  }

  // Multiple interrupted reversals retain reached position rather than
  // loading another full budget or treating every new direction as position0.
  configure(11, 11, 100);
  pulse(false); pulse(false); pulse(false); // Position3.
  pulse(true); // Position2.
  assert(finish(false) == 9);
  assert(planner.netAt(0) == 11);
  configure(11, 11, 150);
  pulse(false); pulse(false); pulse(false);
  assert(finish(true) == 5); // ceil(3 * 150%) doses, once per retarget.
  assert(planner.netAt(0) == -2);

  // Planning, previewing and adopting a preview never apply the re-estimate.
  configure(11, 11, 150);
  pulse(false);
  target(true);
  for (int retry = 0; retry < 5; ++retry) {
    assert(planner.plan(image));
    assert(planner.preview(image));
    planner.applyPreview(image);
    assert(planner.netAt(0) == 1);
  }
  assert(finish(true) == 2);
  configure(11, 11, 150);
  pulse(false);
  target(true); assert(planner.preview(image)); planner.applyPreview(image);
  assert(finish(false) == 10); // Abandoned reversal did not alter position1.

  // Asymmetric black/white steps share an exact77-unit position scale.
  configure(7, 11, 100);
  pulse(false);
  assert(finish(true) == 2); // 11 black-units, seven units per white pulse.
  configure(7, 11, 150);
  pulse(false);
  assert(finish(true) == 3); // ceil(11*1.5)=17 units, three white pulses.

  // Maintenance changes both real pulse history and estimated position.
  configure(7, 11, 100);
  assert(finish(false) == 7); assert(finish(true) == 11);
  assert(planner.netAt(0) == -4);
  target(false);
  assert(planner.plan(image, true)); planner.commit(image, true);
  assert(planner.netAt(0) == -3);
  assert(finish(false) == 6); // Maintenance already supplied one black step.
  assert(planner.netAt(0) == 3);

  // Invalid configurations are rejected explicitly and preserve old values.
  configure(11, 11, 150);
  assert(!planner.setReversalPercent(101));
  assert(planner.reversalPercent() == 150);
  assert(!planner.setPulses(31, 32));
  assert(finish(false) == 11);
  assert(planner.setReversalPercent(0));
  assert(planner.setPulses(31, 32)); // Ordinary budgets retain their full range.
  assert(!planner.setReversalPercent(150));
  assert(planner.reversalPercent() == 0);

  // History does not replace the separate net-limited strategy.
  configure(11, 11, 150);
  planner.reset(DriveMode::Net, image);
  assert(finish(true) == 11);
  assert(finish(false) == 22);
  assert(planner.netAt(0) == 11);

  std::mt19937 random(725);
  for (unsigned percent : {100u, 150u, 200u}) {
    memset(current, 0xff, sizeof(current));
    for (Planner* p : {&direct, &live}) {
      assert(p->setPulses(3, 11));
      assert(p->setReversalPercent(percent));
      p->reset(DriveMode::Budget, current);
    }
    for (unsigned n = 0; n < 360; ++n) {
      mutate(current, random, n % 41 == 0);
      const bool maintenance = n % 19 == 0;
      const bool driven = direct.plan(current, maintenance);
      assert(live.plan(current, maintenance) == driven); sameMasks();
      shadow = live;
      if (driven) shadow.commit(current, maintenance);
      else if (!maintenance) shadow.accept(current);
      memcpy(future, current, kFrameBytes);
      mutate(future, random, n % 17 == 0);
      const bool futureMaintenance = n % 23 == 0;
      const bool futureDriven = shadow.preview(future, futureMaintenance);
      sameDeliveredHistory();
      if (n % 29 == 0) {
        // Fault: discard the speculative object without advancing live state.
        assert(live.plan(current, maintenance) == driven);
        sameMasks(); sameDeliveredHistory();
        direct.reset(DriveMode::Budget, current); live.reset(DriveMode::Budget, current);
        continue;
      }
      if (driven) direct.commit(current, maintenance);
      else if (!maintenance) direct.accept(current);
      live = shadow; sameDeliveredHistory();
      bool nextDriven, nextMaintenance = futureMaintenance;
      if (n % 3 == 0) {
        memcpy(replacement, current, kFrameBytes);
        mutate(replacement, random, n % 7 == 0);
        nextMaintenance = !futureMaintenance;
        nextDriven = live.plan(replacement, nextMaintenance);
        assert(direct.plan(replacement, nextMaintenance) == nextDriven);
        memcpy(future, replacement, kFrameBytes);
      } else {
        live.applyPreview(future, futureMaintenance);
        nextDriven = futureDriven;
        assert(direct.plan(future, futureMaintenance) == nextDriven);
      }
      sameMasks();
      if (nextDriven) { direct.commit(future, nextMaintenance); live.commit(future, nextMaintenance); }
      else if (!nextMaintenance) { direct.accept(future); live.accept(future); }
      sameDeliveredHistory();
      memcpy(current, future, kFrameBytes);
    }
  }
  puts("PASS: exact history doses, interrupted reversals, asymmetric steps, maintenance, rejection, preview/abort equivalence");
}
