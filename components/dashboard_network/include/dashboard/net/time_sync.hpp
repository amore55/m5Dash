// Network time, and the battery-backed clock it keeps honest.
//
// WHY BOTH SNTP AND AN RTC
//
// The Tab5 has an RX8130CE with a battery, so it keeps time across a power cut. But an RTC is
// only ever as correct as the last thing that told it the truth, and it drifts. SNTP is the thing
// that tells it the truth. So the two are a pair:
//
//   boot      -> read the RTC, so the clock is right within seconds of power-on and before any
//                network exists (tab5::Rx8130::restoreSystemTime(), called from app_main)
//   on sync   -> write the freshly-corrected system time back (Rx8130::persistSystemTime())
//
// Without the write-back the RTC would slowly drift out of usefulness; without the RTC the
// dashboard would show "--:--" for the first few seconds of every boot, and indefinitely on a
// boot with no network.
//
// TIMEZONE
//
// Not this file's problem. SNTP sets UTC; timeutil::setTimezone() has already installed the POSIX
// TZ rule from settings, so every localtime conversion picks it up. Nothing here converts.

#pragma once

#include <ctime>

#include "esp_err.h"

namespace dashboard::net {

class TimeSync {
  public:
    /// Configure and start the SNTP client. Only the first call does anything.
    ///
    /// Call once the link is actually up: SNTP started without a route just burns retries and
    /// fills the log with failures that say nothing about the real problem.
    esp_err_t begin(const char* primary, const char* secondary);

    bool started() const { return started_; }

    /// True once SNTP has set the clock at least once since boot.
    bool synced() const;

    /// Take the pending "just synced" event, if there is one. Returns true at most once per sync.
    ///
    /// Polled rather than delivered by callback on purpose. The interesting thing to do on a sync
    /// is an I2C write to the RTC, and SNTP's notification runs on the lwip task — whose modest
    /// stack should not be carrying a blocking bus transaction. So the caller polls from a task
    /// that can afford it.
    bool consumeSyncEvent();

    /// UTC epoch seconds at the last sync, or 0 if never.
    std::time_t lastSyncUtc() const;

  private:
    static void onSynced(struct timeval* tv);

    bool started_ = false;
};

}  // namespace dashboard::net
