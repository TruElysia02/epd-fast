#include "planner.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <random>

static Planner planner, direct, live, shadow;
static uint8_t image[kFrameBytes], current[kFrameBytes], future[kFrameBytes];

static void configure(unsigned black, unsigned white, unsigned tail) {
  assert(planner.setReversalPercent(0));
  assert(planner.setWeakTail(0));
  memset(image, 0xff, sizeof(image));
  planner.reset(DriveMode::Budget, image);
  assert(planner.setPulses(black, white));
  assert(planner.setWeakTail(tail));
  planner.reset(DriveMode::Budget, image);
}

static unsigned code(const Planner& p, size_t pixel = 0) {
  const uint8_t mask = 0x80u >> (pixel % 8);
  return ((p.black[pixel / 8] & mask) ? 2u : 0u) |
         ((p.white[pixel / 8] & mask) ? 1u : 0u);
}

struct Reference {
  uint8_t target[kFrameBytes]{}, remaining[kPixels]{};
  int16_t net[kPixels]{};
  uint8_t black[kFrameBytes]{}, white[kFrameBytes]{};
  bool plan(const uint8_t* next, unsigned b, unsigned w, unsigned tail, bool settle) {
    memset(black, 0, sizeof(black)); memset(white, 0, sizeof(white));
    bool any = false;
    for (size_t p = 0; p < kPixels; ++p) {
      const size_t i = p / 8;
      const uint8_t mask = 0x80u >> (p % 8);
      const bool wantWhite = next[i] & mask;
      if (!settle && ((target[i] ^ next[i]) & mask)) remaining[p] = 2 * (wantWhite ? w : b);
      const bool drive = settle ? (wantWhite ? net[p] > 0 : net[p] < 0) : remaining[p] != 0;
      if (!drive) continue;
      any = true;
      if (wantWhite) white[i] |= mask;
      else {
        black[i] |= mask;
        if (!settle && remaining[p] <= 2 * tail) white[i] |= mask;
      }
    }
    return any;
  }
  void commit(const uint8_t* next, bool settle) {
    for (size_t p = 0; p < kPixels; ++p) {
      const uint8_t mask = 0x80u >> (p % 8);
      const bool b = black[p / 8] & mask, w = white[p / 8] & mask;
      if (!b && !w) continue;
      const unsigned dose = b && w ? 1 : 2;
      net[p] += b ? int(dose) : -int(dose);
      if (!settle) remaining[p] = remaining[p] > dose ? remaining[p] - dose : 0;
    }
    if (!settle) memcpy(target, next, kFrameBytes);
  }
};
static Reference ref;

static void mutate(uint8_t* frame, std::mt19937& random, bool dense) {
  if (dense) for (size_t i = 0; i < kFrameBytes; ++i) frame[i] = random();
  else for (unsigned bit = 0; bit < 20; ++bit)
    frame[random() % kFrameBytes] ^= uint8_t(1u << (random() % 8));
}

static void sameMasks(const Planner& a, const Planner& b) {
  assert(!memcmp(a.black, b.black, kFrameBytes));
  assert(!memcmp(a.white, b.white, kFrameBytes));
}

