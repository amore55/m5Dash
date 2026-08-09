// Temporary stand-in for a page that has not been written yet.
//
// Satisfies Milestone 2's "placeholder pages for all five plugins": the navigation, theme and
// page lifecycle can be exercised end to end before any integration exists.
//
// Each one is deleted as its real plugin lands. If any of these are still here when the project
// is finished, something has gone wrong.
//
// refreshIntervalMs() returns 0, which PageManager reads as "never schedule this" — including
// the one-off first refresh — so the page stays Idle with an empty footer instead of claiming
// it was "Updated just now" when nothing was ever fetched.

#pragma once

#include "dashboard/plugin_base.hpp"
#include "dashboard/theme.hpp"

namespace dash {

class PlaceholderPlugin final : public dashboard::PluginBase {
  public:
    /// All three strings must have static lifetime — they are stored by pointer.
    PlaceholderPlugin(const char* id, const char* title, const char* description)
        : PluginBase(id, title), description_(description) {}

    uint32_t refreshIntervalMs() const override { return 0; }

  protected:
    void buildBody(lv_obj_t* body) override {
        namespace theme = dashboard::theme;

        lv_obj_t* centre = lv_obj_create(body);
        theme::makePlain(centre);
        lv_obj_set_width(centre, LV_PCT(100));
        lv_obj_set_flex_grow(centre, 1);
        lv_obj_set_flex_flow(centre, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(centre, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(centre, theme::kGapM, LV_PART_MAIN);

        theme::makeLabel(centre, "Not implemented yet", theme::fontDisplay(),
                         theme::textSecondary());

        lv_obj_t* detail =
            theme::makeLabel(centre, description_, theme::fontBody(), theme::textMuted());
        lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(detail, LV_PCT(70));
        lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }

    esp_err_t fetch(bool force) override {
        (void)force;
        return ESP_OK;  // never called: refreshIntervalMs() is 0
    }

    void updateUi() override {}

    /// No network, no worker activity — keep the stack minimal.
    bool requiresNetwork() const override { return false; }
    uint32_t workerStackBytes() const override { return 2560; }

  private:
    const char* description_;
};

}  // namespace dash
