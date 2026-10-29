#pragma once
#include <Arduino.h>
#include <driver/spi_master.h>
#include "planner.h"

struct ScanTiming {
  uint32_t uploadUs = 0;
  uint32_t busyUs = 0;
  uint32_t activationUs = 0;
  uint8_t planeWrites = 0;
};

class Panel {
 public:
  bool begin();
  bool full(const uint8_t* image);
  bool prepareFast();
  bool scan(const uint8_t* black, const uint8_t* white, ScanTiming& timing);
  bool startScan(const uint8_t* black, const uint8_t* white);
  bool scanReady() const;
  bool finishScan(ScanTiming& timing);
  void setEndOption(uint8_t value) { endOption_ = value; }
  uint8_t endOption() const { return endOption_; }
  void setPartialUpload(bool enabled) { partialUpload_ = enabled; }
  bool partialUpload() const { return partialUpload_; }
  // Applied by the next prepareFast(); callers stop/full-refresh before changing.
  bool setFrameRate(uint8_t code);
  uint8_t frameRate() const { return frameRate_; }
  void setPairedHold(bool enabled) { pairedHold_ = enabled; }
  bool pairedHold() const { return pairedHold_; }
  void setWeakState(bool enabled) { weakState_ = enabled; }
  bool weakState() const { return weakState_; }
  bool setWeakSource(uint8_t selector);
  uint8_t weakSource() const { return weakSource_; }
  bool setScanRows(uint16_t first, uint16_t last);
  uint16_t scanFirst() const { return scanFirst_; }
  uint16_t scanLast() const { return scanLast_; }
  void setReuseMasks(bool enabled) { reuseMasks_ = enabled; }
  bool reuseMasks() const { return reuseMasks_; }
  bool analogOn() const { return analogOn_; }
  bool powerOff();
  bool setClock(uint32_t hz);
  uint32_t clock() const { return clockHz_; }
  const char* error() const { return error_; }

 private:
  bool fail(const char* operation, esp_err_t code = ESP_OK);
  bool transfer(bool data, const uint8_t* bytes, size_t length);
  bool cmd(uint8_t value);
  bool reg(uint8_t command, std::initializer_list<uint8_t> values);
  bool window(uint16_t row0 = 0, uint16_t row1 = kHeight - 1);
  bool reset(bool software = true);
  bool waitIdle(uint32_t timeoutMs);
  bool activate(uint8_t flags, uint32_t timeoutMs, ScanTiming* timing = nullptr);
  bool startActivation(uint8_t flags, int64_t& start);
  bool finishActivation(int64_t start, uint32_t timeoutMs, ScanTiming* timing);
  bool plane(uint8_t command, const uint8_t* image, uint16_t row0 = 0, uint16_t row1 = kHeight - 1);
  spi_device_handle_t spi_ = nullptr;
  uint32_t clockHz_ = 20000000;
  bool analogOn_ = false;
  bool partialUpload_ = false; // Full RAM writes until optical window behavior is verified.
  uint8_t frameRate_ = 2;
  bool pairedHold_ = false;
  bool weakState_ = false;
  uint8_t weakSource_ = 3;
  uint16_t scanFirst_ = 0, scanLast_ = kHeight - 1;
  bool reuseMasks_ = false;
  bool masksValid_ = false;
  uint8_t cachedBlack_[kFrameBytes]{};
  uint8_t cachedWhite_[kFrameBytes]{};
  uint8_t endOption_ = 0x02;
  bool pending_ = false;
  int64_t pendingSince_ = 0;
  ScanTiming pendingTiming_;
  // Previous nonzero RAM rows must also be cleared when a moving mask shrinks.
  uint16_t previousFirst_ = 0, previousLast_ = kHeight - 1;
  char error_[100]{};
};
