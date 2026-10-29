#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <esp_timer.h>
#include <algorithm>
#include "panel.h"
#include "planner.h"
#include "apple_video.h"

#if EPD_ENABLE_VIDEO
extern const uint8_t appleStart[] asm("_binary_assets_badapple_bin_start");
extern const uint8_t appleEnd[] asm("_binary_assets_badapple_bin_end");
#endif

namespace {
Panel panel;
Planner planner;
AppleVideo apple;
uint32_t appleLimitMs = 0;
bool appleReady = false;
GFXcanvas1 canvas(kWidth, kHeight);
uint8_t frame[kFrameBytes];
uint8_t pendingFrame[kFrameBytes];
DriveMode mode = DriveMode::Budget;
enum class Pattern { Blocks, Line, Reverse, Clock, Apple };
Pattern pattern = Pattern::Blocks;
bool running = false, faulted = false, holdPending = false;
bool pending = false, pendingDriven = false, pendingMaintenance = false;
bool framePrepared = false, frozen = false;
uint8_t blackPulses = 7, whitePulses = 7;
uint16_t speed = 24;
uint16_t targetFps = 30;
uint32_t scans = 0;
int64_t epochUs = 0, nextUs = 0;
int64_t lastCleanUs = 0, lastSettleUs = 0;
uint32_t lastBusyUs = 20000;
int64_t diagnosticOffUs = 0;
constexpr uint32_t kFullGuardMs = 3000; // Isolate full->fast transition during diagnosis.
char input[64];
size_t inputLength = 0;
struct {
  uint32_t scans = 0, plans = 0, overruns = 0;
  uint64_t planUs = 0, uploadUs = 0, busyUs = 0, renderUs = 0, commitUs = 0;
  int64_t sinceUs = 0;
} stats;

const char* modeName() { return mode == DriveMode::Budget ? "budget" : "net"; }
const char* patternName() {
  switch (pattern) {
    case Pattern::Blocks: return "blocks";
    case Pattern::Line: return "line";
    case Pattern::Reverse: return "reverse";
    case Pattern::Clock: return "clock";
    case Pattern::Apple: return "badapple";
  }
  return "unknown";
}

void fault() {
  running = false;
  faulted = true;
  // Do not pretend the software's charge model is still synchronized.
  Serial.printf("STOPPED: %s. Reset the board before continuing.\n", panel.error());
}

void help() {
  Serial.println("fast29 v6 / SSD1680 T94 / XIAO ESP32-C6 / 296x128 landscape");
  Serial.println("budget | net : full clean, reset model, run selected strategy");
  Serial.println("blocks | line | reverse | clock : change test after full clean");
  Serial.println("fps N : 1..60; fps 0 : uncapped (scan rate, NOT optical settling)");
  Serial.println("spi N : 4, 10, 20 MHz; stops and returns to full reference");
  Serial.println("pulse B W : 1..32 black/white pulses, full clean before applying");
  Serial.println("speed N : 1..120 pixels/sec; default 24");
  Serial.println("end fast | end normal : compare LUT ending 0x02 / 0x22 after clean");
  Serial.println("window full | window partial : compare full vs cropped RAM writes after clean");
  Serial.println("prepare : full refresh, wait 3s, enable fast power without scanning; auto-off after 8s");
  Serial.println("hold N : 1..200 zero-mask scans after prepare, then analog off; no full refresh");
  Serial.println("settle : freeze target, finish its drive budgets, HOLD, power off");
  Serial.println("clean : full clean and continue; stop : full clean and power off");
  Serial.println("status | help");
  Serial.println("apple : play complete silent Bad Apple; apple N : first N seconds; stop/settle also work");
}

void status() {
  Serial.printf("mode=%s pattern=%s running=%d fault=%d target_fps=%u spi_mhz=%lu "
                "scans_since_clean=%lu free_heap=%lu planner_bytes=%u "
                "end=0x%02x pulses=%u/%u speed=%u frozen=%d window=%s analog=%d guard_ms=%lu fast_hw_reset=1 version=6\n",
                modeName(), patternName(), running, faulted, targetFps,
                (unsigned long)(panel.clock() / 1000000), (unsigned long)scans,
                (unsigned long)ESP.getFreeHeap(), unsigned(sizeof(planner)),
                panel.endOption(), blackPulses, whitePulses, speed, frozen,
                panel.partialUpload() ? "partial" : "full", panel.analogOn(),
                (unsigned long)kFullGuardMs);
}

void render(int64_t now) {
  const int64_t begin = esp_timer_get_time();
  const uint32_t elapsedMs = (now - epochUs) / 1000;
  if (pattern == Pattern::Apple) {
    if (elapsedMs >= appleLimitMs) {
      frozen = true; framePrepared = true;
      Serial.println("APPLE end; settling last target");
    } else if (!apple.frame(uint64_t(elapsedMs) * apple.fps() / 1000, frame)) {
      Serial.println("FAULT video frame decode"); faulted = true; running = false;
    }
    stats.renderUs += esp_timer_get_time() - begin;
    return;
  }
  canvas.fillScreen(0);  // Canvas 1 = ink. Wire format is inverted below.
  canvas.setTextColor(1);
  canvas.setTextSize(1);
  canvas.setCursor(5, 4);
  canvas.printf("T94  %s  %s  %uHz", modeName(), patternName(), targetFps);
  canvas.drawLine(0, 16, 295, 16, 1);
  // Static references make background graying and contrast changes visible.
  canvas.fillRect(4, 24, 12, 80, 1);
  canvas.drawRect(20, 24, 12, 80, 1);
  const uint32_t triangle = (uint64_t(elapsedMs) * speed / 1000) % 480;
  const int x = 38 + int(triangle <= 240 ? triangle : 480 - triangle);
  switch (pattern) {
    case Pattern::Blocks:
      canvas.fillRect(x, 32, 16, 28, 1);
      canvas.fillCircle(315 - x, 86, 9, 1);
      break;
    case Pattern::Line:
      canvas.drawFastVLine(x, 24, 78, 1);
      canvas.drawFastVLine(315 - x, 24, 78, 1);
      break;
    case Pattern::Reverse:
      // Same patch: 80 ms black, then 320 ms white. Exercises interrupted
      // transitions and asymmetric pulse histories; not a durability test.
      if (elapsedMs % 400 < 80) canvas.fillRect(100, 36, 64, 56, 1);
      canvas.setCursor(180, 50); canvas.print("80ms ink");
      canvas.setCursor(180, 64); canvas.print("320ms white");
      break;
    case Pattern::Clock:
      canvas.setTextSize(3); canvas.setCursor(45, 47);
      canvas.printf("%02lu:%02lu.%02lu", (unsigned long)(elapsedMs / 60000 % 100),
                    (unsigned long)(elapsedMs / 1000 % 60),
                    (unsigned long)(elapsedMs / 10 % 100));
      canvas.setTextSize(1);
      break;
    case Pattern::Apple: break; // Native bitmap was decoded above.
  }
  canvas.drawLine(0, 110, 295, 110, 1);
  canvas.setCursor(5, 117); canvas.print("USB: budget / net / stop / help");
  for (size_t i = 0; i < kFrameBytes; ++i) frame[i] = ~canvas.getBuffer()[i];
  stats.renderUs += esp_timer_get_time() - begin;
}

void resetStats() {
  stats = {};
  stats.sinceUs = esp_timer_get_time();
}

bool finishPending() {
  if (!pending) return true;
  ScanTiming timing;
  if (!panel.finishScan(timing)) { fault(); return false; }
  const int64_t begin = esp_timer_get_time();
  if (pendingDriven) planner.commit(pendingFrame, pendingMaintenance);
  else if (!pendingMaintenance) planner.accept(pendingFrame);
  stats.commitUs += esp_timer_get_time() - begin;
  ++scans; ++stats.scans;
  stats.uploadUs += timing.uploadUs; stats.busyUs += timing.busyUs;
  lastBusyUs = timing.busyUs;
  holdPending = pendingDriven;
  pending = false;
  return true;
}

bool clean(bool resume) {
  running = false;
  diagnosticOffUs = 0;
  if (!finishPending()) return false;
  const int64_t start = esp_timer_get_time();
  if (!panel.full(frame)) { fault(); return false; }
  if (resume) {
    Serial.printf("PHASE full_busy_finished; guard_ms=%lu; fast_not_loaded\n", (unsigned long)kFullGuardMs);
    delay(kFullGuardMs);
  }
  planner.reset(mode, frame);
  planner.setPulses(blackPulses, whitePulses);
  scans = 0; holdPending = false;
  framePrepared = false;
  if (resume && !panel.prepareFast()) { fault(); return false; }
  running = resume;
  nextUs = esp_timer_get_time();
  lastCleanUs = lastSettleUs = nextUs;
  resetStats();
  if (resume && pattern == Pattern::Apple) epochUs += esp_timer_get_time() - start;
  Serial.printf("CLEAN full_and_prepare_ms=%lu running=%d\n",
                (unsigned long)((esp_timer_get_time() - start) / 1000), running);
  return true;
}

void start(DriveMode selected) {
  mode = selected;
  if (!finishPending()) return;
  frozen = false;
  epochUs = esp_timer_get_time();
  render(epochUs);
  if (faulted) return;
  clean(true);
  epochUs = esp_timer_get_time();
  status();
}

void command(char* text) {
  if (!strcmp(text, "help")) { help(); return; }
  if (!strcmp(text, "status")) { status(); return; }
  if (faulted) { Serial.println("FAULT latched; reset required."); return; }
  if (!strcmp(text, "budget")) { start(DriveMode::Budget); return; }
  if (!strcmp(text, "net")) { start(DriveMode::Net); return; }
  if (!strcmp(text, "apple") || !strncmp(text, "apple ", 6)) {
    if (!appleReady) { Serial.println("Video unavailable; build xiao_esp32c6_video with a valid asset. Playback refused."); return; }
    unsigned seconds = 0;
    char extra;
    if (strcmp(text, "apple") &&
        (sscanf(text, "apple %u %c", &seconds, &extra) != 1 || seconds < 1 || seconds > 233)) {
      Serial.println("Use apple or apple N (1..233 seconds)."); return;
    }
    appleLimitMs = seconds ? std::min<uint32_t>(seconds * 1000u, apple.durationMs()) : apple.durationMs();
    pattern = Pattern::Apple; targetFps = 0;
    Serial.printf("APPLE content_fps=%u duration_ms=%lu audio=off\n", apple.fps(), (unsigned long)appleLimitMs);
    start(DriveMode::Budget); return;
  }
  if (!strcmp(text, "stop")) { clean(false); return; }
  if (!strcmp(text, "clean")) { clean(running); return; }
  if (!strcmp(text, "prepare")) {
    if (!clean(false)) return;
    Serial.println("PHASE full_finished; waiting_3000ms; no_fast_LUT");
    delay(kFullGuardMs);
    if (!panel.prepareFast()) { fault(); return; }
    diagnosticOffUs = esp_timer_get_time() + 8000000;
    Serial.println("PHASE fast_prepared; pixel_scans=0; auto_off_in_8s");
    status(); return;
  }
  if (!strcmp(text, "settle")) {
    if (!running) { Serial.println("Already idle; no unfinished drive."); return; }
    if (!finishPending()) return;
    frozen = true; framePrepared = true;
    Serial.println("SETTLE completing current target, then HOLD and analog off");
    return;
  }
  if (!strcmp(text, "end fast") || !strcmp(text, "end normal")) {
    const bool resume = running;
    if (!clean(false)) return;
    panel.setEndOption(!strcmp(text, "end fast") ? 0x02 : 0x22);
    if (resume && !clean(true)) return;
    status(); return;
  }
  if (!strcmp(text, "window full") || !strcmp(text, "window partial")) {
    const bool resume = running;
    if (!clean(false)) return;
    panel.setPartialUpload(!strcmp(text, "window partial"));
    if (resume && !clean(true)) return;
    status(); return;
  }
  bool newPattern = true;
  if (!strcmp(text, "blocks")) pattern = Pattern::Blocks;
  else if (!strcmp(text, "line")) pattern = Pattern::Line;
  else if (!strcmp(text, "reverse")) pattern = Pattern::Reverse;
  else if (!strcmp(text, "clock")) pattern = Pattern::Clock;
  else newPattern = false;
  if (newPattern) { start(mode); return; }
  unsigned value;
  unsigned other;
  char extra;
  if (sscanf(text, "hold %u %c", &value, &extra) == 1 && value >= 1 && value <= 200) {
    if (running || !diagnosticOffUs || !panel.analogOn()) {
      Serial.println("Send prepare first; hold is only allowed in the idle diagnostic session."); return;
    }
    diagnosticOffUs = 0;
    memset(planner.black, 0, kFrameBytes); memset(planner.white, 0, kFrameBytes);
    Serial.printf("PHASE zero_masks; scans=%u; all_pixels_HOLD\n", value);
    for (unsigned i = 0; i < value; ++i) {
      ScanTiming timing;
      if (!panel.scan(planner.black, planner.white, timing)) { fault(); return; }
      ++scans;
    }
    if (!panel.powerOff()) { fault(); return; }
    Serial.println("PHASE zero_masks_done; analog_off; image_retained; no_full_refresh");
    status(); return;
  }
  if (sscanf(text, "pulse %u %u %c", &value, &other, &extra) == 2 &&
      value >= 1 && value <= 32 && other >= 1 && other <= 32) {
    const bool resume = running;
    if (!finishPending()) return;
    blackPulses = value; whitePulses = other;
    clean(resume); status(); return;
  }
  if (sscanf(text, "speed %u %c", &value, &extra) == 1 && value >= 1 && value <= 120) {
    if (!finishPending()) return;
    const bool resume = running;
    speed = value; frozen = false;
    epochUs = esp_timer_get_time(); render(epochUs);
    clean(resume); epochUs = esp_timer_get_time(); status(); return;
  }
  if (sscanf(text, "fps %u %c", &value, &extra) == 1 && value <= 60) {
    targetFps = value; nextUs = esp_timer_get_time(); resetStats(); status(); return;
  }
  if (sscanf(text, "spi %u %c", &value, &extra) == 1 &&
      (value == 4 || value == 10 || value == 20)) {
    if (!clean(false)) return;
    if (!panel.setClock(value * 1000000)) { fault(); return; }
    status(); return;
  }
  Serial.println("Unknown command. Send help.");
}

void pollCommands() {
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\r' || c == '\n') {
      if (inputLength) { input[inputLength] = 0; command(input); inputLength = 0; }
    } else if (inputLength < sizeof(input) - 1) input[inputLength++] = c;
    else { inputLength = 0; Serial.println("Command too long; send help."); }
  }
}

