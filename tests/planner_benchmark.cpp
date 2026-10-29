#ifdef PLANNER_BASELINE
#include "../.scratch/planner-optimization/planner-original.h"
#else
#include "planner.h"
#endif
#include <array>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

// Desktop comparison only: this measures CPU work, not the panel scan period.
static Planner planner;
static volatile uint64_t result = 0;
using Frame = std::array<uint8_t, kFrameBytes>;

static void run(const char* name, DriveMode mode, unsigned changes, bool maintenance) {
  std::mt19937 random(29);
  std::vector<Frame> frames(128);
  Frame image;
  image.fill(0xff);
  for (auto& frame : frames) {
    for (unsigned j = 0; j < changes; ++j) {
      const unsigned offset = random() % kFrameBytes;
      image[offset] ^= uint8_t(1u << (random() % 8));
    }
    frame = image;
  }
  Frame initial;
  initial.fill(0xff);
  constexpr unsigned count = 2048;
  planner.reset(mode, initial.data());
  planner.setPulses(3, 9);
  const auto begin = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < count; ++i) {
    const Frame& frame = frames[i % frames.size()];
    const bool settle = maintenance && i % 13 == 0;
    if (planner.plan(frame.data(), settle)) planner.commit(frame.data(), settle);
    else if (!settle) planner.accept(frame.data());
    result += planner.black[i % kFrameBytes] + planner.white[i % kFrameBytes] +
              planner.netAt(i % kPixels);
  }
  const double us = std::chrono::duration<double, std::micro>(
      std::chrono::steady_clock::now() - begin).count() / count;
  std::printf("%-22s %8.2f us/step\n", name, us);
}

int main() {
  run("budget idle", DriveMode::Budget, 0, false);
  run("budget sparse", DriveMode::Budget, 12, false);
  run("budget dense", DriveMode::Budget, kPixels, false);
  run("budget dense maintain", DriveMode::Budget, kPixels, true);
  run("net idle", DriveMode::Net, 0, false);
  run("net sparse", DriveMode::Net, 12, false);
  run("net dense", DriveMode::Net, kPixels, false);
  run("net dense maintain", DriveMode::Net, kPixels, true);
  std::printf("result=%llu planner_bytes=%u\n", static_cast<unsigned long long>(result),
              unsigned(sizeof(planner)));
}