int main() {
  static_assert(sizeof(Planner) < 133000, "Weak tail must not add pixel arrays");
  assert(planner.weakTail() == 0);
  assert(!planner.setWeakTail(5));
  assert(planner.setReversalPercent(150));
  assert(!planner.setWeakTail(1));
  assert(planner.setReversalPercent(0));
  assert(planner.setWeakTail(1));
  assert(!planner.setReversalPercent(100));
  assert(planner.setWeakTail(0));
  memset(image, 0xff, sizeof(image));
  planner.reset(DriveMode::Net, image);
  assert(!planner.setWeakTail(1));

  for (unsigned b : {1u, 3u, 11u, 32u}) for (unsigned tail : {1u, 2u, 4u}) {
    configure(b, 11, tail);
    assert(!planner.plan(image));
    image[kFrameBytes - 1] &= 0xfe; // Native row 295, last bit.
    unsigned strong = 0, weak = 0;
    while (planner.plan(image)) {
      const unsigned state = code(planner, kPixels - 1);
      assert(state == 2 || state == 3);
      if (state == 2) ++strong; else ++weak;
      planner.commit(image);
    }
    const unsigned count = std::min(b, tail);
    assert(strong == b - count && weak == 2 * count);
    assert(planner.netAt(kPixels - 1) == int(2 * b));
    image[kFrameBytes - 1] |= 1;
    for (unsigned n = 0; n < 11; ++n) {
      assert(planner.plan(image)); assert(code(planner, kPixels - 1) == 1);
      planner.commit(image);
    }
    assert(!planner.plan(image));
    assert(planner.netAt(kPixels - 1) == int(2 * b) - 22);
  }

  // A strong white delivery with only one half-unit left must clip its budget,
  // while its delivered history still records the full strong pulse (-2).
  configure(3, 3, 1); image[0] &= 0x7f;
  for (unsigned n = 0; n < 3; ++n) { assert(planner.plan(image)); planner.commit(image); }
  assert(planner.netAt(0) == 5); // Two strong black doses, one weak dose; 1 remains.
  memset(planner.black, 0, kFrameBytes); memset(planner.white, 0, kFrameBytes);
  planner.white[0] = 0x80; image[0] |= 0x80;
  planner.commit(image);
  assert(planner.netAt(0) == 3);
  assert(!planner.plan(image));

  std::mt19937 random(1701);
  for (unsigned tail : {1u, 4u}) {
    configure(3, 11, tail);
    ref = {}; memcpy(ref.target, image, kFrameBytes);
    for (unsigned n = 0; n < 300; ++n) {
      mutate(image, random, n % 37 == 0);
      const bool settle = n % 19 == 0;
      const bool driven = planner.plan(image, settle);
      assert(ref.plan(image, 3, 11, tail, settle) == driven);
      assert(!memcmp(planner.black, ref.black, kFrameBytes));
      assert(!memcmp(planner.white, ref.white, kFrameBytes));
      for (size_t p = 0; p < kPixels; ++p) assert(planner.netAt(p) == ref.net[p]);
      if (driven) { planner.commit(image, settle); ref.commit(image, settle); }
      else if (!settle) { planner.accept(image); memcpy(ref.target, image, kFrameBytes); }
    }
  }

  // Shadow promotion, cancellation and maintenance switching must agree with
  // direct planning even though weak black sets both mask planes.
  for (unsigned tail : {1u, 4u}) {
    memset(current, 0xff, sizeof(current));
    assert(direct.setPulses(3, 11)); assert(live.setPulses(3, 11));
    assert(direct.setWeakTail(tail)); assert(live.setWeakTail(tail));
    direct.reset(DriveMode::Budget, current); live.reset(DriveMode::Budget, current);
    for (unsigned n = 0; n < 400; ++n) {
      mutate(current, random, n % 37 == 0);
      const bool maintenance = n % 17 == 0;
      const bool driven = direct.plan(current, maintenance);
      assert(live.plan(current, maintenance) == driven); sameMasks(direct, live);
      shadow = live;
      if (driven) shadow.commit(current, maintenance);
      else if (!maintenance) shadow.accept(current);
      memcpy(future, current, kFrameBytes); mutate(future, random, n % 23 == 0);
      bool nextMaintenance = n % 19 == 0;
      bool futureDriven = shadow.preview(future, nextMaintenance);
      for (size_t p = 0; p < kPixels; ++p) assert(live.netAt(p) == direct.netAt(p));
      if (driven) direct.commit(current, maintenance);
      else if (!maintenance) direct.accept(current);
      live = shadow;
      if (n % 3 == 0) {
        memcpy(future, current, kFrameBytes); mutate(future, random, n % 13 == 0);
        nextMaintenance = !nextMaintenance;
        futureDriven = live.plan(future, nextMaintenance);
      } else live.applyPreview(future, nextMaintenance);
      assert(direct.plan(future, nextMaintenance) == futureDriven); sameMasks(direct, live);
      if (futureDriven) { direct.commit(future, nextMaintenance); live.commit(future, nextMaintenance); }
      else if (!nextMaintenance) { direct.accept(future); live.accept(future); }
      for (size_t p = 0; p < kPixels; ++p) assert(live.netAt(p) == direct.netAt(p));
      memcpy(current, future, kFrameBytes);
    }
  }
  std::printf("PASS: weak-tail states, half-unit doses, clipping, row295, scalar equivalence, pipeline; planner_bytes=%u\n",
              unsigned(sizeof(Planner)));
}
