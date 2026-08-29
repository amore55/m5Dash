#include "dashboard/ota/ota_service.hpp"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "mbedtls/sha256.h"

#include "app_config.hpp"
#include "dashboard/net/response_buffer.hpp"
#include "dashboard/semver.hpp"
#include "version.hpp"

namespace dashboard::ota {
namespace {

constexpr const char* kTag = "ota";

/// Render a 32-byte SHA-256 digest as 64 lowercase hex characters, matching how
/// parseManifest() normalises the manifest's own value — so the comparison in doInstall() is a
/// plain strcmp, not a case-insensitive one.
void hexEncode(const uint8_t* digest, char* out) {
    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        out[i * 2] = kHex[digest[i] >> 4];
        out[i * 2 + 1] = kHex[digest[i] & 0x0F];
    }
    out[64] = '\0';
}

}  // namespace

const char* toString(OtaState state) {
    switch (state) {
        case OtaState::Idle:
            return "idle";
        case OtaState::CheckingManifest:
            return "checking";
        case OtaState::UpToDate:
            return "up_to_date";
        case OtaState::UpdateAvailable:
            return "available";
        case OtaState::Downloading:
            return "downloading";
        case OtaState::Verifying:
            return "verifying";
        case OtaState::Applying:
            return "applying";
        case OtaState::Failed:
            return "failed";
    }
    return "idle";
}

esp_err_t OtaService::start() {
    return worker_.start("ota", 16384);
}

void OtaService::setProgress(const OtaProgress& progress) {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    progress_ = progress;
}

void OtaService::getProgress(OtaProgress& out) const {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    out = progress_;
}

