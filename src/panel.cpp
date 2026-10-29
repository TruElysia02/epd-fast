#include "panel.h"
#include "waveform.h"
#include <driver/gpio.h>
#include <esp_timer.h>
#include <algorithm>

namespace {
constexpr int kSclk = 0, kMosi = 1, kRst = 2, kDc = 21, kCs = 22, kBusy = 23;
TaskHandle_t waiter;
portMUX_TYPE edgeMux = portMUX_INITIALIZER_UNLOCKED;
volatile int64_t riseUs = 0, fallUs = 0;
DMA_ATTR uint8_t dmaBuffer[kFrameBytes];

void ARDUINO_ISR_ATTR busyEdge() {
  const int64_t now = esp_timer_get_time();
  const bool high = gpio_get_level(GPIO_NUM_23);
  portENTER_CRITICAL_ISR(&edgeMux);
  if (high) riseUs = now;
  else fallUs = now;
  portEXIT_CRITICAL_ISR(&edgeMux);
  if (!high && waiter) {
    BaseType_t wake = pdFALSE;
    vTaskNotifyGiveFromISR(waiter, &wake);
    if (wake) portYIELD_FROM_ISR();
  }
}
}  // namespace

bool Panel::fail(const char* operation, esp_err_t code) {
  snprintf(error_, sizeof(error_), "%s: %s", operation,
           code == ESP_OK ? "BUSY timeout or missing edge" : esp_err_to_name(code));
  Serial.printf("FAULT %s\n", error_);
  return false;
}

bool Panel::begin() {
  pinMode(kCs, OUTPUT); digitalWrite(kCs, HIGH);
  pinMode(kDc, OUTPUT); digitalWrite(kDc, HIGH);
  pinMode(kRst, OUTPUT); digitalWrite(kRst, HIGH);
  pinMode(kBusy, INPUT_PULLDOWN);
  waiter = xTaskGetCurrentTaskHandle();
  attachInterrupt(digitalPinToInterrupt(kBusy), busyEdge, CHANGE);
  spi_bus_config_t bus{};
  bus.mosi_io_num = kMosi; bus.miso_io_num = -1; bus.sclk_io_num = kSclk;
  bus.quadwp_io_num = -1; bus.quadhd_io_num = -1;
  bus.max_transfer_sz = kFrameBytes;
  const esp_err_t result = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
  return result == ESP_OK ? setClock(clockHz_) : fail("SPI bus init", result);
}

bool Panel::setClock(uint32_t hz) {
  if (spi_) {
    const esp_err_t result = spi_bus_remove_device(spi_);
    if (result != ESP_OK) return fail("SPI remove", result);
    spi_ = nullptr;
  }
  spi_device_interface_config_t config{};
  config.clock_speed_hz = hz; config.mode = 0;
  config.spics_io_num = kCs; config.queue_size = 1;
  const esp_err_t result = spi_bus_add_device(SPI2_HOST, &config, &spi_);
  if (result != ESP_OK) return fail("SPI device init", result);
  clockHz_ = hz;
  return true;
}

bool Panel::transfer(bool data, const uint8_t* bytes, size_t length) {
  if (length > sizeof(dmaBuffer) || !spi_) return fail("SPI buffer", ESP_ERR_INVALID_SIZE);
  digitalWrite(kDc, data);
  spi_transaction_t tx{};
  tx.length = length * 8;
  if (length <= 4) {
    tx.flags = SPI_TRANS_USE_TXDATA;
    memcpy(tx.tx_data, bytes, length);
  } else {
    memcpy(dmaBuffer, bytes, length);
    tx.tx_buffer = dmaBuffer;
  }
  const esp_err_t result = spi_device_polling_transmit(spi_, &tx);
  return result == ESP_OK || fail("SPI transmit", result);
}

bool Panel::cmd(uint8_t value) { return transfer(false, &value, 1); }
bool Panel::reg(uint8_t command, std::initializer_list<uint8_t> values) {
  return cmd(command) && transfer(true, values.begin(), values.size());
}

bool Panel::waitIdle(uint32_t timeoutMs) {
  const int64_t end = esp_timer_get_time() + int64_t(timeoutMs) * 1000;
  while (digitalRead(kBusy)) {
    if (esp_timer_get_time() >= end) return fail("wait idle");
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1) + 1);
  }
  return true;
}

bool Panel::activate(uint8_t flags, uint32_t timeoutMs, ScanTiming* timing) {
  if (!waitIdle(timeoutMs)) return false;
  int64_t start;
  return startActivation(flags, start) && finishActivation(start, timeoutMs, timing);
}

bool Panel::startActivation(uint8_t flags, int64_t& start) {
  if (!reg(0x22, {flags})) return false;
  ulTaskNotifyTake(pdTRUE, 0);
  portENTER_CRITICAL(&edgeMux); riseUs = 0; fallUs = 0; portEXIT_CRITICAL(&edgeMux);
  start = esp_timer_get_time();
  if (!cmd(0x20)) return false;
  while (!digitalRead(kBusy)) {
    if (esp_timer_get_time() - start > 10000) return fail("BUSY did not assert");
    delayMicroseconds(50);
  }
  return true;
}

