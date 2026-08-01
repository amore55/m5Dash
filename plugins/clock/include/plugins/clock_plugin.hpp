// Page 1: the clock.
//
// Built first because it has no API dependency — it is the page that proves the whole shell
// works, and the page that must keep working when everything else fails.
//
// Two faces, per the brief:
//   Minimal — one large time, day and date beneath. Quiet.
//   Flap    — each digit in its own card, split-flap / departure-board flavoured.
//
// Deliberate properties:
//   * requiresNetwork() is false. Once the time is known the clock needs nothing, so it never
//     shows an offline warning and is never suppressed by the offline check in PluginBase.
//   * showHeaderClock() is false. A second, smaller clock on the clock page would be silly.
//   * The refresh cycle does not fetch anything. It only re-checks whether the system clock has
//     become valid, so "waiting for time sync" clears promptly once SNTP or the RTC lands.
//   * Burn-in mitigation shifts the whole face by a few pixels on a slow cycle. This is an
//     always-on page on an OLED-adjacent panel; a static clock is the classic way to etch a
//     display.

#pragma once

#include <cstdint>
#include <ctime>

#include "dashboard/plugin_base.hpp"

namespace plugins {

enum class ClockFace : uint8_t {
    Minimal,
    Flap,
};

/// Parse a settings string ("minimal" / "flap") into a face. Unknown values fall back to
/// Minimal rather than failing, so a corrupt or future config cannot leave the page blank.
ClockFace clockFaceFromString(const char* text);
const char* clockFaceToString(ClockFace face);

class ClockPlugin final : public dashboard::PluginBase {
  public:
    ClockPlugin();

    uint32_t refreshIntervalMs() const override;

    // ---- configuration, applied live from Settings -------------------------------------
    void setFace(ClockFace face);
    void setShowSeconds(bool show);
    ClockFace face() const { return face_; }
    bool showSeconds() const { return show_seconds_; }

  protected:
    void buildBody(lv_obj_t* body) override;
    esp_err_t fetch(bool force) override;
    void updateUi() override;
    void onTick() override;

    bool requiresNetwork() const override { return false; }
    bool showHeaderClock() const override { return false; }
    /// No TLS, no JSON — the worker only ever evaluates the system clock.
    uint32_t workerStackBytes() const override { return 3072; }

  private:
    static constexpr size_t kDigitCount = 6;  ///< HHMMSS

    void buildMinimalFace(lv_obj_t* parent);
    void buildFlapFace(lv_obj_t* parent);
    void applyFaceVisibility();
    void renderTime(const std::tm& now, bool time_known);
    void renderMinimal(const std::tm& now, bool time_known);
    void renderFlap(const std::tm& now, bool time_known);
    void applyBurnInShift();

    ClockFace face_ = ClockFace::Minimal;
    bool show_seconds_ = false;

    lv_obj_t* container_ = nullptr;

    // Minimal face
    lv_obj_t* minimal_root_ = nullptr;
    lv_obj_t* minimal_time_ = nullptr;
    lv_obj_t* minimal_date_ = nullptr;

    // Flap face
    lv_obj_t* flap_root_ = nullptr;
    lv_obj_t* flap_digits_[kDigitCount] = {};
    lv_obj_t* flap_seconds_group_ = nullptr;
    lv_obj_t* flap_date_ = nullptr;

    /// Last rendered values, so the labels are only touched when they actually change.
    /// Rewriting a transformed label every 250 ms would force a needless full redraw.
    int last_rendered_second_ = -1;
    int last_rendered_minute_ = -1;
    int last_rendered_yday_ = -1;
    bool last_time_known_ = false;

    uint32_t burn_in_last_tick_ = 0;
    uint8_t burn_in_phase_ = 0;
};

}  // namespace plugins