void reportStats(int64_t now) {
  const int64_t elapsed = now - stats.sinceUs;
  if (elapsed < 2000000) return;
  Serial.printf("MEASURE mode=%s pattern=%s scan_hz=%.2f plan_us=%lu "
                "upload_us=%lu busy_us=%lu render_us=%lu commit_us=%lu "
                "overruns=%lu scans=%lu heap=%lu\n",
                modeName(), patternName(), stats.scans * 1000000.0 / elapsed,
                (unsigned long)(stats.plans ? stats.planUs / stats.plans : 0),
                (unsigned long)(stats.scans ? stats.uploadUs / stats.scans : 0),
                (unsigned long)(stats.scans ? stats.busyUs / stats.scans : 0),
                (unsigned long)(stats.plans ? stats.renderUs / stats.plans : 0),
                (unsigned long)(stats.scans ? stats.commitUs / stats.scans : 0),
                (unsigned long)stats.overruns, (unsigned long)scans,
                (unsigned long)ESP.getFreeHeap());
  resetStats();
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1200);
  canvas.setRotation(3);  // Same landscape orientation as Tipsy Kernel Lite.
  #if EPD_ENABLE_VIDEO
  appleReady = apple.begin(appleStart, size_t(appleEnd - appleStart));
  Serial.printf("APPLE asset_ready=%d bytes=%u frames=%lu fps=%u\n", appleReady,
                unsigned(appleEnd - appleStart), (unsigned long)apple.count(), apple.fps());
  #else
  Serial.println("Video disabled; see assets/README.md to build with your own video.");
  #endif
  if (!canvas.getBuffer()) { Serial.println("FAULT canvas allocation"); faulted = true; return; }
  if (!panel.begin()) { fault(); return; }
  epochUs = esp_timer_get_time();
  render(epochUs);
  clean(false);  // Boot into a standard full-refresh reference, fast drive idle.
  help();
  status();
}

