#include "planner.h"
#include <cassert>
#include <cstdio>
#include <random>

// Scalar reference, deliberately walking every pixel. Exercise sparse caches
// against dense and sparse changes, mid-budget reversals and maintenance.
struct Reference {
  uint8_t target[kFrameBytes]{}, remaining[kPixels]{};
  int16_t net[kPixels]{};
  uint8_t black[kFrameBytes]{}, white[kFrameBytes]{};
  bool step(const uint8_t* next, DriveMode mode, unsigned bLimit, unsigned wLimit, bool settle) {
    memset(black, 0, sizeof(black)); memset(white, 0, sizeof(white));
    bool any = false;
    for (size_t p = 0; p < kPixels; ++p) {
      const size_t i = p / 8;
      const uint8_t bit = 0x80u >> (p % 8);
      const bool w = next[i] & bit;
      bool driven;
      if (settle) driven = w ? net[p] > 0 : net[p] < 0;
      else if (mode == DriveMode::Net) {
        int candidate = net[p] + (w ? -1 : 1);
        driven = candidate >= -int(wLimit) && candidate <= int(bLimit);
      } else {
        if ((next[i] ^ target[i]) & bit) remaining[p] = w ? wLimit : bLimit;
        driven = remaining[p] != 0;
      }
      if (driven) { (w ? white[i] : black[i]) |= bit; any = true; }
    }
    return any;
  }
  void commit(const uint8_t* next, DriveMode mode, bool settle) {
    for (size_t p = 0; p < kPixels; ++p) {
      const uint8_t bit = 0x80u >> (p % 8);
      if (!((black[p / 8] | white[p / 8]) & bit)) continue;
      net[p] += (black[p / 8] & bit) ? 1 : -1;
      if (!settle && mode == DriveMode::Budget && remaining[p]) --remaining[p];
    }
    if (!settle) memcpy(target, next, kFrameBytes);
  }
};
static Planner fast;
static Reference ref;
static uint8_t image[kFrameBytes], controllerBlack[kFrameBytes], controllerWhite[kFrameBytes];

