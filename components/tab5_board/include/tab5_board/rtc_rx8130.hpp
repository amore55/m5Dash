// Epson RX8130CE real-time clock driver for the M5Stack Tab5.
//
// The Espressif Tab5 BSP does not expose the RTC (BSP_CAPS_BAT == 0 and there is no
// bsp_rtc_* API), so this is the one piece of low-level hardware access the dashboard
// implements itself.
//
// Verified facts this driver relies on:
//   * The chip is an Epson RX8130CE and sits on the BSP's I2C bus (SCL GPIO32 / SDA GPIO31)
//     at 7-bit address 0x32. Documented by M5Stack for the Tab5.
//   * Time registers are 0x10..0x16 = SEC, MIN, HOUR, WEEK, DAY, MONTH, YEAR, all BCD,
//     hours in 24-hour form. FLAG is 0x1D (bit 1 = VLF, "voltage low / data lost"),
//     CTRL0 is 0x1E (bit 6 = STOP). Taken from the Epson datasheet and cross-checked
//     against the mainline Linux `rtc-rx8130` driver.
//
// Deliberately NOT touched: CTRL1 (0x1F) — INIEN / CHGEN / BFVSEL select backup-cell
// trickle charging, and the correct values depend on which cell M5Stack fitted. Writing
// guesses there could damage hardware. See docs/IMPLEMENTATION_PLAN.md §10.3.

#pragma once

#include <ctime>
#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"

namespace tab5 {

class Rx8130 {
  public:
    /// 7-bit I2C address of the RX8130CE on the Tab5.
    static constexpr uint8_t kI2cAddress = 0x32;

    Rx8130() = default;
    ~Rx8130();

    Rx8130(const Rx8130&) = delete;
    Rx8130& operator=(const Rx8130&) = delete;

    /// Attach to an already-initialised I2C master bus (bsp_i2c_get_handle()).
    /// Probes the device; returns ESP_ERR_NOT_FOUND if it does not answer.
    esp_err_t attach(i2c_master_bus_handle_t bus);

    bool attached() const { return dev_ != nullptr; }

    /// True once attach() succeeded and the VLF flag was clear, i.e. the RTC has been
    /// running continuously and its contents can be trusted.
    bool timeValid() const { return attached() && time_valid_; }

    /// Read the RTC into a UTC broken-down time.
    ///
    /// tm_wday/tm_yday are recomputed from the date rather than read from the WEEK
    /// register: the RX8130CE encodes weekday as one-of-seven bits, and recomputing is
    /// both cheaper than trusting it and immune to a mis-set WEEK register.
    esp_err_t readUtc(std::tm& out);

    /// Write a UTC broken-down time. Stops the counter, writes, restarts it and clears the
    /// VLF flag so a subsequent read reports the time as valid.
    esp_err_t writeUtc(const std::tm& in);

    /// Convenience: read the RTC and push it into the system clock with settimeofday().
    /// Used at boot so the UI has a plausible time before (or without) NTP.
    esp_err_t restoreSystemTime();

    /// Convenience: take the current system time and persist it to the RTC.
    /// Called after every successful SNTP sync.
    esp_err_t persistSystemTime();

  private:
    esp_err_t readRegs(uint8_t reg, uint8_t* buf, size_t len);
    esp_err_t writeRegs(uint8_t reg, const uint8_t* buf, size_t len);
    esp_err_t writeReg(uint8_t reg, uint8_t value);

    i2c_master_dev_handle_t dev_ = nullptr;
    bool time_valid_ = false;
};

}  // namespace tab5
