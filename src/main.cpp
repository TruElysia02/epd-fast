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
Planner plannerStorage[2];
Planner* currentPlanner = &plannerStorage[0];
Planner* shadowPlanner = &plannerStorage[1];
Planner& planner() { return *currentPlanner; }
bool pipelineEnabled = false, pipelinePrepared = false, pipelineReady = false;
bool pipelineDriven = false, pipelineMaintenance = false;
uint32_t pipelineCommitUs = 0, pipelinePlanUs = 0;
AppleVideo apple;
uint32_t appleLimitMs = 0;
bool appleReady = false;
bool appleLoop = false;
bool irqWait = false;
bool maintenanceEnabled = true;
uint32_t videoFrame = 0, pendingVideoFrame = 0, presentedVideoFrame = UINT32_MAX;
GFXcanvas1 canvas(kWidth, kHeight);
uint8_t frame[kFrameBytes];
uint8_t pendingFrame[kFrameBytes];
DriveMode mode = DriveMode::Budget;
enum class Pattern { Blocks, Line, Reverse, Clock, Apple };
Pattern pattern = Pattern::Blocks;
bool running = false, faulted = false, holdPending = false;
bool pending = false, pendingDriven = false, pendingMaintenance = false;
uint32_t pendingMaintenancePixels = 0;
bool framePrepared = false, frozen = false;
uint8_t blackPulses = 7, whitePulses = 7;
uint16_t speed = 24;
uint16_t targetFps = 30;
uint32_t scans = 0;
int64_t epochUs = 0, nextUs = 0;
int64_t lastCleanUs = 0, lastSettleUs = 0;
uint32_t lastBusyUs = 20000;
int64_t diagnosticOffUs = 0;
uint32_t fullGuardMs = 3000; // Diagnostic guard; configurable after a full-refresh stop.
char input[64];
size_t inputLength = 0;
struct {
  uint32_t scans = 0, plans = 0, overruns = 0;
  uint32_t contentFrames = 0, skippedFrames = 0;
  uint32_t pipelineStages = 0, pipelineUses = 0;
  uint32_t stableTailPulses = 0;
  uint32_t maintenanceScans = 0, maintenancePixels = 0;
  uint64_t pipelineCopyUs = 0;
  uint64_t planUs = 0, uploadUs = 0, busyUs = 0, renderUs = 0, commitUs = 0;
  int64_t sinceUs = 0;
} stats;
struct {
  bool active = false, naturalEnd = false;
  uint32_t sourceFrames = 0, acceptedIndices = 0, skippedIndices = 0, scans = 0;
  uint32_t stableTailPulses = 0;
  uint32_t maintenanceScans = 0, maintenancePixels = 0;
  int64_t wallStartUs = 0, cleanWallUs = 0;
} appleResult;

enum class DriftPhase { None, Baseline, Sample };
struct {
  DriftPhase phase = DriftPhase::None;
  bool scanning = false, savedPaired = false, savedWeak = false;
  uint16_t scans = 0;
  int64_t phaseStartUs = 0, nextScanUs = 0;
  uint64_t busyUs = 0;
} drift;
constexpr unsigned kDriftPulses = 13, kDriftScans = 1500;
constexpr int64_t kDriftPeriodUs = 12500, kDriftSampleUs = kDriftScans * kDriftPeriodUs;
static_assert(2 * kDriftPulses + 1 + kDriftScans < kCleanScans,
              "Drift probe must fit between full cleans");

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
  drift.phase = DriftPhase::None;
  appleResult.active = false;
  pipelinePrepared = pipelineReady = false;
  running = false;
  faulted = true;
  // Do not pretend the software's charge model is still synchronized.
  Serial.printf("STOPPED: %s. Reset the board before continuing.\n", panel.error());
}

