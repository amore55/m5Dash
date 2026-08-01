#include "tab5_board/rtc_rx8130.hpp"

#include <cstring>
#include <sys/time.h>

#include "esp_log.h"

namespace tab5 {
namespace {

constexpr const char* kTag = "rtc8130";

// Register map — Epson RX8130CE.
constexpr uint8_t kRegSec = 0x10;
constexpr uint8_t kRegFlag = 0x1D;
constexpr uint8_t kRegCtrl0 = 0x1E;

constexpr uint8_t kFlagVlf = 1u << 1;   // Voltage Low Flag: oscillator stopped, data lost.
constexpr uint8_t kCtrl0Stop = 1u << 6; // 1 = time counters halted.

constexpr int kI2cTimeoutMs = 200;

/// The RX8130CE stores the year as an offset from 2000.
constexpr int kRtcEpochYear = 2000;

uint8_t toBcd(int value) {
    return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

int fromBcd(uint8_t value) {
    return ((value >> 4) & 0x0F) * 10 + (value & 0x0F);
}

/// Days since 1970-01-01 for a proleptic Gregorian y/m/d (Howard Hinnant's days_from_civil).
///
/// Used instead of timegm() because timegm is a GNU extension whose availability in the
/// toolchain's newlib we would rather not depend on, and because this is exact and
/// branch-free for every date the RTC can hold.
long long daysFromCivil(int y, unsigned m, unsigned d) {
    y -= (m <= 2) ? 1 : 0;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);           // [0, 399]
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;  // [0, 365]
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;       // [0, 146096]
    return era * 146097LL + static_cast<long long>(doe) - 719468LL;
}

/// Broken-down UTC time -> Unix epoch seconds. Returns -1 on an out-of-range input.
std::time_t utcToEpoch(const std::tm& tm) {
    if (tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31) {
        return static_cast<std::time_t>(-1);
    }
    const long long days = daysFromCivil(tm.tm_year + 1900, static_cast<unsigned>(tm.tm_mon + 1),
                                         static_cast<unsigned>(tm.tm_mday));
    return static_cast<std::time_t>(days * 86400LL + tm.tm_hour * 3600LL + tm.tm_min * 60LL +
                                    tm.tm_sec);
}

}  // namespace

Rx8130::~Rx8130() {
    if (dev_ != nullptr) {
        i2c_master_bus_rm_device(dev_);
        dev_ = nullptr;
    }
}

esp_err_t Rx8130::attach(i2c_master_bus_handle_t bus) {
    if (bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dev_ != nullptr) {
        return ESP_OK;  // idempotent
    }

    // Probe before adding a device handle, so a board without a populated RTC degrades to
    // "no RTC" rather than to a stream of I2C timeouts on every read.
    esp_err_t err = i2c_master_probe(bus, kI2cAddress, kI2cTimeoutMs);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "no RX8130CE at 0x%02X (%s); running without an RTC", kI2cAddress,
                 esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = kI2cAddress,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(bus, &dev_cfg, &dev_);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        dev_ = nullptr;
        return err;
    }

    // Make sure the counters are running. A brand-new board can come up with STOP set.
    uint8_t ctrl0 = 0;
    if (readRegs(kRegCtrl0, &ctrl0, 1) == ESP_OK && (ctrl0 & kCtrl0Stop) != 0) {
        ESP_LOGW(kTag, "RTC counters were halted (CTRL0=0x%02X); starting them", ctrl0);
        writeReg(kRegCtrl0, static_cast<uint8_t>(ctrl0 & ~kCtrl0Stop));
    }

    uint8_t flag = 0;
    if (readRegs(kRegFlag, &flag, 1) == ESP_OK) {
        time_valid_ = (flag & kFlagVlf) == 0;
        if (!time_valid_) {
            ESP_LOGW(kTag, "VLF set (FLAG=0x%02X): RTC contents are not trustworthy", flag);
        }
    }

    ESP_LOGI(kTag, "RX8130CE attached at 0x%02X, stored time %s", kI2cAddress,
             time_valid_ ? "valid" : "invalid");
    return ESP_OK;
}

