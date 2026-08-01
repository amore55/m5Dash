// The common chrome every page wears: a thin header, a body, a thin footer.
//
// Plugins never build their own header or footer. They receive body() and fill it. That is what
// makes six independently-written pages look like one product, and it is why the status dot and
// the "last updated" line behave identically everywhere.
//
// Sizes are deliberately small (56 px header, 40 px footer out of 720) because the brief asks
// for full-screen content and explicitly rules out large navigation bars. Navigation is by
// gesture; the footer only *indicates* position, it is not a control surface.

#pragma once

#include "lvgl.h"

#include "dashboard/plugin.hpp"

namespace dashboard {

class PageScaffold {
  public:
    /// Build the chrome inside `parent` (a full-screen container owned by PageManager).
    /// Safe to call once per plugin; calling twice would leak the first tree.
    void build(lv_obj_t* parent, const char* title_text);

    bool built() const { return root_ != nullptr; }

    /// Where the plugin puts its content. Full width, takes all remaining vertical space,
    /// already inset by the page gutter.
    lv_obj_t* body() const { return body_; }

    lv_obj_t* root() const { return root_; }

    void setTitle(const char* text);

    /// Drives the header dot's colour. Pair with setFooterStatus() so colour and words agree.
    void setState(DataState state);

    /// Left-hand footer text: "Updated 3 min ago", "Offline — data from 12:40", an error.
    void setFooterText(const char* text);
    void setFooterColour(lv_color_t colour);

    /// Header clock, right-aligned. Pass nullptr or "" to hide it.
    void setHeaderClock(const char* text);

    /// Small Wi-Fi glyph next to the header clock. Hidden entirely when online, because a
    /// permanently-lit "connected" icon is decoration; only the problem state earns pixels.
    void setOffline(bool offline);

  private:
    lv_obj_t* root_ = nullptr;
    lv_obj_t* header_ = nullptr;
    lv_obj_t* status_dot_ = nullptr;
    lv_obj_t* title_ = nullptr;
    lv_obj_t* clock_ = nullptr;
    lv_obj_t* offline_icon_ = nullptr;
    lv_obj_t* body_ = nullptr;
    lv_obj_t* footer_ = nullptr;
    lv_obj_t* footer_text_ = nullptr;
};

}  // namespace dashboard