int main() {
  std::mt19937 random(29);
  for (DriveMode mode : {DriveMode::Budget, DriveMode::Net}) {
    ref = {}; memset(image, 0xff, sizeof(image));
    memcpy(ref.target, image, sizeof(image));
    fast.reset(mode, image); fast.setPulses(3, 9);
    for (int n = 0; n < 240; ++n) {
      if (n % 61 == 0) for (auto& x : image) x = random();
      else if (n % 13 < 6) for (int k = 0; k < 12; ++k) image[random() % kFrameBytes] ^= 1u << (random() % 8);
      const bool settle = mode == DriveMode::Budget && n % 47 == 0;
      bool a = fast.plan(image, settle), b = ref.step(image, mode, 3, 9, settle);
      assert(a == b);
      assert(!memcmp(fast.black, ref.black, kFrameBytes));
      assert(!memcmp(fast.white, ref.white, kFrameBytes));
      if (a) {
        fast.commit(image, settle); ref.commit(image, mode, settle);
      } else if (!settle) { fast.accept(image); memcpy(ref.target, image, kFrameBytes); }
      for (size_t p = 0; p < kPixels; ++p) assert(fast.netAt(p) == ref.net[p]);
    }
  }
  // Replanning the same target before commit may refresh budgets, but must
  // not change masks or delivered net. A real transport fault latches stopped;
  // arbitrary target changes after an abandoned plan are not a supported retry.
  for (unsigned seed : {7u, 29u, 301u}) {
    std::mt19937 trial(seed);
    for (DriveMode mode : {DriveMode::Budget, DriveMode::Net}) {
      ref = {}; memset(image, 0xff, sizeof(image));
      memcpy(ref.target, image, sizeof(image));
      const unsigned blackLimit = 1 + trial() % 32, whiteLimit = 1 + trial() % 32;
      fast.reset(mode, image); fast.setPulses(blackLimit, whiteLimit);
      for (unsigned n = 0; n < 360; ++n) {
        if (n % 37 == 0) for (auto& x : image) x = trial();
        else if (n % 5) for (int bit = 0; bit < 20; ++bit) {
          image[trial() % kFrameBytes] ^= uint8_t(1u << (trial() % 8));
        }
        const bool settle = trial() % 11 == 0;
        const bool a = fast.plan(image, settle);
        const bool b = ref.step(image, mode, blackLimit, whiteLimit, settle);
        assert(a == b);
        assert(!memcmp(fast.black, ref.black, kFrameBytes));
        assert(!memcmp(fast.white, ref.white, kFrameBytes));
        for (size_t p = 0; p < kPixels; ++p) assert(fast.netAt(p) == ref.net[p]);
        if (trial() % 7 == 0) {
          assert(fast.plan(image, settle) == a);
          assert(!memcmp(fast.black, ref.black, kFrameBytes));
          assert(!memcmp(fast.white, ref.white, kFrameBytes));
          for (size_t p = 0; p < kPixels; ++p) assert(fast.netAt(p) == ref.net[p]);
        }
        if (a) { fast.commit(image, settle); ref.commit(image, mode, settle); }
        else if (!settle) { fast.accept(image); memcpy(ref.target, image, kFrameBytes); }
        for (size_t p = 0; p < kPixels; ++p) assert(fast.netAt(p) == ref.net[p]);
      }
    }
  }
  // Mirror exactly the partial upload algorithm, including the first clear.
  MaskRows previous{0, kHeight - 1};
  memset(controllerBlack, 0xa5, sizeof(controllerBlack));
  memset(controllerWhite, 0x5a, sizeof(controllerWhite));
  for (int n = 0; n < 50; ++n) {
    memset(fast.black, 0, kFrameBytes); memset(fast.white, 0, kFrameBytes);
    if (n % 4 != 0) {
      fast.black[(n % 2 ? 295 : 2) * 16] = 0x80;
      fast.white[(n * 17 % 296) * 16 + 15] = 1;
    }
    const MaskRows current = maskRows(fast.black, fast.white);
    const MaskRows upload = uploadRows(current, previous);
    const size_t offset = upload.first * 16, size = (upload.last - upload.first + 1) * 16;
    memcpy(controllerBlack + offset, fast.black + offset, size);
    memcpy(controllerWhite + offset, fast.white + offset, size);
    assert(!memcmp(controllerBlack, fast.black, kFrameBytes));
    assert(!memcmp(controllerWhite, fast.white, kFrameBytes));
    previous = current;
  }
  // Static UI must never receive an intentional drive while another region
  // moves. This proves software masks, NOT electrical/optical HOLD neutrality.
  memset(image, 0xff, sizeof(image));
  image[10 * 16 + 2] = 0; // A static black reference in an untouched UI region.
  fast.reset(DriveMode::Budget, image); fast.setPulses(7, 7);
  previous = {0, kHeight - 1};
  for (int n = 0; n < 500; ++n) {
    memset(image, 0xff, sizeof(image)); image[10 * 16 + 2] = 0;
    image[(200 + n % 80) * 16 + 7] = 0;
    const bool maintenance = n == 250;
    bool driven = fast.plan(image, maintenance);
    assert(fast.black[10 * 16 + 2] == 0 && fast.white[10 * 16 + 2] == 0);
    assert(fast.black[0] == 0 && fast.white[0] == 0);
    if (driven) fast.commit(image, maintenance);
    else if (!maintenance) fast.accept(image);
    for (int b = 0; b < 8; ++b) {
      assert(fast.netAt((10 * 16 + 2) * 8 + b) == 0);
      assert(fast.netAt(b) == 0);
    }
  }
  puts("PASS: randomized equivalence, uncommitted replans, both-mode maintenance, asymmetric budgets, stale RAM, 500 static-UI scans");
}