esp_err_t Rx8130::readRegs(uint8_t reg, uint8_t* buf, size_t len) {
    if (dev_ == nullptr || buf == nullptr || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(dev_, &reg, 1, buf, len, kI2cTimeoutMs);
}

esp_err_t Rx8130::writeRegs(uint8_t reg, const uint8_t* buf, size_t len) {
    if (dev_ == nullptr || buf == nullptr || len == 0 || len > 8) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t frame[9];
    frame[0] = reg;
    std::memcpy(&frame[1], buf, len);
    return i2c_master_transmit(dev_, frame, len + 1, kI2cTimeoutMs);
}

esp_err_t Rx8130::writeReg(uint8_t reg, uint8_t value) {
    return writeRegs(reg, &value, 1);
}

esp_err_t Rx8130::readUtc(std::tm& out) {
    uint8_t raw[7] = {};
    esp_err_t err = readRegs(kRegSec, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    std::tm tm = {};
    tm.tm_sec = fromBcd(raw[0] & 0x7F);
    tm.tm_min = fromBcd(raw[1] & 0x7F);
    tm.tm_hour = fromBcd(raw[2] & 0x3F);
    // raw[3] is WEEK (one-of-seven bits) and is intentionally ignored; see the header.
    tm.tm_mday = fromBcd(raw[4] & 0x3F);
    tm.tm_mon = fromBcd(raw[5] & 0x1F) - 1;
    tm.tm_year = fromBcd(raw[6]) + (kRtcEpochYear - 1900);
    tm.tm_isdst = 0;

    // Range-check before handing the value to anything that will do arithmetic on it. A
    // corrupted I2C read otherwise turns into a nonsense clock rather than an error.
    if (tm.tm_sec > 59 || tm.tm_min > 59 || tm.tm_hour > 23 || tm.tm_mday < 1 ||
        tm.tm_mday > 31 || tm.tm_mon < 0 || tm.tm_mon > 11) {
        ESP_LOGW(kTag, "RTC returned an out-of-range date; treating as invalid");
        time_valid_ = false;
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Normalise so tm_wday / tm_yday are populated consistently.
    std::time_t as_epoch = utcToEpoch(tm);
    if (as_epoch == static_cast<std::time_t>(-1)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    gmtime_r(&as_epoch, &out);
    return ESP_OK;
}

esp_err_t Rx8130::writeUtc(const std::tm& in) {
    if (dev_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const int year_offset = in.tm_year + 1900 - kRtcEpochYear;
    if (year_offset < 0 || year_offset > 99) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t ctrl0 = 0;
    esp_err_t err = readRegs(kRegCtrl0, &ctrl0, 1);
    if (err != ESP_OK) {
        return err;
    }

    // Halt the counters across the multi-byte write so the seconds cannot roll over
    // half-way through and produce a time an hour or a day out.
    err = writeReg(kRegCtrl0, static_cast<uint8_t>(ctrl0 | kCtrl0Stop));
    if (err != ESP_OK) {
        return err;
    }

    uint8_t raw[7];
    raw[0] = toBcd(in.tm_sec);
    raw[1] = toBcd(in.tm_min);
    raw[2] = toBcd(in.tm_hour);
    // WEEK is one-of-seven bits on this part. We write it for completeness; reads ignore it.
    raw[3] = static_cast<uint8_t>(1u << (in.tm_wday & 0x07));
    raw[4] = toBcd(in.tm_mday);
    raw[5] = toBcd(in.tm_mon + 1);
    raw[6] = toBcd(year_offset);

    esp_err_t write_err = writeRegs(kRegSec, raw, sizeof(raw));

    // Always restart the counters, even if the write failed — leaving STOP set would
    // silently freeze the clock.
    esp_err_t restart_err = writeReg(kRegCtrl0, static_cast<uint8_t>(ctrl0 & ~kCtrl0Stop));

    if (write_err != ESP_OK) {
        return write_err;
    }
    if (restart_err != ESP_OK) {
        return restart_err;
    }

    // Clear VLF: the contents are now known-good.
    uint8_t flag = 0;
    if (readRegs(kRegFlag, &flag, 1) == ESP_OK && (flag & kFlagVlf) != 0) {
        writeReg(kRegFlag, static_cast<uint8_t>(flag & ~kFlagVlf));
    }
    time_valid_ = true;
    return ESP_OK;
}

esp_err_t Rx8130::restoreSystemTime() {
    std::tm tm = {};
    esp_err_t err = readUtc(tm);
    if (err != ESP_OK) {
        return err;
    }
    if (!time_valid_) {
        return ESP_ERR_INVALID_STATE;
    }

    std::time_t epoch = utcToEpoch(tm);
    if (epoch <= 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const struct timeval tv = {.tv_sec = epoch, .tv_usec = 0};
    if (settimeofday(&tv, nullptr) != 0) {
        return ESP_FAIL;
    }
    ESP_LOGI(kTag, "system clock restored from RTC: %04d-%02d-%02d %02d:%02d:%02dZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return ESP_OK;
}

esp_err_t Rx8130::persistSystemTime() {
    std::time_t now = std::time(nullptr);
    // Anything before 2021 means the system clock was never set; do not overwrite a good
    // RTC with a bogus value.
    if (now < 1609459200) {
        return ESP_ERR_INVALID_STATE;
    }
    std::tm tm = {};
    gmtime_r(&now, &tm);
    esp_err_t err = writeUtc(tm);
    if (err == ESP_OK) {
        ESP_LOGI(kTag, "RTC updated from system clock");
    } else {
        ESP_LOGW(kTag, "failed to update RTC: %s", esp_err_to_name(err));
    }
    return err;
}

}  // namespace tab5