void help() {
  Serial.println("fast29 v10 / SSD1680 T94 / XIAO ESP32-C6 / 296x128 landscape");
  Serial.println("budget | net : full clean, reset model, run selected strategy");
  Serial.println("blocks | line | reverse | clock : change test after full clean");
  Serial.println("fps N : 1..120 scans/s; fps 0 : uncapped (NOT optical settling)");
  Serial.println("spi N : 4, 10, 20 MHz; stops and returns to full reference");
  Serial.println("pulse B W : 1..32 black/white pulses, full clean before applying");
  Serial.println("speed N : 1..120 pixels/sec; default 24");
  Serial.println("end fast | end normal : compare LUT ending 0x02 / 0x22 after clean");
  Serial.println("window full | window partial : compare full vs cropped RAM writes after clean");
  Serial.println("prepare : full refresh, configured guard, enable fast power without scanning; auto-off after 8s");
  Serial.println("hold N : 1..200 zero-mask scans after prepare, then analog off; no full refresh");
  Serial.println("settle : freeze target, finish its drive budgets, HOLD, power off");
  Serial.println("clean : full clean and continue; stop : full clean and power off");
  Serial.println("status | help");
  Serial.println("apple : play complete silent Bad Apple; apple N : first N seconds; stop/settle also work");
  Serial.println("apple loop [N] : repeat the complete video or its first N seconds until stop/settle");
  Serial.println("fr N : waveform frequency selector 0..7; full clean and stop before applying");
  Serial.println("scan full | scan FIRST LAST : gate scan rows, 16..296 lines; stops first");
  Serial.println("reuse on | reuse off : reuse identical controller masks; stops first");
  Serial.println("wait irq | wait poll : BUSY falling-edge wakeup or 1ms polling");
  Serial.println("verify N : decode video frame N while idle and report its FNV-1a checksum");
  Serial.println("decodebench N : decode 1..1000 sequential frames while idle/analog off; no panel scans");
  Serial.println("guard N : diagnostic delay after full refresh, 0..3000ms; stops first");
  Serial.println("pipeline on | pipeline off : predict next scan while BUSY; stops first, default off");
  Serial.println("paired on | paired off : follow each drive with one HOLD phase; stops first");
  Serial.println("history off | history 100|150|200 : experimental return-dose estimate; stops first");
  Serial.println("weak N | weak off : experimental black tail, 1..4 strong doses replaced by twice as many weak scans");
  Serial.println("stable off | stable 2|4 : once-per-target extra strong pulses after ~192ms stable; budget only");
  Serial.println("maintenance on | maintenance off : 20s net-count correction; stops first, default on; full cleans remain");
  Serial.println("weakprobe N [weak|sameA|sameB] : 1..11 pulses on HOLD/A/test/B patches, then power off");
  Serial.println("drift hold | drift idle : FR5 static reference/round-trip probe; 13+13 pulses, then 1500 HOLD scans or equal power-off wait");
}

void status() {
  Serial.printf("mode=%s pattern=%s running=%d fault=%d target_fps=%u spi_mhz=%lu "
                "end=0x%02x pulses=%u/%u window=%s analog=%d guard_ms=%lu video_loop=%d "
                "fr=%u gate_first=%u gate_last=%u reuse=%d wait=%s video_ready=%d content_fps=%u "
                "pipeline=%d paired=%d history=%u weak_tail=%u stable_tail=%u stable_ms=192 maintenance=%d drift=%d version=10\n",
                modeName(), patternName(), running, faulted, targetFps,
                (unsigned long)(panel.clock() / 1000000), panel.endOption(), blackPulses, whitePulses,
                panel.partialUpload() ? "partial" : "full", panel.analogOn(),
                (unsigned long)fullGuardMs, appleLoop, panel.frameRate(), panel.scanFirst(), panel.scanLast(),
                panel.reuseMasks(), irqWait ? "irq" : "poll", appleReady, apple.fps(),
                pipelineEnabled, panel.pairedHold(), planner().reversalPercent(), planner().weakTail(),
                planner().stableTail(), maintenanceEnabled, drift.phase != DriftPhase::None);
  Serial.flush();
}

