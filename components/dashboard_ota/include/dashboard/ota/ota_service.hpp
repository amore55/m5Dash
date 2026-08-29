// Checking for and applying a firmware update.
//
// MANUAL BY DEFAULT, per Settings::ota_automatic_install — this service only ever acts when
// asked (requestCheck() / requestInstall()), whether that ask comes from a settings-page button
// or from app_main polling on a schedule when the owner has opted into automatic install. It
// never decides to update on its own initiative.
//
// A BAD DOWNLOAD CAN NEVER BECOME A BAD BOOT. The size is checked before a byte is written (the
// manifest's declared size against Settings' configured bounds, and the OTA partition's own
// capacity via esp_ota_begin's own size argument), and the SHA-256 is checked after the whole
// image has streamed, BEFORE esp_ota_set_boot_partition() is ever called. Any mismatch calls
// esp_ota_abort() and leaves the currently-running partition as the boot target — the failure
// mode is "the update did not happen", never "the device now boots something unverified".
//
// ROLLBACK. CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is on, so a freshly-flashed image boots in
// ESP_OTA_IMG_PENDING_VERIFY and the bootloader will revert to the previous slot on the NEXT
// reset unless something calls esp_ota_mark_app_valid_cancel_rollback() first. confirmBootIfPending()
// is that call — see its own comment for when app_main should make it.
//
// THREADING. requestCheck() / requestInstall() are fire-and-forget: they post a job to this
// service's own worker task and return immediately, so the caller (an HTTP handler, app_main's
// startup) never blocks for the tens of seconds to minutes an install can take. getProgress()
// is the thread-safe way to see how it is going, from any thread, at any time.

#pragma once

#include <cstddef>
#include <mutex>

#include "esp_err.h"

#include "dashboard/fixed_string.hpp"
#include "dashboard/net/https_client.hpp"
#include "dashboard/ota/manifest.hpp"
#include "dashboard/worker.hpp"

namespace dashboard::ota {

enum class OtaState : uint8_t {
    Idle,
    CheckingManifest,
    UpToDate,
    UpdateAvailable,
    Downloading,
    Verifying,
    Applying,   ///< Between esp_ota_end() succeeding and esp_restart(). Very brief.
    Failed,
};

/// Lower-case, underscore-separated names — "up_to_date", "available" — because the one consumer
/// is the settings page's JavaScript, which switches on exactly these strings. Change one side,
/// change the other: nothing enforces them staying in sync but eyeballing both files at once.
const char* toString(OtaState state);

struct OtaProgress {
    OtaState state = OtaState::Idle;

    /// The last manifest that parsed successfully, whatever it said — kept even across a later
    /// failure, so a settings page showing "0.3.0 available" does not blank itself out just
    /// because a subsequent poll hit a network blip.
    Manifest manifest;

    /// Bytes written to the OTA partition so far, for a progress bar. 0 outside Downloading.
    size_t bytes_downloaded = 0;

    /// Human-facing detail. Set for Failed (why), and for UpToDate / UpdateAvailable (which
    /// version, so the settings page has something to print without re-deriving it).
    MediumString message;
};

class OtaService {
  public:
    /// Starts the worker task. Call once, during app start-up.
    esp_err_t start();

    /// Ask for a manifest check. Does not download anything — a few hundred bytes of JSON. Safe
    /// to call often. If the worker is already busy (a check or install in flight), this request
    /// is simply dropped, which is correct: the in-flight one will produce a progress update
    /// shortly regardless.
    void requestCheck(const char* manifest_url, const char* channel);

    /// Ask for the full download-verify-apply. Re-fetches the manifest first and re-validates it
    /// is still a genuine upgrade at the moment of the call, rather than trusting whatever the
    /// last requestCheck() found — see ota_service.cpp for why that matters.
    ///
    /// On success this device REBOOTS. The caller does not get to observe that outcome through
    /// getProgress() — there is no "Success" state above deliberately, because nothing is left
    /// running to report it once esp_restart() has been called.
    void requestInstall(const char* manifest_url, const char* channel);

    /// Thread-safe snapshot. Safe from any thread, including the LVGL thread and an HTTP
    /// handler's task.
    void getProgress(OtaProgress& out) const;

    bool busy() const { return worker_.busy() || worker_.pending() > 0; }

  private:
    void doCheck(MediumString manifest_url, ShortString channel);
    void doInstall(MediumString manifest_url, ShortString channel);

    /// Fetch and parse the manifest, and separately decide whether it is a genuine upgrade for
    /// THIS device. Split from the caller so both requestCheck() and requestInstall() apply
    /// exactly the same channel/version/minimum_version logic rather than two hand-written copies
    /// of it drifting apart.
    esp_err_t fetchManifest(const char* manifest_url, Manifest& out);

    enum class Verdict { Upgrade, AlreadyCurrent, NotEligible };

    /// Classifies a fetched manifest against THIS device's running version and configured
    /// channel, and explains the answer in `reason` either way — including the Upgrade case, so
    /// requestCheck()'s UpdateAvailable message has something to show without re-deriving it.
    Verdict classifyManifest(const Manifest& manifest, const char* channel,
                            MediumString& reason) const;

    void setProgress(const OtaProgress& progress);

    net::HttpsClient http_;
    Worker worker_;

    mutable std::mutex progress_mutex_;
    OtaProgress progress_;
};

/// If the running image is in ESP_OTA_IMG_PENDING_VERIFY (a freshly-applied OTA that has not yet
/// been confirmed healthy), mark it valid so the bootloader does not roll it back on the next
/// reset. A no-op — cheap to call unconditionally — on a device that booted normally from a
/// partition already marked valid, which is every boot except the one right after an OTA.
///
/// Call this from app_main ONCE START-UP HAS REACHED A POINT WORTH TRUSTING — after the display,
/// storage and network stack have all initialised without a panic — rather than at the very top
/// of app_main. The whole point of PENDING_VERIFY is to give a genuinely broken image a chance to
/// be rolled back automatically; confirming it valid before anything has actually been exercised
/// would defeat that.
void confirmBootIfPending();

}  // namespace dashboard::ota