void loop() {
  pollCommands();
  if (diagnosticOffUs && esp_timer_get_time() >= diagnosticOffUs) {
    diagnosticOffUs = 0;
    if (!panel.powerOff()) { fault(); return; }
    Serial.println("PHASE diagnostic_auto_off; pixel_scans=0; image_retained");
    status();
  }
  if (!running || faulted) { delay(1); return; }
  if (pending) {
    // Canvas work for the next target overlaps the controller's scan.
    // The current target and masks stay intact until completion is confirmed.
    if (!framePrepared && !frozen) {
      const int64_t predicted = std::max(nextUs, esp_timer_get_time() + lastBusyUs);
      render(predicted); framePrepared = true;
      if (faulted) return;
    }
    if (!panel.scanReady()) { delay(1); return; }
    if (!finishPending()) return;
    reportStats(esp_timer_get_time());
  }
  const int64_t now = esp_timer_get_time();
  if (now < nextUs) { delay(1); return; }
  if (scans >= kCleanScans || now - lastCleanUs >= 45000000) { clean(true); return; }
  if (!framePrepared && !frozen) render(now);
  if (faulted) return;
  const bool maintenance = !frozen && mode == DriveMode::Budget && now - lastSettleUs >= 20000000;
  if (maintenance) lastSettleUs = now;
  const int64_t planStart = esp_timer_get_time();
  const bool driven = planner.plan(frame, maintenance);
  stats.planUs += esp_timer_get_time() - planStart;
  ++stats.plans;
  if (driven || holdPending) {
    memcpy(pendingFrame, frame, kFrameBytes);
    if (!panel.startScan(planner.black, planner.white)) { fault(); return; }
    pending = true; pendingDriven = driven; pendingMaintenance = maintenance;
  } else {
    if (!maintenance) planner.accept(frame);
    if (frozen) {
      if (!panel.powerOff()) { fault(); return; }
      running = false;
      Serial.println("SETTLED running=0 analog_off=1 (no full refresh)");
      status(); return;
    }
  }
  framePrepared = frozen;
  const int64_t end = esp_timer_get_time();
  if (targetFps) {
    const int64_t period = 1000000 / targetFps;
    if (end > nextUs + period) {
      // A slower panel should run at its available rate, not accidentally
      // halve its rate by dropping every second fixed scheduling slot.
      ++stats.overruns;
    }
    nextUs = now + period;
  } else nextUs = end;
  reportStats(end);
  // Keep USB command handling and the RTOS idle task responsive, also uncapped.
  delay(1);
}
