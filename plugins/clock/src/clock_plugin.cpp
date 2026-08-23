#include "plugins/clock_plugin.hpp"

#include <cstdio>
#include <cstring>

#include "esp_log.h"

#include "app_config.hpp"
#include "dashboard/net/response_buffer.hpp"
#include "dashboard/storage/cache_store.hpp"
#include "dashboard/theme.hpp"
#include "dashboard/time_utils.hpp"

namespace plugins {
namespace {

using dashboard::theme::applyHeroScale;
namespace theme = dashboard::theme;
namespace timeutil = dashboard::timeutil;
using dashboard::storage::CacheStore;

constexpr const char* kTag = "clock";

/// Cache key for the quote body, so a restart inside the same half-day reuses it.
constexpr const char* kQuoteCacheKey = "quote";

/// Attempts allowed per half-day before giving up on the quote until the next bucket. Four, at
/// the 60 s refresh cadence, covers a slow Wi-Fi association or a brief upstream blip without
/// retrying a rejected key all day.
constexpr int kMaxQuoteAttempts = 4;

/// Minimal face: 48 px base font at 350 % ≈ 168 px tall digits, readable right across a room.
///
/// Kept below 400 %, where the scaled bitmap starts to look obviously soft — see theme.hpp. Note
/// heroScalePercent(): these were previously bare numbers in fixed-point units, so the digits
/// drew at 117 % rather than the 300 % the comment claimed.
constexpr int32_t kMinimalScale = theme::heroScalePercent(350);

/// Flap face: each digit sits in a fixed-width card, which is what makes proportional Montserrat
/// digits line up like a departure board. 200 % of 48 px ≈ 96 px inside a 180 px card.
constexpr int32_t kFlapScale = theme::heroScalePercent(200);
constexpr int32_t kFlapCardWidth = 120;
constexpr int32_t kFlapCardHeight = 180;

/// Burn-in nudge offsets, cycled slowly. Small enough to be invisible at desk distance, large
/// enough that no pixel holds the same value indefinitely.
constexpr int32_t kBurnInOffsets[][2] = {
    {0, 0},
    {dash::cfg::kBurnInShiftPx, dash::cfg::kBurnInShiftPx / 2},
    {0, dash::cfg::kBurnInShiftPx},
    {-dash::cfg::kBurnInShiftPx, dash::cfg::kBurnInShiftPx / 2},
};
constexpr uint8_t kBurnInPhaseCount =
    static_cast<uint8_t>(sizeof(kBurnInOffsets) / sizeof(kBurnInOffsets[0]));

constexpr const char* kUnknownTime = "--:--";
constexpr const char* kUnknownDigit = "-";

}  // namespace

ClockFace clockFaceFromString(const char* text) {
    if (text != nullptr && std::strcmp(text, "flap") == 0) {
        return ClockFace::Flap;
    }
    return ClockFace::Minimal;
}

const char* clockFaceToString(ClockFace face) {
    return face == ClockFace::Flap ? "flap" : "minimal";
}

ClockPlugin::ClockPlugin() : PluginBase("clock", "Clock") {}

uint32_t ClockPlugin::refreshIntervalMs() const { return dash::cfg::kClockRefreshMs; }

// ---------------------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------------------

void ClockPlugin::buildBody(lv_obj_t* body) {
    // One container holding both faces. The burn-in shift is applied to this, so it moves the
    // whole face as a unit whichever style is active.
    container_ = lv_obj_create(body);
    theme::makePlain(container_);
    lv_obj_set_width(container_, LV_PCT(100));
    lv_obj_set_flex_grow(container_, 1);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    buildMinimalFace(container_);
    buildFlapFace(container_);
    applyFaceVisibility();

    // Sibling of both faces, so it appears beneath whichever one is visible. Hidden until a
    // quote actually lands — an empty label would still take up its two lines and push the face
    // off centre for anyone with no api-ninjas key.
    quote_label_ = theme::makeLabel(container_, "", theme::fontTitle(), theme::textMuted());
    lv_obj_set_width(quote_label_, LV_PCT(80));
    lv_label_set_long_mode(quote_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(quote_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_add_flag(quote_label_, LV_OBJ_FLAG_HIDDEN);

    loadCachedQuote();
}

void ClockPlugin::buildMinimalFace(lv_obj_t* parent) {
    minimal_root_ = lv_obj_create(parent);
    theme::makePlain(minimal_root_);
    lv_obj_set_size(minimal_root_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(minimal_root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(minimal_root_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    // Vertical room for the scaled glyphs: the transform grows the drawn area beyond the label's
    // laid-out box, and a tight parent would clip it. At 350 % a 48 px line draws 168 px tall,
    // overflowing about 60 px above and below its box, so the padding has to cover that on both
    // sides — and the row gap has to keep the overflow off the date underneath it.
    lv_obj_set_style_pad_row(minimal_root_, theme::kGapXl * 3, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(minimal_root_, theme::kGapXl * 3, LV_PART_MAIN);

    minimal_time_ =
        theme::makeLabel(minimal_root_, kUnknownTime, theme::fontHero(), theme::textPrimary());
    applyHeroScale(minimal_time_, kMinimalScale);

    // fontDisplay() rather than fontTitle(): 40 px against 168 px digits, so the date still reads
    // as secondary but is legible from where the time is.
    minimal_date_ =
        theme::makeLabel(minimal_root_, "", theme::fontDisplay(), theme::textSecondary());
}

void ClockPlugin::buildFlapFace(lv_obj_t* parent) {
    flap_root_ = lv_obj_create(parent);
    theme::makePlain(flap_root_);
    lv_obj_set_size(flap_root_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(flap_root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(flap_root_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(flap_root_, theme::kGapXl, LV_PART_MAIN);

    lv_obj_t* row = lv_obj_create(flap_root_);
    theme::makePlain(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, theme::kGapS, LV_PART_MAIN);

    // Build hours and minutes directly on the row; seconds go in their own group so the whole
    // pair (colon included) can be hidden as a unit without disturbing the layout.
    auto makeCard = [this](lv_obj_t* card_parent, size_t digit_index) {
        lv_obj_t* card = lv_obj_create(card_parent);
        theme::makePlain(card);
        lv_obj_set_size(card, kFlapCardWidth, kFlapCardHeight);
        lv_obj_set_style_bg_color(card, theme::surfaceAlt(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(card, theme::kRadius, LV_PART_MAIN);
        lv_obj_set_style_border_width(card, theme::kHairline, LV_PART_MAIN);
        lv_obj_set_style_border_color(card, theme::border(), LV_PART_MAIN);

        lv_obj_t* label =
            theme::makeLabel(card, kUnknownDigit, theme::fontHero(), theme::textPrimary());
        lv_obj_center(label);
        applyHeroScale(label, kFlapScale);
        flap_digits_[digit_index] = label;
        return card;
    };

    auto makeColon = [](lv_obj_t* colon_parent) {
        lv_obj_t* colon =
            theme::makeLabel(colon_parent, ":", theme::fontHero(), theme::textMuted());
        return colon;
    };

    makeCard(row, 0);
    makeCard(row, 1);
    makeColon(row);
    makeCard(row, 2);
    makeCard(row, 3);

    flap_seconds_group_ = lv_obj_create(row);
    theme::makePlain(flap_seconds_group_);
    lv_obj_set_size(flap_seconds_group_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(flap_seconds_group_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(flap_seconds_group_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(flap_seconds_group_, theme::kGapS, LV_PART_MAIN);
    makeColon(flap_seconds_group_);
    makeCard(flap_seconds_group_, 4);
    makeCard(flap_seconds_group_, 5);

    flap_date_ = theme::makeLabel(flap_root_, "", theme::fontDisplay(), theme::textSecondary());
}

void ClockPlugin::applyFaceVisibility() {
    if (minimal_root_ == nullptr || flap_root_ == nullptr) {
        return;
    }
    const bool minimal = (face_ == ClockFace::Minimal);
    if (minimal) {
        lv_obj_remove_flag(minimal_root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(flap_root_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(minimal_root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(flap_root_, LV_OBJ_FLAG_HIDDEN);
    }

    if (flap_seconds_group_ != nullptr) {
        if (show_seconds_) {
            lv_obj_remove_flag(flap_seconds_group_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(flap_seconds_group_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ---------------------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------------------

void ClockPlugin::setFace(ClockFace face) {
    if (face_ == face) {
        return;
    }
    face_ = face;
    applyFaceVisibility();
    // Force a re-render: the newly visible face still holds whatever it last displayed.
    last_rendered_second_ = -1;
    last_rendered_minute_ = -1;
    last_rendered_yday_ = -1;
    markDirty();
}

void ClockPlugin::setShowSeconds(bool show) {
    if (show_seconds_ == show) {
        return;
    }
    show_seconds_ = show;
    applyFaceVisibility();
    last_rendered_second_ = -1;
    last_rendered_minute_ = -1;
    markDirty();
}

// ---------------------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------------------

esp_err_t ClockPlugin::fetch(bool force) {
    (void)force;
    // The only thing that can be "wrong" with a clock is not knowing the time, so that is what
    // the state machine reports — which gives the page a truthful "waiting for time sync" footer
    // instead of silently showing 01/01/1970.
    if (!timeutil::systemTimeValid()) {
        setError("waiting for time sync");
        return ESP_ERR_INVALID_STATE;
    }

    // AFTER the time check and outside the state machine: the quote cannot influence what this
    // function returns. A page that reported an error because a decorative quote failed would be
    // lying about the clock, which is the one thing it must be trusted on.
    maybeFetchQuote();
    return ESP_OK;
}

void ClockPlugin::onNetworkChanged(bool online) {
    network_online_.store(online, std::memory_order_relaxed);
    PluginBase::onNetworkChanged(online);
}

void ClockPlugin::maybeFetchQuote() {
    if (!QuoteProvider::configured()) {
        return;  // no key: the feature is off, not broken
    }
    if (!network_online_.load(std::memory_order_relaxed)) {
        // No point resolving a hostname with no route. This is the common case at boot, where the
        // first refresh runs seconds before the Wi-Fi link comes up.
        return;
    }

    const int64_t bucket = quoteBucket(timeutil::nowUtc(), timeutil::systemTimeValid());
    if (bucket < 0) {
        return;  // clock not settled; a quote fetched now would be filed under the wrong half-day
    }

    {
        std::lock_guard<std::mutex> lock(modelMutex());
        if (quote_.valid && quote_.bucket == bucket) {
            return;  // already have this half-day's quote
        }
    }

    if (quote_attempted_bucket_ != bucket) {
        quote_attempted_bucket_ = bucket;
        quote_attempts_ = 0;
    }
    if (quote_attempts_ >= kMaxQuoteAttempts) {
        return;
    }
    ++quote_attempts_;

    dashboard::net::ResponseBuffer buffer(QuoteProvider::kResponseBytes);
    if (!buffer.valid()) {
        return;
    }

    Quote fetched;
    if (quotes_.fetch(buffer.data(), buffer.capacity(), fetched) != ESP_OK) {
        ESP_LOGW(kTag, "quote unavailable: %s", quotes_.lastError());
        return;
    }
    fetched.bucket = bucket;

    {
        std::lock_guard<std::mutex> lock(modelMutex());
        quote_ = fetched;
    }
    // Cached so a restart inside the same half-day shows the same quote rather than spending
    // another request — the whole point of a bucket rather than a timer.
    CacheStore::put(kQuoteCacheKey, buffer.data(), std::strlen(buffer.data()));
    ESP_LOGI(kTag, "quote for bucket %lld by %s", static_cast<long long>(bucket),
             fetched.author.empty() ? "an unknown author" : fetched.author.c_str());
    markDirty();
}

void ClockPlugin::loadCachedQuote() {
    if (!CacheStore::has(kQuoteCacheKey)) {
        return;
    }
    dashboard::net::ResponseBuffer buffer(QuoteProvider::kResponseBytes);
    if (!buffer.valid()) {
        return;
    }
    size_t length = 0;
    if (CacheStore::get(kQuoteCacheKey, buffer.data(), buffer.capacity(), &length) != ESP_OK ||
        length == 0) {
        return;
    }

    Quote cached;
    if (!parseQuote(buffer.data(), length, cached)) {
        CacheStore::remove(kQuoteCacheKey);
        return;
    }
    // The bucket is deliberately NOT restored from the cache — it was not stored, and the stamp
    // is the cache's own. Leaving it at -1 means the next fetch() re-derives the current bucket
    // and refreshes if the half-day has moved on, while the cached text shows meanwhile.
    std::lock_guard<std::mutex> lock(modelMutex());
    quote_ = cached;
}

void ClockPlugin::renderQuote(const Quote& quote) {
    if (quote_label_ == nullptr) {
        return;
    }
    if (!quote.valid || quote.text.empty()) {
        lv_obj_add_flag(quote_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    char text[320];
    if (quote.author.empty()) {
        std::snprintf(text, sizeof(text), "\"%s\"", quote.text.c_str());
    } else {
        // The bullet is in the glyph range (0x2022); an em dash is not, and would render as an
        // empty box. See the font note in theme.hpp.
        std::snprintf(text, sizeof(text), "\"%s\"  %s %s", quote.text.c_str(), "\xE2\x80\xA2",
                      quote.author.c_str());
    }
    lv_label_set_text(quote_label_, text);
    lv_obj_remove_flag(quote_label_, LV_OBJ_FLAG_HIDDEN);
}

void ClockPlugin::summarise(dashboard::PluginSummary& out) const {
    if (!timeutil::systemTimeValid()) {
        // Never show 01/01/1970 on the hub: an obviously wrong date is worse than an admitted
        // unknown, because it looks like the device is working.
        out.primary.assign("--:--");
        out.secondary.assign("Waiting for time sync");
        return;
    }

    const std::tm now = timeutil::localNow();
    char text[32];
    timeutil::formatTime24h(text, sizeof(text), now, /*with_seconds=*/false);
    out.primary.assign(text);
    timeutil::formatBritishDateShort(text, sizeof(text), now);
    out.secondary.assign(text);
}

void ClockPlugin::updateUi() {
    const bool known = timeutil::systemTimeValid();
    const std::tm now = timeutil::localNow();
    // A config change or a state change invalidates whatever is on screen.
    last_rendered_second_ = -1;
    last_rendered_minute_ = -1;
    renderTime(now, known);

    Quote quote;
    {
        std::lock_guard<std::mutex> lock(modelMutex());
        quote = quote_;
    }
    renderQuote(quote);
}

void ClockPlugin::onTick() {
    const bool known = timeutil::systemTimeValid();
    const std::tm now = timeutil::localNow();
    renderTime(now, known);
    applyBurnInShift();
}

void ClockPlugin::renderTime(const std::tm& now, bool time_known) {
    // Only touch the labels when the displayed value would actually change. onTick() runs four
    // times a second; rewriting transformed labels every time would force four full-screen
    // redraws a second for no visible benefit.
    const bool changed = (time_known != last_time_known_) ||
                         (show_seconds_ ? now.tm_sec != last_rendered_second_
                                        : now.tm_min != last_rendered_minute_) ||
                         now.tm_yday != last_rendered_yday_;
    if (!changed) {
        return;
    }
    last_time_known_ = time_known;
    last_rendered_second_ = now.tm_sec;
    last_rendered_minute_ = now.tm_min;
    last_rendered_yday_ = now.tm_yday;

    if (face_ == ClockFace::Minimal) {
        renderMinimal(now, time_known);
    } else {
        renderFlap(now, time_known);
    }
}

void ClockPlugin::renderMinimal(const std::tm& now, bool time_known) {
    if (minimal_time_ == nullptr || minimal_date_ == nullptr) {
        return;
    }
    if (!time_known) {
        lv_label_set_text(minimal_time_, kUnknownTime);
        lv_label_set_text(minimal_date_, "Waiting for time synchronisation");
        return;
    }

    char time_text[16];
    timeutil::formatTime24h(time_text, sizeof(time_text), now, show_seconds_);
    lv_label_set_text(minimal_time_, time_text);

    char date_text[48];
    timeutil::formatBritishDate(date_text, sizeof(date_text), now);
    lv_label_set_text(minimal_date_, date_text);
}

void ClockPlugin::renderFlap(const std::tm& now, bool time_known) {
    char digits[kDigitCount + 1];
    if (time_known) {
        std::snprintf(digits, sizeof(digits), "%02d%02d%02d", now.tm_hour, now.tm_min,
                      now.tm_sec);
    } else {
        std::snprintf(digits, sizeof(digits), "------");
    }

    for (size_t i = 0; i < kDigitCount; ++i) {
        if (flap_digits_[i] == nullptr) {
            continue;
        }
        const char text[2] = {digits[i], '\0'};
        lv_label_set_text(flap_digits_[i], text);
    }

    if (flap_date_ != nullptr) {
        if (time_known) {
            char date_text[48];
            timeutil::formatBritishDate(date_text, sizeof(date_text), now);
            lv_label_set_text(flap_date_, date_text);
        } else {
            lv_label_set_text(flap_date_, "Waiting for time synchronisation");
        }
    }
}

void ClockPlugin::applyBurnInShift() {
    if (container_ == nullptr) {
        return;
    }
    // lv_tick_elaps is wraparound-safe; plain subtraction on the 32-bit tick counter is not.
    if (lv_tick_elaps(burn_in_last_tick_) < dash::cfg::kBurnInPeriodMs) {
        return;
    }
    burn_in_last_tick_ = lv_tick_get();
    burn_in_phase_ = static_cast<uint8_t>((burn_in_phase_ + 1) % kBurnInPhaseCount);
    lv_obj_set_style_translate_x(container_, kBurnInOffsets[burn_in_phase_][0], LV_PART_MAIN);
    lv_obj_set_style_translate_y(container_, kBurnInOffsets[burn_in_phase_][1], LV_PART_MAIN);
}

}  // namespace plugins