void render(int64_t now) {
  const int64_t begin = esp_timer_get_time();
  const uint32_t elapsedMs = (now - epochUs) / 1000;
  if (pattern == Pattern::Apple) {
    if (elapsedMs >= appleLimitMs) {
      videoFrame = std::min<uint32_t>(apple.count() - 1,
          uint64_t(appleLimitMs - 1) * apple.fps() / 1000);
      if (!apple.frame(videoFrame, frame)) {
        Serial.println("FAULT final video frame decode"); faulted = true; running = false; return;
      }
      appleResult.naturalEnd = true;
      frozen = true; framePrepared = true;
      Serial.println("APPLE end; settling last target");
    } else {
      // A periodic clean pauses the clock after a predicted target was decoded.
      // Keep that target rather than briefly going backwards after the pause.
      videoFrame = std::max<uint32_t>(videoFrame, uint64_t(elapsedMs) * apple.fps() / 1000);
      if (!apple.frame(videoFrame, frame)) {
        Serial.println("FAULT video frame decode"); faulted = true; running = false;
      }
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

void acceptVideoFrame(uint32_t index) {
  if (pattern != Pattern::Apple || presentedVideoFrame == index) return;
  if (presentedVideoFrame != UINT32_MAX && index > presentedVideoFrame + 1)
    stats.skippedFrames += index - presentedVideoFrame - 1;
  ++stats.contentFrames;
  if (appleResult.active) {
    if (presentedVideoFrame != UINT32_MAX && index > presentedVideoFrame + 1)
      appleResult.skippedIndices += index - presentedVideoFrame - 1;
    ++appleResult.acceptedIndices;
  }
  presentedVideoFrame = index;
}

void reportAppleResult() {
  if (!appleResult.active) return;
  const int64_t activeUs = esp_timer_get_time() - appleResult.wallStartUs - appleResult.cleanWallUs;
  Serial.printf("APPLE_RESULT content_fps=%u total_source_frames=%lu accepted_indices=%lu "
                "skipped_indices=%lu scan_count=%lu elapsed_active_us=%llu clean_wall_us=%llu completed=%d "
                "stable_tail_pulses=%lu maintenance_scans=%lu maintenance_pixels=%lu\n",
                apple.fps(), (unsigned long)appleResult.sourceFrames,
                (unsigned long)appleResult.acceptedIndices, (unsigned long)appleResult.skippedIndices,
                (unsigned long)appleResult.scans, (unsigned long long)activeUs,
                (unsigned long long)appleResult.cleanWallUs, appleResult.naturalEnd,
                (unsigned long)appleResult.stableTailPulses,
                (unsigned long)appleResult.maintenanceScans, (unsigned long)appleResult.maintenancePixels);
  appleResult.active = false;
}

// Work only on a copy until the panel confirms the in-flight scan. Preview
// writes masks but leaves next-frame budgets untouched, so cancellation is safe.
void preparePipeline(int64_t predicted) {
  if (!pipelineEnabled || pipelinePrepared || pipelineReady) return;
  const int64_t copyStart = esp_timer_get_time();
  *shadowPlanner = planner();
  stats.pipelineCopyUs += esp_timer_get_time() - copyStart;
  ++stats.pipelineStages;
  const int64_t commitStart = esp_timer_get_time();
  if (pendingDriven) shadowPlanner->commit(pendingFrame, pendingMaintenance);
  else if (!pendingMaintenance) shadowPlanner->accept(pendingFrame);
  pipelineCommitUs = esp_timer_get_time() - commitStart;
  pipelineMaintenance = maintenanceEnabled && !frozen && mode == DriveMode::Budget &&
                        predicted - lastSettleUs >= 20000000;
  const int64_t planStart = esp_timer_get_time();
  // Use wall time for stability, not the future source-frame prediction.
  pipelineDriven = shadowPlanner->preview(frame, pipelineMaintenance,
      uint32_t(esp_timer_get_time() / 1000));
  pipelinePlanUs = esp_timer_get_time() - planStart;
  pipelinePrepared = true;
}

bool finishPending() {
  if (!pending) return true;
  ScanTiming timing;
  if (!panel.finishScan(timing)) { fault(); return false; }
  const int64_t begin = esp_timer_get_time();
  const uint32_t tailBefore = planner().stableTailPulsesDelivered();
  if (pipelinePrepared) {
    // The shadow's physical history contains exactly this successful scan.
    std::swap(currentPlanner, shadowPlanner);
    pipelinePrepared = false;
    pipelineReady = true;
    stats.commitUs += pipelineCommitUs;
  } else {
    if (pendingDriven) planner().commit(pendingFrame, pendingMaintenance);
    else if (!pendingMaintenance) planner().accept(pendingFrame);
    stats.commitUs += esp_timer_get_time() - begin;
  }
  if (!pendingMaintenance) acceptVideoFrame(pendingVideoFrame);
  ++scans; ++stats.scans;
  const uint32_t tailPulses = planner().stableTailPulsesDelivered() - tailBefore;
  stats.stableTailPulses += tailPulses;
  uint32_t maintenancePixels = 0;
  if (pendingMaintenance && pendingDriven) {
    // With a staged pipeline the current masks already belong to the next
    // preview. The scan's pixel count is captured before starting it below.
    maintenancePixels = pendingMaintenancePixels;
    ++stats.maintenanceScans;
    stats.maintenancePixels += maintenancePixels;
    Serial.printf("MAINTENANCE pixel_pulses=%lu\n", (unsigned long)maintenancePixels);
  }
  if (appleResult.active) {
    ++appleResult.scans;
    appleResult.stableTailPulses += tailPulses;
    if (pendingMaintenance && pendingDriven) {
      ++appleResult.maintenanceScans;
      appleResult.maintenancePixels += maintenancePixels;
    }
  }
  stats.uploadUs += timing.uploadUs; stats.busyUs += timing.busyUs;
  lastBusyUs = timing.busyUs;
  holdPending = pendingDriven;
  pending = false;
  return true;
}

bool clean(bool resume) {
  pipelinePrepared = pipelineReady = false;
  running = false;
  diagnosticOffUs = 0;
  if (!finishPending()) return false;
  const int64_t start = esp_timer_get_time();
  if (!panel.full(frame)) { fault(); return false; }
  if (resume) {
    Serial.printf("PHASE full_busy_finished; guard_ms=%lu; fast_not_loaded\n", (unsigned long)fullGuardMs);
    delay(fullGuardMs);
  }
  if (!planner().setPulses(blackPulses, whitePulses)) {
    faulted = true;
    Serial.println("FAULT planner pulse configuration rejected; board stopped");
    return false;
  }
  planner().reset(mode, frame);
  panel.setWeakState(planner().weakTail() != 0);
  panel.setWeakSource(3);
  scans = 0; holdPending = false;
  framePrepared = false;
  if (resume && !panel.prepareFast()) { fault(); return false; }
  running = resume;
  nextUs = esp_timer_get_time();
  lastCleanUs = lastSettleUs = nextUs;
  resetStats();
  const int64_t cleanUs = esp_timer_get_time() - start;
  if (appleResult.active) appleResult.cleanWallUs += cleanUs;
  if (resume && pattern == Pattern::Apple) epochUs += cleanUs;
  Serial.printf("CLEAN full_and_prepare_ms=%lu running=%d\n",
                (unsigned long)((esp_timer_get_time() - start) / 1000), running);
  return true;
}

void start(DriveMode selected) {
  if (selected == DriveMode::Net && (planner().weakTail() || planner().stableTail())) {
    Serial.println("Disable weak and stable tails before selecting net mode."); return;
  }
  mode = selected;
  if (!finishPending()) return;
  frozen = false;
  videoFrame = 0;
  presentedVideoFrame = UINT32_MAX;
  appleResult = {};
  epochUs = esp_timer_get_time();
  render(epochUs);
  if (faulted) return;
  if (pattern == Pattern::Apple) {
    appleResult.active = true;
    appleResult.sourceFrames = std::min<uint32_t>(apple.count(),
        uint64_t(appleLimitMs - 1) * apple.fps() / 1000 + 1);
    appleResult.wallStartUs = esp_timer_get_time();
  }
  if (!clean(true)) return;
  epochUs = esp_timer_get_time();
  if (pattern == Pattern::Apple) acceptVideoFrame(0); // Initial full-refresh image.
  status();
}

void weakProbe(unsigned pulses, uint8_t source) {
  appleResult.active = false;
  appleLoop = false;
  running = false;
  if (!finishPending()) return;
  const bool paired = panel.pairedHold();
  const bool weakState = panel.weakState();
  const uint8_t weakSource = panel.weakSource();
  canvas.fillScreen(0);
  canvas.setTextColor(1);
  canvas.setTextSize(1);
  canvas.setCursor(18, 8); canvas.print("Source-state probe");
  const char* labels[] = {"HOLD", "A", source == 1 ? "sameA" : source == 2 ? "sameB" : "weak", "B"};
  for (unsigned col = 0; col < 4; ++col) {
    canvas.setCursor(42 + col * 60, 28); canvas.print(labels[col]);
  }
  canvas.fillRect(30, 78, 242, 32, 1);
  for (unsigned col = 0; col < 4; ++col) {
    canvas.drawRect(38 + col * 60, 44, 36, 28, 1);
    canvas.drawRect(38 + col * 60, 80, 36, 28, 0);
  }
  for (size_t i = 0; i < kFrameBytes; ++i) frame[i] = ~canvas.getBuffer()[i];
  panel.setScanRows(0, kHeight - 1);
  if (!clean(false)) return;
  panel.setPairedHold(false);
  panel.setWeakState(true);
  panel.setWeakSource(source);
  if (!panel.prepareFast()) { fault(); return; }
  // Wire-state order is (0x26,0x24): 00,10,11,01. The probe bypasses the
  // pulse model; every later animation starts with a full refresh and reset.
  canvas.fillScreen(0);
  for (unsigned row : {46u, 82u}) {
    canvas.fillRect(100, row, 32, 24, 1);
    canvas.fillRect(160, row, 32, 24, 1);
  }
  memcpy(planner().black, canvas.getBuffer(), kFrameBytes);
  canvas.fillScreen(0);
  for (unsigned row : {46u, 82u}) {
    canvas.fillRect(160, row, 32, 24, 1);
    canvas.fillRect(220, row, 32, 24, 1);
  }
  memcpy(planner().white, canvas.getBuffer(), kFrameBytes);
  uint64_t busyUs = 0;
  for (unsigned n = 0; n < pulses; ++n) {
    ScanTiming timing;
    if (!panel.scan(planner().black, planner().white, timing)) { fault(); return; }
    busyUs += timing.busyUs;
  }
  memset(planner().black, 0, kFrameBytes);
  memset(planner().white, 0, kFrameBytes);
  ScanTiming ending;
  if (!panel.scan(planner().black, planner().white, ending) || !panel.powerOff()) { fault(); return; }
  panel.setWeakState(weakState);
  panel.setWeakSource(weakSource);
  panel.setPairedHold(paired);
  holdPending = framePrepared = false;
  Serial.printf("WEAKPROBE pulses=%u source=%u busy_us=%lu states=00,10,11,01 voltage_registers=unchanged analog_off=1\n",
                pulses, source, (unsigned long)(busyUs / pulses));
  status();
}

bool endDrift(bool canceled) {
  const int64_t sampleUs = drift.phase == DriftPhase::Sample ? esp_timer_get_time() - drift.phaseStartUs : 0;
  drift.phase = DriftPhase::None;
  if (!panel.powerOff()) { fault(); return false; }
  panel.setPairedHold(drift.savedPaired);
  panel.setWeakState(drift.savedWeak);
  holdPending = framePrepared = false;
  Serial.printf("DRIFT_RESULT mode=%s hold_scans=%u hold_busy_us=%llu sample_wall_us=%llu "
                "total_activations=%u canceled=%d analog_off=1\n",
                drift.scanning ? "hold" : "idle", drift.scans,
                (unsigned long long)drift.busyUs, (unsigned long long)sampleUs,
                2 * kDriftPulses + 1 + drift.scans, canceled);
  if (!canceled) status();
  return true;
}

void startDrift(bool scanning) {
  if (panel.frameRate() != 5 || panel.scanFirst() != 0 || panel.scanLast() != kHeight - 1) {
    Serial.println("Drift probe requires fr 5 and scan full."); return;
  }
  appleResult.active = false;
  appleLoop = false;
  running = false;
  if (!finishPending()) return;
  drift = {};
  drift.scanning = scanning;
  drift.savedPaired = panel.pairedHold();
  drift.savedWeak = panel.weakState();
  canvas.fillScreen(0);
  canvas.setTextSize(1); canvas.setTextColor(1);
  canvas.setCursor(28, 4); canvas.print("Static history: ");
  canvas.print(scanning ? "HOLD" : "IDLE");
  canvas.setCursor(66, 20); canvas.print("REF");
  canvas.setCursor(190, 20); canvas.print("RETURN");
  canvas.setCursor(12, 42); canvas.print("W");
  for (unsigned x : {42u, 178u}) canvas.drawRect(x, 32, 76, 32, 1);
  canvas.fillRect(0, 76, 296, 44, 1);
  canvas.setTextColor(0); canvas.setCursor(12, 94); canvas.print("B");
  for (unsigned x : {42u, 178u}) canvas.drawRect(x, 84, 76, 32, 0);
  for (size_t i = 0; i < kFrameBytes; ++i) frame[i] = ~canvas.getBuffer()[i];
  if (!clean(false)) return;
  panel.setPairedHold(false); panel.setWeakState(false);
  if (!panel.prepareFast()) { fault(); return; }
  // Only alternating 8px squares inside RETURN receive drive. Their neighbors
  // and both REF patches retain the same-color full-refresh reference.
  canvas.fillScreen(0);
  for (unsigned row = 0; row < 3; ++row) for (unsigned col = 0; col < 8; ++col)
    if ((row + col) % 2 == 0) canvas.fillRect(184 + col * 8, 36 + row * 8, 8, 8, 1);
  memcpy(planner().black, canvas.getBuffer(), kFrameBytes);
  canvas.fillScreen(0);
  for (unsigned row = 0; row < 3; ++row) for (unsigned col = 0; col < 8; ++col)
    if ((row + col) % 2 == 0) canvas.fillRect(184 + col * 8, 88 + row * 8, 8, 8, 1);
  memcpy(planner().white, canvas.getBuffer(), kFrameBytes);
  for (unsigned leg = 0; leg < 2; ++leg) {
    for (unsigned n = 0; n < kDriftPulses; ++n) {
      ScanTiming timing;
      if (!panel.scan(planner().black, planner().white, timing)) { fault(); return; }
      delay(1);
    }
    for (size_t i = 0; i < kFrameBytes; ++i) std::swap(planner().black[i], planner().white[i]);
  }
  memset(planner().black, 0, kFrameBytes); memset(planner().white, 0, kFrameBytes);
  ScanTiming ending;
  if (!panel.scan(planner().black, planner().white, ending)) { fault(); return; }
  drift.phase = DriftPhase::Baseline;
  drift.phaseStartUs = esp_timer_get_time();
  Serial.printf("DRIFT baseline_ms=3000 mode=%s roundtrip_pulses=%u expected_final=W_top,B_bottom\n",
                scanning ? "hold" : "idle", kDriftPulses);
  status();
}

void serviceDrift() {
  const int64_t now = esp_timer_get_time();
  if (drift.phase == DriftPhase::Baseline) {
    if (now - drift.phaseStartUs < 3000000) return;
    if (!drift.scanning && !panel.powerOff()) { fault(); return; }
    drift.phase = DriftPhase::Sample;
    drift.phaseStartUs = drift.nextScanUs = esp_timer_get_time();
    Serial.printf("DRIFT sample_ms=%lu mode=%s masks=zero\n", (unsigned long)(kDriftSampleUs / 1000),
                  drift.scanning ? "hold" : "idle");
    return;
  }
  if (drift.phase != DriftPhase::Sample) return;
  if (!drift.scanning) {
    if (now - drift.phaseStartUs >= kDriftSampleUs) endDrift(false);
    return;
  }
  if (now < drift.nextScanUs) return;
  // Synchronous scan finishes before returning to pollCommands, so canceling
  // never leaves a separate panel scan pending outside the normal pipeline.
  ScanTiming timing;
  if (!panel.scan(planner().black, planner().white, timing)) { fault(); return; }
  drift.busyUs += timing.busyUs;
  ++drift.scans;
  drift.nextScanUs = std::max(drift.phaseStartUs + drift.scans * kDriftPeriodUs, esp_timer_get_time());
  if (drift.scans % 500 == 0) Serial.printf("DRIFT checkpoint=%u elapsed_ms=%lu\n", drift.scans,
      (unsigned long)((esp_timer_get_time() - drift.phaseStartUs) / 1000));
  if (drift.scans == kDriftScans) endDrift(false);
}

void command(char* text) {
  if (!strcmp(text, "help")) { help(); return; }
  if (!strcmp(text, "status")) { status(); return; }
  if (faulted) { Serial.println("FAULT latched; reset required."); return; }
  if (drift.phase != DriftPhase::None && !endDrift(true)) return;
  // Any changing command may invalidate the pre-rendered target or its timing.
  pipelinePrepared = pipelineReady = false;
  if (!strcmp(text, "drift hold") || !strcmp(text, "drift idle")) {
    startDrift(!strcmp(text, "drift hold")); return;
  }
  if (!strcmp(text, "pipeline on") || !strcmp(text, "pipeline off")) {
    if (!clean(false)) return;
    pipelineEnabled = !strcmp(text, "pipeline on"); status(); return;
  }
  if (!strcmp(text, "maintenance on") || !strcmp(text, "maintenance off")) {
    if (!clean(false)) return;
    maintenanceEnabled = !strcmp(text, "maintenance on");
    status(); return;
  }
  if (!strcmp(text, "paired on") || !strcmp(text, "paired off")) {
    if (!clean(false)) return;
    panel.setPairedHold(!strcmp(text, "paired on")); status(); return;
  }
  if (!strcmp(text, "budget")) { start(DriveMode::Budget); return; }
  if (!strcmp(text, "net")) { start(DriveMode::Net); return; }
  if (!strcmp(text, "apple") || !strncmp(text, "apple ", 6)) {
    if (!appleReady) { Serial.println("Video unavailable; build xiao_esp32c6_video with a valid asset. Playback refused."); return; }
    unsigned seconds = 0;
    char extra;
    const bool repeat = !strcmp(text, "apple loop") || !strncmp(text, "apple loop ", 11);
    const char* secondsText = text + (repeat ? 10 : 5);
    if (*secondsText &&
        (sscanf(secondsText, " %u %c", &seconds, &extra) != 1 || seconds < 1 || seconds > 233)) {
      Serial.println("Use apple [N] or apple loop [N] (1..233 seconds)."); return;
    }
    appleLoop = repeat;
    appleLimitMs = seconds ? std::min<uint32_t>(seconds * 1000u, apple.durationMs()) : apple.durationMs();
    pattern = Pattern::Apple; targetFps = 0;
    Serial.printf("APPLE content_fps=%u duration_ms=%lu audio=off\n", apple.fps(), (unsigned long)appleLimitMs);
    start(DriveMode::Budget); return;
  }
  if (!strcmp(text, "stop")) { appleResult.active = false; appleLoop = false; clean(false); return; }
  if (!strcmp(text, "clean")) { clean(running); return; }
  if (!strcmp(text, "prepare")) {
    if (!clean(false)) return;
    Serial.printf("PHASE full_finished; guard_ms=%lu; no_fast_LUT\n", (unsigned long)fullGuardMs);
    delay(fullGuardMs);
    if (!panel.prepareFast()) { fault(); return; }
    diagnosticOffUs = esp_timer_get_time() + 8000000;
    Serial.println("PHASE fast_prepared; pixel_scans=0; auto_off_in_8s");
    status(); return;
  }
  if (!strcmp(text, "settle")) {
    appleLoop = false;
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
  if (newPattern) {
    appleLoop = false;
    panel.setScanRows(0, kHeight - 1);
    start(mode); return;
  }
  unsigned value;
  unsigned other;
  char extra;
  if (!strcmp(text, "stable off") ||
      (sscanf(text, "stable %u %c", &value, &extra) == 1 && (value == 2 || value == 4))) {
    if (!strcmp(text, "stable off")) value = 0;
    if (value && (mode == DriveMode::Net || planner().reversalPercent() || planner().weakTail())) {
      Serial.println("Stable tail requires budget mode, history off, and weak off."); return;
    }
    if (!clean(false)) return;
    if (!planner().setStableTail(value)) {
      Serial.println("Stable tail configuration rejected."); return;
    }
    planner().reset(mode, frame);
    status(); return;
  }
  if (!strcmp(text, "weak off") ||
      (sscanf(text, "weak %u %c", &value, &extra) == 1 && value >= 1 && value <= 4)) {
    if (!strcmp(text, "weak off")) value = 0;
    if (value && (mode == DriveMode::Net || planner().reversalPercent() || planner().stableTail())) {
      Serial.println("Weak tail requires budget mode, history off, and stable off."); return;
    }
    if (!clean(false)) return;
    if (!planner().setWeakTail(value)) {
      Serial.println("Weak tail configuration rejected."); return;
    }
    planner().reset(mode, frame);
    panel.setWeakState(value != 0);
    panel.setWeakSource(3);
    status(); return;
  }
  if (!strcmp(text, "history off") ||
      (sscanf(text, "history %u %c", &value, &extra) == 1 &&
       (value == 100 || value == 150 || value == 200))) {
    if (!strcmp(text, "history off")) value = 0;
    if (value && (planner().weakTail() || planner().stableTail())) {
      Serial.println("Disable weak and stable tails before selecting history mode."); return;
    }
    if (value && !Planner::supportsHistory(blackPulses, whitePulses)) {
      Serial.println("History requires lcm(black pulses, white pulses) <= 255."); return;
    }
    if (!clean(false)) return;
    if (!planner().setReversalPercent(value)) {
      Serial.println("History configuration rejected."); return;
    }
    planner().reset(mode, frame);
    status(); return;
  }
  char probeMode[8] = {};
  const int probeArgs = sscanf(text, "weakprobe %u %7s %c", &value, probeMode, &extra);
  if ((probeArgs == 1 || probeArgs == 2) && value >= 1 && value <= 11) {
    uint8_t source = 3;
    if (probeArgs == 2 && !strcmp(probeMode, "sameA")) source = 1;
    else if (probeArgs == 2 && !strcmp(probeMode, "sameB")) source = 2;
    else if (probeArgs == 2 && strcmp(probeMode, "weak")) {
      Serial.println("Use weakprobe N [weak|sameA|sameB]."); return;
    }
    weakProbe(value, source); return;
  }
  if (sscanf(text, "guard %u %c", &value, &extra) == 1 && value <= 3000) {
    if (!clean(false)) return;
    fullGuardMs = value; status(); return;
  }
  if (sscanf(text, "decodebench %u %c", &value, &extra) == 1 && value >= 1 && value <= 1000) {
    if (running || pending || panel.analogOn() || !appleReady) {
      Serial.println("Decoder benchmark requires an idle board, analog off, and valid video."); return;
    }
    uint64_t decodeUs = 0;
    uint32_t checksum = 2166136261u;
    const int64_t wallStart = esp_timer_get_time();
    for (unsigned n = 0; n < value; ++n) {
      const int64_t begin = esp_timer_get_time();
      if (!apple.frame(n % apple.count(), pendingFrame)) {
        Serial.println("FAULT decoder benchmark"); faulted = true; return;
      }
      decodeUs += esp_timer_get_time() - begin;
      for (uint8_t byte : pendingFrame) checksum = (checksum ^ byte) * 16777619u;
      if (n % 16 == 15) delay(1);
    }
    Serial.printf("DECODEBENCH frames=%u decode_us=%llu decode_fps=%.2f wall_us=%llu hash=%lu panel_scans=0\n",
                  value, (unsigned long long)decodeUs, decodeUs ? value * 1000000.0 / decodeUs : 0,
                  (unsigned long long)(esp_timer_get_time() - wallStart), (unsigned long)checksum);
    status(); return;
  }
  if (sscanf(text, "verify %u %c", &value, &extra) == 1) {
    if (running || pending || !appleReady || value >= apple.count()) {
      Serial.println("Verification requires an idle board and a valid video frame."); return;
    }
    const int64_t begin = esp_timer_get_time();
    if (!apple.frame(value, pendingFrame)) {
      Serial.println("FAULT video verification decode"); faulted = true; return;
    }
    const int64_t decoded = esp_timer_get_time();
    uint32_t hash = 2166136261u;
    for (uint8_t byte : pendingFrame) hash = (hash ^ byte) * 16777619u;
    Serial.printf("VERIFY frame=%u hash=%lu decode_us=%lu\n", value, (unsigned long)hash,
                  (unsigned long)(decoded - begin));
    status(); return;
  }
  if (sscanf(text, "fr %u %c", &value, &extra) == 1 && value <= 7) {
    if (!clean(false)) return;
    panel.setFrameRate(value); status(); return;
  }
  if (!strcmp(text, "scan full")) {
    if (!clean(false)) return;
    panel.setScanRows(0, kHeight - 1); status(); return;
  }
  if (sscanf(text, "scan %u %u %c", &value, &other, &extra) == 2 &&
      value < kHeight && other < kHeight && other >= value + 15) {
    if (!clean(false)) return;
    panel.setScanRows(value, other); status(); return;
  }
  if (!strcmp(text, "reuse on") || !strcmp(text, "reuse off")) {
    if (!clean(false)) return;
    panel.setReuseMasks(!strcmp(text, "reuse on")); status(); return;
  }
  if (!strcmp(text, "wait irq") || !strcmp(text, "wait poll")) {
    irqWait = !strcmp(text, "wait irq"); status(); return;
  }
  if (sscanf(text, "hold %u %c", &value, &extra) == 1 && value >= 1 && value <= 200) {
    if (running || !diagnosticOffUs || !panel.analogOn()) {
      Serial.println("Send prepare first; hold is only allowed in the idle diagnostic session."); return;
    }
    diagnosticOffUs = 0;
    memset(planner().black, 0, kFrameBytes); memset(planner().white, 0, kFrameBytes);
    Serial.printf("PHASE zero_masks; scans=%u; all_pixels_HOLD\n", value);
    uint64_t busyTotal = 0, uploadTotal = 0;
    uint32_t busyMin = UINT32_MAX, busyMax = 0;
    for (unsigned i = 0; i < value; ++i) {
      ScanTiming timing;
      if (!panel.scan(planner().black, planner().white, timing)) { fault(); return; }
      busyTotal += timing.busyUs; uploadTotal += timing.uploadUs;
      busyMin = std::min(busyMin, timing.busyUs); busyMax = std::max(busyMax, timing.busyUs);
      ++scans;
    }
    lastBusyUs = busyTotal / value;
    Serial.printf("PROBE fr=%u gate_first=%u gate_last=%u scans=%u busy_us=%lu min_us=%lu max_us=%lu upload_us=%lu\n",
                  panel.frameRate(), panel.scanFirst(), panel.scanLast(), value,
                  (unsigned long)(busyTotal / value), (unsigned long)busyMin, (unsigned long)busyMax,
                  (unsigned long)(uploadTotal / value));
    if (!panel.powerOff()) { fault(); return; }
    Serial.println("PHASE zero_masks_done; analog_off; image_retained; no_full_refresh");
    status(); return;
  }
  if (sscanf(text, "pulse %u %u %c", &value, &other, &extra) == 2 &&
      value >= 1 && value <= 32 && other >= 1 && other <= 32) {
    if (planner().reversalPercent() && !Planner::supportsHistory(value, other)) {
      Serial.println("History requires lcm(black pulses, white pulses) <= 255."); return;
    }
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
  if (sscanf(text, "fps %u %c", &value, &extra) == 1 && value <= 120) {
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
                "overruns=%lu scans=%lu heap=%lu content_hz=%.2f skipped=%lu "
                "pipeline_used=%lu pipeline_staged=%lu pipeline_copy_us=%lu stable_tail_pulses=%lu "
                "maintenance_scans=%lu maintenance_pixels=%lu\n",
                modeName(), patternName(), stats.scans * 1000000.0 / elapsed,
                (unsigned long)(stats.plans ? stats.planUs / stats.plans : 0),
                (unsigned long)(stats.scans ? stats.uploadUs / stats.scans : 0),
                (unsigned long)(stats.scans ? stats.busyUs / stats.scans : 0),
                (unsigned long)(stats.plans ? stats.renderUs / stats.plans : 0),
                (unsigned long)(stats.scans ? stats.commitUs / stats.scans : 0),
                (unsigned long)stats.overruns, (unsigned long)scans,
                (unsigned long)ESP.getFreeHeap(), stats.contentFrames * 1000000.0 / elapsed,
                (unsigned long)stats.skippedFrames,
                (unsigned long)stats.pipelineUses, (unsigned long)stats.pipelineStages,
                (unsigned long)(stats.pipelineStages ? stats.pipelineCopyUs / stats.pipelineStages : 0),
                (unsigned long)stats.stableTailPulses,
                (unsigned long)stats.maintenanceScans, (unsigned long)stats.maintenancePixels);
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
  if (drift.phase != DriftPhase::None) { serviceDrift(); delay(1); return; }
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
    if (pipelineEnabled && !pipelinePrepared && !panel.scanReady()) {
      preparePipeline(std::max(nextUs, esp_timer_get_time() + lastBusyUs));
    }
    if (!panel.scanReady()) {
      if (irqWait) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
      else delay(1);
      return;
    }
    if (!finishPending()) return;
    reportStats(esp_timer_get_time());
  }
  const int64_t now = esp_timer_get_time();
  if (now < nextUs) { delay(1); return; }
  if (scans >= kCleanScans || now - lastCleanUs >= 45000000) { clean(true); return; }
  if (!framePrepared && !frozen) render(now);
  if (faulted) return;
  const bool maintenance = maintenanceEnabled && !frozen && mode == DriveMode::Budget &&
                           now - lastSettleUs >= 20000000;
  if (maintenance) lastSettleUs = now;
  const int64_t planStart = esp_timer_get_time();
  bool driven;
  if (pipelineReady && pipelineMaintenance == maintenance) {
    planner().applyPreview(frame, maintenance);
    driven = pipelineDriven;
    stats.planUs += pipelinePlanUs + esp_timer_get_time() - planStart;
    ++stats.pipelineUses;
  } else {
    driven = planner().plan(frame, maintenance, uint32_t(now / 1000));
    stats.planUs += esp_timer_get_time() - planStart;
  }
  pipelineReady = false;
  ++stats.plans;
  if (driven || holdPending) {
    pendingMaintenancePixels = 0;
    if (maintenance && driven) {
      for (size_t i = 0; i < kFrameBytes; ++i)
        pendingMaintenancePixels += __builtin_popcount(unsigned(planner().black[i] | planner().white[i]));
    }
    memcpy(pendingFrame, frame, kFrameBytes);
    pendingVideoFrame = videoFrame;
    if (!panel.startScan(planner().black, planner().white)) { fault(); return; }
    pending = true; pendingDriven = driven; pendingMaintenance = maintenance;
  } else {
    if (!maintenance) { planner().accept(frame); acceptVideoFrame(videoFrame); }
    if (frozen) {
      if (planner().hasStableTailPending()) {
        framePrepared = true;
        delay(1);
        return;
      }
      if (pattern == Pattern::Apple && appleLoop) {
        reportAppleResult();
        Serial.println("APPLE loop; restarting after full refresh");
        start(mode);
        return;
      }
      if (!panel.powerOff()) { fault(); return; }
      running = false;
      reportAppleResult();
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
