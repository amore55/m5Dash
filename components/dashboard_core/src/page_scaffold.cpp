#include "dashboard/page_scaffold.hpp"

#include "app_config.hpp"
#include "dashboard/theme.hpp"

namespace dashboard {
namespace {

/// A transparent, zero-size flex child whose only job is to push what follows to the far edge.
/// Cheaper and more predictable than juggling flex alignment on the container.
lv_obj_t* makeSpacer(lv_obj_t* parent) {
    lv_obj_t* spacer = lv_obj_create(parent);
    theme::makePlain(spacer);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    return spacer;
}

lv_obj_t* makeBar(lv_obj_t* parent, int32_t height) {
    lv_obj_t* bar = lv_obj_create(parent);
    theme::makePlain(bar);
    lv_obj_set_size(bar, LV_PCT(100), height);
    lv_obj_set_style_pad_left(bar, theme::kGutter, LV_PART_MAIN);
    lv_obj_set_style_pad_right(bar, theme::kGutter, LV_PART_MAIN);
    lv_obj_set_style_pad_column(bar, theme::kGapM, LV_PART_MAIN);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return bar;
}

}  // namespace

void PageScaffold::build(lv_obj_t* parent, const char* title_text) {
    if (parent == nullptr || root_ != nullptr) {
        return;
    }

    root_ = parent;
    theme::applyPageRoot(root_);

    // ---- header -----------------------------------------------------------------------
    header_ = makeBar(root_, dash::cfg::kHeaderHeight);
    status_dot_ = theme::makeStatusDot(header_);
    title_ = theme::makeLabel(header_, title_text, theme::fontTitle(), theme::textPrimary());

    makeSpacer(header_);

    offline_icon_ =
        theme::makeLabel(header_, LV_SYMBOL_WARNING, theme::fontLabel(), theme::warn());
    lv_obj_add_flag(offline_icon_, LV_OBJ_FLAG_HIDDEN);

    clock_ = theme::makeLabel(header_, "", theme::fontBody(), theme::textSecondary());

    // ---- body -------------------------------------------------------------------------
    body_ = lv_obj_create(root_);
    theme::makePlain(body_);
    lv_obj_set_width(body_, LV_PCT(100));
    lv_obj_set_flex_grow(body_, 1);
    lv_obj_set_style_pad_left(body_, theme::kGutter, LV_PART_MAIN);
    lv_obj_set_style_pad_right(body_, theme::kGutter, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(body_, theme::kGapS, LV_PART_MAIN);
    lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(body_, theme::kGapM, LV_PART_MAIN);

    // ---- footer -----------------------------------------------------------------------
    //
    // Only the left-hand status text lives here. The page-position indicator is a single
    // shared widget on LVGL's top layer, owned by PageManager — see page_manager.hpp. The
    // footer is left deliberately short so it cannot collide with it.
    footer_ = makeBar(root_, dash::cfg::kFooterHeight);
    footer_text_ = theme::makeLabel(footer_, "", theme::fontMicro(), theme::textMuted());
    lv_label_set_long_mode(footer_text_, LV_LABEL_LONG_DOT);
    lv_obj_set_width(footer_text_, LV_PCT(60));
}

void PageScaffold::setTitle(const char* text) {
    if (title_ != nullptr) {
        lv_label_set_text(title_, text != nullptr ? text : "");
    }
}

void PageScaffold::setState(DataState state) { theme::setStatusDot(status_dot_, state); }

void PageScaffold::setFooterText(const char* text) {
    if (footer_text_ != nullptr) {
        lv_label_set_text(footer_text_, text != nullptr ? text : "");
    }
}

void PageScaffold::setFooterColour(lv_color_t colour) {
    if (footer_text_ != nullptr) {
        lv_obj_set_style_text_color(footer_text_, colour, LV_PART_MAIN);
    }
}

void PageScaffold::setHeaderClock(const char* text) {
    if (clock_ == nullptr) {
        return;
    }
    lv_label_set_text(clock_, text != nullptr ? text : "");
}

void PageScaffold::setOffline(bool offline) {
    if (offline_icon_ == nullptr) {
        return;
    }
    if (offline) {
        lv_obj_remove_flag(offline_icon_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(offline_icon_, LV_OBJ_FLAG_HIDDEN);
    }
}

}  // namespace dashboard
