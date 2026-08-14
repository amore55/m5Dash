// Page 2: the weather.
//
// The first page with an upstream dependency, and therefore the first one that has to be honest
// about not having data. Three things follow from that, and they are the whole design:
//
//   * The last good RESPONSE is written to CacheStore, so a device that boots with the internet
//     down shows yesterday's forecast labelled with its age instead of six dashes.
//   * All HTTP and JSON sit behind WeatherProvider. This class contains no network code, which is
//     what makes the layout reviewable on its own terms.
//   * Coordinates come from Settings and are never geocoded here. Geocoding happens once, in the
//     browser, on the settings page — see components/dashboard_network/web/settings.html. A
//     dashboard that resolved a place name on every refresh would be making two calls to answer one
//     question, and would break when the geocoder rate-limited it.
//
// Layout on the 1280x720 panel: current conditions on the left at hero size, a details card on the
// right, and a strip of the next six hours across the bottom.

#pragma once

#include <cstddef>
#include <ctime>

#include "dashboard/fixed_string.hpp"
#include "dashboard/plugin_base.hpp"
#include "plugins/weather_model.hpp"
#include "plugins/weather_provider.hpp"

namespace plugins {

class WeatherPlugin final : public dashboard::PluginBase {
  public:
    WeatherPlugin();

    uint32_t refreshIntervalMs() const override;

    /// Applied live from Settings. Safe from the LVGL thread while a fetch is in flight.
    ///
    /// A changed latitude or longitude discards the displayed forecast and refetches immediately —
    /// keeping Berlin's temperature on screen under a heading that now says Madrid would be worse
    /// than a moment of dashes. A changed label alone just relabels.
    void setLocation(double latitude, double longitude, const char* label);

  protected:
    esp_err_t onInitialise() override;
    void buildBody(lv_obj_t* body) override;
    esp_err_t fetch(bool force) override;
    void updateUi() override;

  private:
    void buildCurrentBlock(lv_obj_t* parent);
    void buildDetailsCard(lv_obj_t* parent);
    void buildHourStrip(lv_obj_t* parent);

    /// One "name ....... value" line in the details card. Returns the value label.
    lv_obj_t* addDetailRow(lv_obj_t* parent, const char* name, bool separator_above);

    void renderCurrent(const WeatherData& data);
    void renderDetails(const WeatherData& data);
    void renderHours(const WeatherData& data);

    /// Read the cached response written by a previous run and display it. Called once, from
    /// onInitialise(). Never fails the plugin: no cache is the normal first-boot state.
    void loadCachedForecast();

    OpenMeteoProvider provider_;

    /// Guarded by modelMutex(): written by fetch() on the worker, read by updateUi() on the LVGL
    /// thread, and the location is written by setLocation() from whichever task applies settings.
    WeatherData data_;
    double latitude_ = 51.5072;
    double longitude_ = -0.1276;
    dashboard::MediumString label_{"London"};

    // ---- widgets ------------------------------------------------------------------------
    lv_obj_t* place_ = nullptr;
    lv_obj_t* temperature_ = nullptr;
    lv_obj_t* condition_ = nullptr;

    static constexpr size_t kDetailRows = 6;
    lv_obj_t* detail_values_[kDetailRows] = {};

    struct HourChip {
        lv_obj_t* root = nullptr;
        lv_obj_t* time = nullptr;
        lv_obj_t* temperature = nullptr;
        lv_obj_t* sky = nullptr;
        lv_obj_t* rain = nullptr;
    };
    HourChip hours_[WeatherData::kMaxHours];
};

}  // namespace plugins
