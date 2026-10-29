#include "planner.h"
#include <cassert>
#include <cstdio>
#include <random>

static Planner direct, live, shadow;
static uint8_t current[kFrameBytes], future[kFrameBytes], replacement[kFrameBytes];

static void sameMasks(const Planner& a, const Planner& b) {
  assert(!memcmp(a.black, b.black, kFrameBytes));
  assert(!memcmp(a.white, b.white, kFrameBytes));
}

static void sameHistory(const Planner& a, const Planner& b) {
  for (size_t p = 0; p < kPixels; ++p) assert(a.netAt(p) == b.netAt(p));
}

static void mutate(uint8_t* image, std::mt19937& random, bool dense) {
  if (dense) for (size_t i = 0; i < kFrameBytes; ++i) image[i] = random();
  else for (unsigned j = 0; j < 20; ++j)
    image[random() % kFrameBytes] ^= uint8_t(1u << (random() % 8));
}

int main() {
  std::mt19937 random(701);
  for (DriveMode mode : {DriveMode::Budget, DriveMode::Net}) {
    memset(current, 0xff, sizeof(current));
    direct.reset(mode, current); live.reset(mode, current);
    direct.setPulses(3, 11); live.setPulses(3, 11);
    for (unsigned n = 0; n < 600; ++n) {
      mutate(current, random, n % 41 == 0);
      const bool maintenance = n % 19 == 0;
      const bool driven = direct.plan(current, maintenance);
      assert(live.plan(current, maintenance) == driven);
      sameMasks(live, direct);

      shadow = live;
      if (driven) shadow.commit(current, maintenance);
      else if (!maintenance) shadow.accept(current);
      memcpy(future, current, kFrameBytes);
      mutate(future, random, n % 17 == 0);
      const bool futureMaintenance = n % 23 == 0;
      const bool futureDriven = shadow.preview(future, futureMaintenance);
      sameHistory(live, direct); // No early commit to the live state.

      if (n % 29 == 0) {
        // A failed transport discards the shadow; only a reset can resume.
        assert(live.plan(current, maintenance) == driven);
        sameMasks(live, direct);
        sameHistory(live, direct);
        direct.reset(mode, current); live.reset(mode, current);
        continue;
      }

      if (driven) direct.commit(current, maintenance);
      else if (!maintenance) direct.accept(current);
      live = shadow; // Represents pointer handoff after successful completion.
      sameHistory(live, direct);
      bool nextDriven;
      bool nextMaintenance = futureMaintenance;
      if (n % 3 == 0) {
        // A command or maintenance boundary invalidates the preview. The
        // unselected future must not leak its reset budgets into a new plan.
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
      sameMasks(live, direct);
      if (nextDriven) {
        direct.commit(future, nextMaintenance); live.commit(future, nextMaintenance);
      } else if (!nextMaintenance) {
        direct.accept(future); live.accept(future);
      }
      sameHistory(live, direct);
      memcpy(current, future, kFrameBytes);
    }
  }
  puts("PASS: preview/adoption equivalence, cancellation, maintenance changes, failed-scan discard, unchanged live history");
}