esp_err_t OtaService::fetchManifest(const char* manifest_url, Manifest& out) {
    out = Manifest{};
    if (manifest_url == nullptr || manifest_url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    net::ResponseBuffer buffer(8 * 1024);
    if (!buffer.valid()) {
        return ESP_ERR_NO_MEM;
    }

    net::HttpRequest request;
    request.url = manifest_url;

    net::HttpResponse response;
    const esp_err_t err = http_.get(request, buffer.data(), buffer.capacity(), response);
    if (err != ESP_OK) {
        return err;
    }
    if (!parseManifest(buffer.data(), response.length, out)) {
        ESP_LOGW(kTag, "manifest did not parse");
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

OtaService::Verdict OtaService::classifyManifest(const Manifest& manifest, const char* channel,
                                                 MediumString& reason) const {
    if (!manifest.valid) {
        reason.assign("update manifest could not be understood");
        return Verdict::NotEligible;
    }
    // Checked against the manifest's OWN claim, not trusted from the URL that fetched it — the
    // same instinct as every other parser in this project distrusting its transport. Only
    // enforced when BOTH sides have an opinion: a device with no channel configured, or a
    // manifest that declares none, accepts whatever it is given.
    if (channel != nullptr && channel[0] != '\0' && !manifest.channel.empty() &&
        !manifest.channel.equals(channel)) {
        char text[96];
        std::snprintf(text, sizeof(text), "manifest is for channel '%s', not '%s'",
                     manifest.channel.c_str(), channel);
        reason.assign(text);
        return Verdict::NotEligible;
    }
    if (!dashboard::isUpgrade(dash::kAppVersion, manifest.version.c_str())) {
        char text[96];
        std::snprintf(text, sizeof(text), "already on %s", dash::kAppVersion);
        reason.assign(text);
        return Verdict::AlreadyCurrent;
    }
    if (!manifest.minimum_version.empty() &&
        !dashboard::satisfiesMinimum(dash::kAppVersion, manifest.minimum_version.c_str())) {
        char text[128];
        std::snprintf(text, sizeof(text),
                     "this update needs at least %s first; running %s",
                     manifest.minimum_version.c_str(), dash::kAppVersion);
        reason.assign(text);
        return Verdict::NotEligible;
    }
    char text[96];
    std::snprintf(text, sizeof(text), "%s is available", manifest.version.c_str());
    reason.assign(text);
    return Verdict::Upgrade;
}

void OtaService::requestCheck(const char* manifest_url, const char* channel) {
    if (manifest_url == nullptr) {
        return;
    }
    MediumString url(manifest_url);
    ShortString ch(channel != nullptr ? channel : "");
    worker_.post([this, url, ch]() { doCheck(url, ch); });
}

void OtaService::requestInstall(const char* manifest_url, const char* channel) {
    if (manifest_url == nullptr) {
        return;
    }
    MediumString url(manifest_url);
    ShortString ch(channel != nullptr ? channel : "");
    worker_.post([this, url, ch]() { doInstall(url, ch); });
}

void OtaService::doCheck(MediumString manifest_url, ShortString channel) {
    OtaProgress progress;
    progress.state = OtaState::CheckingManifest;
    setProgress(progress);

    Manifest manifest;
    if (fetchManifest(manifest_url.c_str(), manifest) != ESP_OK) {
        progress.state = OtaState::Failed;
        progress.message.assign("could not read the update manifest");
        setProgress(progress);
        return;
    }

    MediumString reason;
    const Verdict verdict = classifyManifest(manifest, channel.c_str(), reason);
    progress.manifest = manifest;
    progress.message = reason;
    switch (verdict) {
        case Verdict::Upgrade:
            progress.state = OtaState::UpdateAvailable;
            break;
        case Verdict::AlreadyCurrent:
            progress.state = OtaState::UpToDate;
            break;
        case Verdict::NotEligible:
            progress.state = OtaState::Failed;
            break;
    }
    setProgress(progress);
    ESP_LOGI(kTag, "check: %s", reason.c_str());
}

void OtaService::doInstall(MediumString manifest_url, ShortString channel) {
    OtaProgress progress;
    progress.state = OtaState::CheckingManifest;
    setProgress(progress);

    // Re-fetched and re-classified HERE rather than trusting whatever a PRIOR requestCheck()
    // found: settings, the manifest, or the running version could all have moved on since then,
    // and this is the one call in the whole service that is about to overwrite flash — nothing
    // about it should rely on a decision made moments or minutes earlier.
    Manifest manifest;
    if (fetchManifest(manifest_url.c_str(), manifest) != ESP_OK) {
        progress.state = OtaState::Failed;
        progress.message.assign("could not read the update manifest");
        setProgress(progress);
        return;
    }

    MediumString reason;
    const Verdict verdict = classifyManifest(manifest, channel.c_str(), reason);
    progress.manifest = manifest;
    if (verdict != Verdict::Upgrade) {
        progress.state = (verdict == Verdict::AlreadyCurrent) ? OtaState::UpToDate
                                                              : OtaState::Failed;
        progress.message = reason;
        setProgress(progress);
        ESP_LOGW(kTag, "install refused: %s", reason.c_str());
        return;
    }

    if (manifest.size < dash::cfg::kOtaMinImageBytes ||
        manifest.size > dash::cfg::kOtaMaxImageBytes) {
        progress.state = OtaState::Failed;
        progress.message.assign("declared image size is implausible");
        setProgress(progress);
        ESP_LOGE(kTag, "manifest size %u outside [%u, %u]",
                 static_cast<unsigned>(manifest.size),
                 static_cast<unsigned>(dash::cfg::kOtaMinImageBytes),
                 static_cast<unsigned>(dash::cfg::kOtaMaxImageBytes));
        return;
    }

    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if (target == nullptr) {
        progress.state = OtaState::Failed;
        progress.message.assign("no OTA partition available");
        setProgress(progress);
        return;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, manifest.size, &handle);
    if (err != ESP_OK) {
        progress.state = OtaState::Failed;
        progress.message.assign("could not start the flash write");
        setProgress(progress);
        ESP_LOGE(kTag, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return;
    }

    progress.state = OtaState::Downloading;
    progress.bytes_downloaded = 0;
    setProgress(progress);
    ESP_LOGI(kTag, "downloading %s, %u bytes, to partition '%s'", manifest.version.c_str(),
             static_cast<unsigned>(manifest.size), target->label);

    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);  // 0 = SHA-256, not SHA-224

    size_t bytes_written = 0;
    bool write_failed = false;

    const net::HttpsClient::StreamSink sink = [&](const uint8_t* data, size_t len) -> bool {
        // Refuse a server sending more than the manifest promised BEFORE writing it — a
        // compromised or merely misconfigured host answering with something larger must not be
        // allowed to write past what esp_ota_begin() sized the partition write for.
        if (bytes_written + len > manifest.size) {
            ESP_LOGE(kTag, "server sent more than the manifest declared (>%u bytes)",
                     static_cast<unsigned>(manifest.size));
            write_failed = true;
            return false;
        }
        const esp_err_t write_err = esp_ota_write(handle, data, len);
        if (write_err != ESP_OK) {
            ESP_LOGE(kTag, "esp_ota_write failed: %s", esp_err_to_name(write_err));
            write_failed = true;
            return false;
        }
        mbedtls_sha256_update(&sha_ctx, data, len);
        bytes_written += len;

        // Cheap enough to do on every chunk: setProgress() is a mutex lock and a struct copy,
        // not an allocation, and a settings page polling this wants to see the bar move.
        OtaProgress live;
        live.state = OtaState::Downloading;
        live.manifest = manifest;
        live.bytes_downloaded = bytes_written;
        setProgress(live);
        return true;
    };

    net::HttpRequest request;
    request.url = manifest.url.c_str();
    request.timeout_ms = dash::cfg::kOtaHttpTimeoutMs;

    net::HttpResponse response;
    err = http_.streamGet(request, sink, response);

    if (err != ESP_OK || write_failed || bytes_written != manifest.size) {
        mbedtls_sha256_free(&sha_ctx);
        esp_ota_abort(handle);
        progress.state = OtaState::Failed;
        progress.message.assign(write_failed ? "the flash write failed"
                                             : "the download did not complete");
        setProgress(progress);
        ESP_LOGE(kTag, "download failed: %s (%u of %u bytes)", esp_err_to_name(err),
                 static_cast<unsigned>(bytes_written), static_cast<unsigned>(manifest.size));
        return;
    }

    progress.state = OtaState::Verifying;
    setProgress(progress);

    uint8_t digest[32];
    mbedtls_sha256_finish(&sha_ctx, digest);
    mbedtls_sha256_free(&sha_ctx);

    char digest_hex[65];
    hexEncode(digest, digest_hex);

    if (std::strcmp(digest_hex, manifest.sha256.c_str()) != 0) {
        esp_ota_abort(handle);
        progress.state = OtaState::Failed;
        progress.message.assign("checksum did not match; update discarded");
        setProgress(progress);
        // The computed digest is not a secret, but logging BOTH values is what makes a real
        // corruption-vs-manifest-typo distinction possible from the field, so it is worth the
        // few extra log lines this one failure path costs.
        ESP_LOGE(kTag, "checksum mismatch: got %s, manifest said %s", digest_hex,
                 manifest.sha256.c_str());
        return;
    }

    // esp_ota_end() does its own image-header validation (magic bytes, embedded checksum) on top
    // of the SHA-256 check above — a second, independent gate on the same "is this really a
    // valid image" question, using a completely different mechanism. Both have to agree.
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        progress.state = OtaState::Failed;
        progress.message.assign("the image failed final validation");
        setProgress(progress);
        ESP_LOGE(kTag, "esp_ota_end failed: %s", esp_err_to_name(err));
        return;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        progress.state = OtaState::Failed;
        progress.message.assign("could not set the new boot partition");
        setProgress(progress);
        ESP_LOGE(kTag, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return;
    }

    progress.state = OtaState::Applying;
    progress.message.assign("restarting...");
    setProgress(progress);
    ESP_LOGW(kTag, "update to %s verified and applied; restarting", manifest.version.c_str());

    // Deliberately no delay: anyone watching the settings page is polling getProgress(), which
    // has already seen "Applying" in the line above, and there is nothing further this call
    // could usefully do before rebooting into the new image.
    esp_restart();
}

void confirmBootIfPending() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running == nullptr) {
        return;
    }

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;  // ordinary boot from an already-confirmed partition — the common case
    }

    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(kTag, "OTA image confirmed valid; rollback cancelled");
    } else {
        ESP_LOGW(kTag, "could not confirm the OTA image valid: %s", esp_err_to_name(err));
    }
}

}  // namespace dashboard::ota
