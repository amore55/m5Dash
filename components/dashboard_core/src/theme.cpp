#include "dashboard/theme.hpp"

namespace dashboard::theme {
namespace {

// Palette. Hex literals live here and nowhere else in the codebase.
constexpr uint32_t kBg = 0x0B0D10;
constexpr uint32_t kSurface = 0x14181D;
constexpr uint32_t kSurfaceAlt = 0x1B2027;
constexpr uint32_t kBorder = 0x252B33;

constexpr uint32_t kTextPrimary = 0xE8EDF2;
constexpr uint32_t kTextSecondary = 0x8A94A0;
constexpr uint32_t kTextMuted = 0x5A6472;

constexpr uint32_t kAccent = 0x4C9AFF;
constexpr uint32_t kOk = 0x3FB950;
constexpr uint32_t kWarn = 0xD29922;
constexpr uint32_t kError = 0xF85149;
constexpr uint32_t kStale = 0xB08800;

/// Opacity used for hairline borders and separators. A full-strength 1 px line at this
/// contrast reads as harsh; ~55 % is visible without drawing attention.
constexpr lv_opa_t kBorderOpa = 140;

/// Pick the largest Montserrat face that this build actually enabled, so a trimmed-down
/// Kconfig degrades gracefully instead of failing to link.
const lv_font_t* largestAvailable() {
#if LV_FONT_MONTSERRAT_48
    return &lv_font_montserrat_48;
#elif LV_FONT_MONTSERRAT_40
    return &lv_font_montserrat_40;
#elif LV_FONT_MONTSERRAT_36
    return &lv_font_montserrat_36;
#else
    return LV_FONT_DEFAULT;
#endif
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------------------

lv_color_t bg() { return lv_color_hex(kBg); }
lv_color_t surface() { return lv_color_hex(kSurface); }
lv_color_t surfaceAlt() { return lv_color_hex(kSurfaceAlt); }
lv_color_t border() { return lv_color_hex(kBorder); }
lv_color_t textPrimary() { return lv_color_hex(kTextPrimary); }
lv_color_t textSecondary() { return lv_color_hex(kTextSecondary); }
lv_color_t textMuted() { return lv_color_hex(kTextMuted); }
lv_color_t accent() { return lv_color_hex(kAccent); }
lv_color_t ok() { return lv_color_hex(kOk); }
lv_color_t warn() { return lv_color_hex(kWarn); }
lv_color_t error() { return lv_color_hex(kError); }
lv_color_t stale() { return lv_color_hex(kStale); }

lv_color_t forState(DataState state) {
    switch (state) {
        case DataState::Ok:
            return ok();
        case DataState::Loading:
            return accent();
        case DataState::Stale:
            return stale();
        case DataState::Error:
            return error();
        case DataState::Disabled:
            return textMuted();
        case DataState::Idle:
            break;
    }
    return textMuted();
}

const char* symbolForState(DataState state) {
    switch (state) {
        case DataState::Ok:
            return LV_SYMBOL_OK;
        case DataState::Loading:
            return LV_SYMBOL_REFRESH;
        case DataState::Stale:
            return LV_SYMBOL_WARNING;
        case DataState::Error:
            return LV_SYMBOL_CLOSE;
        case DataState::Disabled:
            return LV_SYMBOL_MINUS;
        case DataState::Idle:
            break;
    }
    return LV_SYMBOL_MINUS;
}

lv_color_t forNetwork(NetworkIndicator indicator) {
    switch (indicator) {
        case NetworkIndicator::Strong:
            return ok();
        case NetworkIndicator::Fair:
            // Neutral, not amber: a workable signal is not a problem, and colouring it as one
            // would cry wolf on most desks most of the time.
            return textSecondary();
        case NetworkIndicator::Weak:
            return warn();
        case NetworkIndicator::Connecting:
            return accent();
        case NetworkIndicator::SetupPortal:
            // The one state that wants the user to act, so it gets the interactive colour.
            return accent();
        case NetworkIndicator::Offline:
            break;
    }
    return textMuted();
}

const char* symbolForNetwork(NetworkIndicator indicator) {
    switch (indicator) {
        case NetworkIndicator::Strong:
        case NetworkIndicator::Fair:
        case NetworkIndicator::Weak:
            return LV_SYMBOL_WIFI;
        case NetworkIndicator::Connecting:
            return LV_SYMBOL_REFRESH;
        case NetworkIndicator::SetupPortal:
            return LV_SYMBOL_SETTINGS;
        case NetworkIndicator::Offline:
            break;
    }
    // Not the Wi-Fi glyph dimmed: "no link" should be a different shape, so it is legible
    // without relying on the colour being noticed.
    return LV_SYMBOL_CLOSE;
}

// ---------------------------------------------------------------------------------------
// Typography
// ---------------------------------------------------------------------------------------

const lv_font_t* fontHero() { return largestAvailable(); }

const lv_font_t* fontDisplay() {
#if LV_FONT_MONTSERRAT_40
    return &lv_font_montserrat_40;
#else
    return largestAvailable();
#endif
}

const lv_font_t* fontTitle() {
#if LV_FONT_MONTSERRAT_28
    return &lv_font_montserrat_28;
#elif LV_FONT_MONTSERRAT_24
    return &lv_font_montserrat_24;
#else
    return LV_FONT_DEFAULT;
#endif
}

const lv_font_t* fontBody() {
#if LV_FONT_MONTSERRAT_20
    return &lv_font_montserrat_20;
#elif LV_FONT_MONTSERRAT_18
    return &lv_font_montserrat_18;
#else
    return LV_FONT_DEFAULT;
#endif
}

const lv_font_t* fontLabel() {
#if LV_FONT_MONTSERRAT_16
    return &lv_font_montserrat_16;
#else
    return LV_FONT_DEFAULT;
#endif
}

const lv_font_t* fontMicro() {
#if LV_FONT_MONTSERRAT_14
    return &lv_font_montserrat_14;
#else
    return LV_FONT_DEFAULT;
#endif
}

// ---------------------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------------------

void makePlain(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    // Scrolling is off by default everywhere. A page that genuinely needs a scrollable list
    // re-enables it on that one child, which also keeps the gesture detector's job simple.
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

void applyPageRoot(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    makePlain(obj);
    lv_obj_set_size(obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(obj, bg(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, textPrimary(), LV_PART_MAIN);
    lv_obj_set_style_text_font(obj, fontBody(), LV_PART_MAIN);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
}

lv_obj_t* makeCard(lv_obj_t* parent) {
    lv_obj_t* card = lv_obj_create(parent);
    makePlain(card);
    lv_obj_set_style_bg_color(card, surface(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, kRadius, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, kHairline, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, border(), LV_PART_MAIN);
    lv_obj_set_style_border_opa(card, kBorderOpa, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, kGapL, LV_PART_MAIN);
    return card;
}

lv_obj_t* makeButton(lv_obj_t* parent, const char* text) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_style_radius(button, kRadius, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, kHairline, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(button, kGapL, LV_PART_MAIN);
    lv_obj_set_height(button, kTouchTarget);
    lv_obj_set_width(button, LV_SIZE_CONTENT);

    // A visible pressed state is not decoration here: the fetch it triggers takes a second or
    // two, and without feedback the first thing a user does is press again.
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, accent(), LV_PART_MAIN | LV_STATE_PRESSED);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text != nullptr ? text : "");
    lv_obj_set_style_text_font(label, fontBody(), LV_PART_MAIN);
    lv_obj_center(label);

    setButtonSelected(button, false);
    return button;
}

void setButtonSelected(lv_obj_t* button, bool selected) {
    if (button == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(button, selected ? accent() : surface(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, selected ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, selected ? accent() : border(), LV_PART_MAIN);
    lv_obj_set_style_border_opa(button, selected ? LV_OPA_COVER : kBorderOpa, LV_PART_MAIN);

    lv_obj_t* label = lv_obj_get_child(button, 0);
    if (label != nullptr) {
        lv_obj_set_style_text_color(label, selected ? textPrimary() : textSecondary(),
                                    LV_PART_MAIN);
    }
}

lv_obj_t* makeTapCard(lv_obj_t* parent) {
    lv_obj_t* card = makeCard(parent);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    // Scrolling off by makePlain(); re-assert that a drag on a tile cannot scroll its own
    // content, or a swipe across the summary page fights the tile under the finger.
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, surfaceAlt(), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(card, accent(), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
    return card;
}

lv_obj_t* makeRow(lv_obj_t* parent) {
    lv_obj_t* row = lv_obj_create(parent);
    makePlain(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    return row;
}

lv_obj_t* makeColumn(lv_obj_t* parent) {
    lv_obj_t* col = lv_obj_create(parent);
    makePlain(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_size(col, LV_PCT(100), LV_SIZE_CONTENT);
    return col;
}

lv_obj_t* makeLabel(lv_obj_t* parent, const char* text, const lv_font_t* font,
                    lv_color_t colour) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text != nullptr ? text : "");
    if (font != nullptr) {
        lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(label, colour, LV_PART_MAIN);
    return label;
}

lv_obj_t* makeStatusDot(lv_obj_t* parent) {
    lv_obj_t* dot = lv_obj_create(parent);
    makePlain(dot);
    lv_obj_set_size(dot, kStatusDotSize, kStatusDotSize);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, textMuted(), LV_PART_MAIN);
    return dot;
}

void setStatusDot(lv_obj_t* dot, DataState state) {
    if (dot == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(dot, forState(state), LV_PART_MAIN);
    // Idle is dimmed rather than coloured: nothing has been attempted, so a bright dot would
    // overstate what is known.
    lv_obj_set_style_bg_opa(dot, state == DataState::Idle ? LV_OPA_40 : LV_OPA_COVER,
                            LV_PART_MAIN);
}

lv_obj_t* makeSeparator(lv_obj_t* parent) {
    lv_obj_t* line = lv_obj_create(parent);
    makePlain(line);
    lv_obj_set_size(line, LV_PCT(100), kHairline);
    lv_obj_set_style_bg_color(line, border(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line, kBorderOpa, LV_PART_MAIN);
    return line;
}

void applyHeroScale(lv_obj_t* label, int32_t scale_256) {
    if (label == nullptr || scale_256 <= 0) {
        return;
    }
    // Pivot at the centre so the label grows about its own midpoint and stays where the layout
    // put it; without this it expands down-right from the top-left corner.
    //
    // Expressed as a percentage rather than half the measured width, because this is typically
    // called from buildBody() before layout has run — lv_obj_get_width() would return 0 and the
    // pivot would silently land on the corner. A percentage is resolved at draw time, so it is
    // also correct when the text length changes.
    lv_obj_set_style_transform_pivot_x(label, lv_pct(50), LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(label, lv_pct(50), LV_PART_MAIN);
    lv_obj_set_style_transform_scale_x(label, scale_256, LV_PART_MAIN);
    lv_obj_set_style_transform_scale_y(label, scale_256, LV_PART_MAIN);
}

void applyGlobalTheme(lv_display_t* display) {
    if (display == nullptr) {
        return;
    }
    // Stock LVGL widgets (the on-screen keyboard, message boxes, sliders) pick their colours
    // from the active theme, not from our page styles. Initialising the default theme in dark
    // mode with our accent keeps them consistent without restyling each widget by hand.
    lv_theme_t* theme = lv_theme_default_init(display, accent(), warn(),
                                              /*dark=*/true, fontBody());
    if (theme != nullptr) {
        lv_display_set_theme(display, theme);
    }
}

}  // namespace dashboard::theme
