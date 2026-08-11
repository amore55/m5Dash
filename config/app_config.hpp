// Compile-time application constants.
//
// Everything here is a *policy* decision (an interval, a limit, a threshold) rather than a
// user preference. User preferences live in dashboard::Settings and are stored in NVS.
//
// Rule of thumb for which one to use: if changing it should not require a firmware build,
// it belongs in Settings. If changing it needs a code review, it belongs here.

#pragma once

#include <cstddef>  // size_t — used below; do not rely on another header pulling it in
#include <cstdint>

namespace dash::cfg {

// ---------------------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------------------

/// Logical UI size after the 90-degree rotation applied in tab5_board.
/// The panel itself is 720x1280 portrait (BSP_LCD_H_RES x BSP_LCD_V_RES).
constexpr int kScreenWidth = 1280;
constexpr int kScreenHeight = 720;

/// Height of the thin page header and footer strips.
constexpr int kHeaderHeight = 56;
constexpr int kFooterHeight = 40;

// ---------------------------------------------------------------------------------------
// Scheduling
// ---------------------------------------------------------------------------------------

/// PageManager's single LVGL timer period. Every plugin's tick() runs at this rate, so it
/// must stay cheap. 250 ms is fast enough for a seconds-resolution clock and a countdown.
constexpr uint32_t kUiTickPeriodMs = 250;

/// How long a plugin may be left showing data before the footer marks it "stale",
/// expressed as a multiple of that plugin's own refresh interval.
constexpr uint32_t kStaleAfterIntervals = 3;

/// Refresh intervals (ms).
constexpr uint32_t kWeatherRefreshMs = 20u * 60u * 1000u;        // ~20 minutes
constexpr uint32_t kTflCommuteRefreshMs = 2u * 60u * 1000u;      // 2 minutes in commute
constexpr uint32_t kTflIdleRefreshMs = 10u * 60u * 1000u;        // 10 minutes otherwise
constexpr uint32_t kClaudeRefreshMs = 5u * 60u * 1000u;          // ~5 minutes
constexpr uint32_t kTodosRefreshMs = 30u * 1000u;                // UI reconcile only

/// The clock face redraws every UI tick; this interval only controls how often the plugin
/// re-checks whether the system clock has become valid (NTP or RTC), so it can stop showing
/// "waiting for time sync" promptly once it has.
constexpr uint32_t kClockRefreshMs = 60u * 1000u;

/// Telegram long-poll timeout handed to getUpdates. The HTTP timeout is this plus a margin.
constexpr uint32_t kTelegramLongPollSeconds = 25;

/// SNTP re-sync interval. The RX8130CE is written after every successful sync.
constexpr uint32_t kNtpResyncMs = 6u * 60u * 60u * 1000u;        // 6 hours

// ---------------------------------------------------------------------------------------
// Networking
// ---------------------------------------------------------------------------------------

/// Hard ceiling on any HTTP response we will buffer. A response larger than this is
/// abandoned rather than truncated, because a truncated JSON body is a parse error that
/// looks like a server fault.
constexpr size_t kHttpMaxResponseBytes = 64u * 1024u;

/// Per-request timeout for ordinary API calls.
constexpr int kHttpTimeoutMs = 12000;

/// Retry policy: attempt 1 immediately, then back off. Total worst case ~14 s of delay.
constexpr int kHttpMaxAttempts = 3;
constexpr int kHttpBackoffBaseMs = 2000;

/// Wi-Fi connect attempts before falling back to offline operation. Reconnection then
/// continues in the background on a slower cadence.
constexpr int kWifiFastRetries = 5;
constexpr uint32_t kWifiRetryBackoffMaxMs = 60u * 1000u;

/// SoftAP used for first-run provisioning.
constexpr const char* kSetupApSsid = "DeskDashboard-Setup";
constexpr int kSetupApChannel = 6;
constexpr int kSetupApMaxConnections = 2;

// ---------------------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------------------

constexpr const char* kNvsConfigNamespace = "dash.cfg";
constexpr const char* kNvsSecretNamespace = "dash.sec";
constexpr const char* kNvsStateNamespace = "dash.state";

constexpr const char* kLittleFsPartitionLabel = "storage";
constexpr const char* kLittleFsMountPoint = "/store";

/// Maximum number of task records held on the device. Bounded so that the task store is a
/// fixed-cost atomic rewrite rather than an unbounded blob. Adding a task beyond this fails
/// loudly instead of silently dropping the oldest.
constexpr size_t kMaxTasks = 200;

/// Maximum bytes a single cache entry may occupy on LittleFS.
constexpr size_t kMaxCacheEntryBytes = 32u * 1024u;

// ---------------------------------------------------------------------------------------
// Gestures
//
// First-guess values; all navigation tuning lives here so it can be adjusted in one place
// once it has been felt on real glass.
// ---------------------------------------------------------------------------------------

/// Minimum horizontal travel for a page change, in logical pixels (1280 wide screen).
constexpr int32_t kSwipeMinDistancePx = 120;

/// A horizontal swipe must be at least this many times longer than its vertical component,
/// so that a diagonal drag does not both change page and trigger a refresh.
constexpr int32_t kSwipeDominanceNumerator = 3;
constexpr int32_t kSwipeDominanceDenominator = 2;

/// Minimum downward travel for a manual refresh.
constexpr int32_t kPullRefreshMinDistancePx = 140;

/// A press held this long, having moved less than kLongPressSlopPx, opens Settings.
constexpr uint32_t kLongPressMs = 900;
constexpr int32_t kLongPressSlopPx = 24;

/// After any gesture fires, ignore further gestures for this long. Prevents a single
/// sloppy flick from paging twice.
constexpr uint32_t kGestureCooldownMs = 450;

/// Upper bound on gesture duration. A drag slower than this is treated as a scroll, not a
/// swipe, so that a plugin's own scrollable list still works.
constexpr uint32_t kSwipeMaxDurationMs = 1200;

// ---------------------------------------------------------------------------------------
// Burn-in mitigation
// ---------------------------------------------------------------------------------------

/// The clock face shifts by up to this many pixels on a slow cycle, so that no pixel holds
/// the same value indefinitely. Small enough to be unnoticeable at desk distance.
constexpr int32_t kBurnInShiftPx = 8;
constexpr uint32_t kBurnInPeriodMs = 5u * 60u * 1000u;

// ---------------------------------------------------------------------------------------
// OTA
// ---------------------------------------------------------------------------------------

/// Chunk used when streaming a firmware image to flash. Must be a multiple of 16.
constexpr size_t kOtaChunkBytes = 4096;

/// A firmware image outside this range is rejected before a single byte is written, so a
/// truncated download or an HTML error page can never reach the OTA partition.
constexpr size_t kOtaMinImageBytes = 256u * 1024u;
constexpr size_t kOtaMaxImageBytes = 6u * 1024u * 1024u;  // == ota_0 size in partitions.csv

/// Timeout for the (much longer) firmware download.
constexpr int kOtaHttpTimeoutMs = 30000;

// ---------------------------------------------------------------------------------------
// Timezone
// ---------------------------------------------------------------------------------------

/// POSIX TZ string for Europe/London including the British Summer Time rules
/// (last Sunday in March 01:00 UTC to last Sunday in October 02:00 UTC).
/// Using the rule string rather than a fixed offset is what makes BST automatic without a
/// tz database on the device.
constexpr const char* kDefaultTimezone = "GMT0BST,M3.5.0/1,M10.5.0/2";

constexpr const char* kDefaultNtpServer1 = "pool.ntp.org";
constexpr const char* kDefaultNtpServer2 = "time.cloudflare.com";

}  // namespace dash::cfg