bool Panel::finishActivation(int64_t start, uint32_t timeoutMs, ScanTiming* timing) {
  if (!waitIdle(timeoutMs)) return false;
  const int64_t end = esp_timer_get_time();
  // GPIO can already read low before the falling-edge ISR has been serviced.
  // Let that notification finish before treating an absent timestamp as a fault.
  portENTER_CRITICAL(&edgeMux);
  const bool missingFall = fallUs == 0;
  portEXIT_CRITICAL(&edgeMux);
  if (missingFall) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1) + 1);
  portENTER_CRITICAL(&edgeMux);
  const int64_t rising = riseUs, falling = fallUs;
  portEXIT_CRITICAL(&edgeMux);
  if (rising < start || falling <= rising) return fail("BUSY edge capture", ESP_ERR_INVALID_STATE);
  if (timing) {
    timing->busyUs = falling - rising;
    timing->activationUs = end - start;
  }
  return true;
}

bool Panel::powerOff() {
  if (!analogOn_) return waitIdle(6000);
  if (!activate(0x83, 6000)) return false;
  analogOn_ = false;
  return true;
}

bool Panel::reset(bool software) {
  if (!powerOff()) return false;
  digitalWrite(kRst, HIGH); delay(20);
  digitalWrite(kRst, LOW); delay(5);
  digitalWrite(kRst, HIGH); delay(20);
  if (!waitIdle(6000)) return false;
  if (software) {
    if (!cmd(0x12)) return false;
    delay(10);
  }
  return waitIdle(6000) && reg(0x01, {0x27, 0x01, 0x00}) &&
         reg(0x11, {0x03}) && reg(0x21, {0x00, 0x80}) && reg(0x18, {0x80});
}

bool Panel::window(uint16_t row0, uint16_t row1) {
  // 296 rows: the high address byte is essential, including on row 295.
  return reg(0x44, {0x00, 0x0f}) &&
         reg(0x45, {uint8_t(row0), uint8_t(row0 >> 8), uint8_t(row1), uint8_t(row1 >> 8)}) &&
         reg(0x4e, {0x00}) && reg(0x4f, {uint8_t(row0), uint8_t(row0 >> 8)});
}

bool Panel::plane(uint8_t command, const uint8_t* image, uint16_t row0, uint16_t row1) {
  return window(row0, row1) && cmd(command) &&
         transfer(true, image + row0 * (kWidth / 8), (row1 - row0 + 1) * (kWidth / 8));
}

bool Panel::full(const uint8_t* image) {
  // Restore the panel's OTP full waveform and voltages, as the existing T94
  // driver does. No 1.54-inch full-refresh table is copied onto this panel.
  if (!reset() || !reg(0x3c, {0x05}) || !plane(0x24, image) ||
      !plane(0x26, image) || !activate(0xf7, 6000)) return false;
  analogOn_ = false;
  return true;
}

bool Panel::prepareFast() {
  // Match the upstream full->fast handoff: release the full-refresh output
  // state with a hardware reset, then restore this panel's addressing before
  // loading the fast waveform. The partial path deliberately skips SWRESET.
  if (!reset(false) || !cmd(0x32) || !transfer(true, kFastWaveform, 153) ||
      !waitIdle(1000) || !reg(0x3f, {endOption_}) ||
      !reg(0x03, {kFastWaveform[154]}) ||
      !reg(0x04, {kFastWaveform[155], kFastWaveform[156], kFastWaveform[157]}) ||
      !reg(0x2c, {kFastWaveform[158]}) ||
      !reg(0x37, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}) ||
      !reg(0x3c, {0x80}) || !activate(0xc0, 6000)) return false;
  analogOn_ = true;
  previousFirst_ = 0; previousLast_ = kHeight - 1;
  return true;
}

bool Panel::scan(const uint8_t* black, const uint8_t* white, ScanTiming& timing) {
  return startScan(black, white) && finishScan(timing);
}

bool Panel::startScan(const uint8_t* black, const uint8_t* white) {
  if (!analogOn_) return fail("fast mode is not prepared", ESP_ERR_INVALID_STATE);
  if (pending_) return fail("previous scan is pending", ESP_ERR_INVALID_STATE);
  const MaskRows current = maskRows(black, white);
  const MaskRows rows = partialUpload_ ? uploadRows(current, {previousFirst_, previousLast_}) :
                                       MaskRows{0, kHeight - 1};
  const int64_t start = esp_timer_get_time();
  if (!plane(0x26, black, rows.first, rows.last) || !plane(0x24, white, rows.first, rows.last)) return false;
  pendingTiming_ = {};
  pendingTiming_.uploadUs = esp_timer_get_time() - start;
  if (!startActivation(0x0c, pendingSince_)) return false;
  previousFirst_ = current.first; previousLast_ = current.last;
  pending_ = true;
  return true;
}

bool Panel::scanReady() const {
  return !digitalRead(kBusy) || esp_timer_get_time() - pendingSince_ >= 1000000;
}

bool Panel::finishScan(ScanTiming& timing) {
  if (!pending_) return fail("no pending scan", ESP_ERR_INVALID_STATE);
  if (esp_timer_get_time() - pendingSince_ >= 1000000) return fail("scan deadline");
  if (!finishActivation(pendingSince_, 1000, &pendingTiming_)) return false;
  pending_ = false;
  timing = pendingTiming_;
  return true;
}
