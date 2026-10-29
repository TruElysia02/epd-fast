#include "planner.h"
#include <cassert>
#include <cstdio>

static Planner planner;
static uint8_t image[kFrameBytes];

static void white() { memset(image, 0xff, sizeof(image)); }
static void blackFirst() { image[0] &= 0x7f; }
static void commitOne() {
  assert(planner.plan(image));
  for (size_t i = 0; i < kFrameBytes; ++i) assert(!(planner.black[i] & planner.white[i]));
  planner.commit(image);
}

int main() {
  static_assert(kFrameBytes == 4736);
  static_assert(sizeof(Planner) < 136000);
  // A still reference is never driven in budget mode.
  white(); planner.reset(DriveMode::Budget, image);
  assert(!planner.plan(image));
  blackFirst();
  // Planning or a failed transfer must not change delivered pulse history.
  assert(planner.plan(image)); assert(planner.netAt(0) == 0);
  assert(planner.plan(image)); assert(planner.netAt(0) == 0);
  for (int i = 0; i < 7; ++i) commitOne();
  assert(planner.netAt(0) == 7);
  assert(!planner.plan(image));
  white();
  for (int i = 0; i < 7; ++i) commitOne();
  assert(planner.netAt(0) == 0);
  assert(!planner.plan(image));

  // Interrupted black pulse followed by a full white budget leaves a debt.
  planner.reset(DriveMode::Budget, image);
  blackFirst(); commitOne();
  white(); for (int i = 0; i < 7; ++i) commitOne();
  assert(planner.netAt(0) == -6);
  assert(!planner.plan(image, true)); // White target: no opposite-color repayment.
  blackFirst(); assert(planner.plan(image, true));
  assert(planner.black[0] & 0x80);
  planner.commit(image, true); assert(planner.netAt(0) == -5);

  // Net mode bounds delivered imbalance, including an interrupted reversal.
  white(); planner.reset(DriveMode::Net, image);
  for (int i = 0; i < 7; ++i) commitOne();
  assert(planner.netAt(0) == -7); assert(!planner.plan(image));
  blackFirst(); commitOne(); assert(planner.netAt(0) == -6);
  white(); commitOne(); assert(planner.netAt(0) == -7);
  assert(!planner.plan(image));
  // Moving from one saturated bound to the other can take fourteen scans.
  blackFirst(); for (int i = 0; i < 14; ++i) commitOne();
  assert(planner.netAt(0) == 7); assert(!planner.plan(image));

  // Last pixel is on native row 295: it must survive the 200->296 adaptation.
  white(); planner.reset(DriveMode::Budget, image);
  image[kFrameBytes - 1] &= 0xfe;
  commitOne(); assert(planner.netAt(kPixels - 1) == 1);
  assert(planner.black[kFrameBytes - 1] == 1);

  // Asymmetric repeated transitions must not wrap signed 8-bit history.
  white(); planner.reset(DriveMode::Budget, image);
  for (int round = 0; round < 100; ++round) {
    blackFirst(); commitOne();
    white(); for (int i = 0; i < 7; ++i) commitOne();
  }
  assert(planner.netAt(0) == -600);
  puts("PASS: budgets, interrupted reversals, net bounds, maintenance, last row, history width");
}
