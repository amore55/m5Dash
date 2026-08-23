// Shared visual language for every page.
//
// The brief's UI direction — "a restrained desk information display, not a mobile phone" — is
// implemented here rather than repeated in each plugin. If a plugin hard-codes a colour or a
// font size, that is a bug: it will drift from every other page and from any future theme
// change.
//
// The palette is a dark, low-chroma set chosen so that the only saturated pixels on screen are
// the ones carrying meaning (a status dot, a disruption warning). Backgrounds are near-black
// but not pure black, so that panel-black and UI-black are distinguishable and the layout
// still reads at a glance.
//
// One deliberate constraint: LVGL's built-in Montserrat range stops at 48 px, which is small
// for a clock read from across a desk. fontHero() therefore returns the 48 px face and callers
// use applyHeroScale() to enlarge it via an LVGL transform. That is slightly soft;
// scripts/generate_fonts.py documents generating a crisp 160 px face from a redistributable
// OFL font, and this is the one place that would need to change to adopt it.

#pragma once

#include <cstdint>

#include "lvgl.h"

#include "dashboard/network_indicator.hpp"
#include "dashboard/plugin.hpp"

namespace dashboard::theme {

// ---------------------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------------------

lv_color_t bg();           ///< Page background. Near-black.
lv_color_t surface();      ///< Card / panel background, one step above the page.
lv_color_t surfaceAlt();   ///< Nested surface, e.g. a row inside a card.
lv_color_t border();       ///< Hairline separators. Never a full-strength line.

lv_color_t textPrimary();    ///< Values and headings.
lv_color_t textSecondary();  ///< Labels and units.
lv_color_t textMuted();      ///< Timestamps, hints, disabled entries.

lv_color_t accent();  ///< Restrained blue. Interactive affordances and the active page dot.
lv_color_t ok();
lv_color_t warn();
lv_color_t error();
lv_color_t stale();  ///< Amber-brown: showing old data, which is neither fine nor broken.

/// Colour that represents a plugin's data state — the single mapping used by both the header
/// status dot and the footer text, so they can never disagree.
lv_color_t forState(DataState state);

/// An LV_SYMBOL_* glyph for a state, for use alongside or instead of the dot.
const char* symbolForState(DataState state);

/// Colour and glyph for the header's network icon.
///
/// Signal strength is carried by colour, because the built-in symbol font has exactly one Wi-Fi
/// glyph and no bar variants. The states that actually change what a person should DO — offline,
/// connecting, setup — get distinct glyphs as well, so the important distinctions do not rest on
/// colour alone.
lv_color_t forNetwork(NetworkIndicator indicator);
const char* symbolForNetwork(NetworkIndicator indicator);

// ---------------------------------------------------------------------------------------
// Typography
//
// Accessors rather than constants so that a missing Kconfig font degrades to the nearest
// available size instead of failing to link.
// ---------------------------------------------------------------------------------------

const lv_font_t* fontHero();     ///< Largest available. Clock digits, headline values.
const lv_font_t* fontDisplay();  ///< ~40 px. Secondary large values.
const lv_font_t* fontTitle();    ///< ~28 px. Page titles, section headings.
const lv_font_t* fontBody();     ///< ~20 px. Body text, list rows.
const lv_font_t* fontLabel();    ///< ~16 px. Field labels, units.
const lv_font_t* fontMicro();    ///< ~14 px. Footer timestamps, hints.

// ---------------------------------------------------------------------------------------
// Spacing. A single scale, so gaps are consistent across pages without measurement.
// ---------------------------------------------------------------------------------------

constexpr int32_t kGutter = 40;  ///< Page edge inset. Generous: this is a 1280 px wide screen.
constexpr int32_t kGapXs = 4;
constexpr int32_t kGapS = 8;
constexpr int32_t kGapM = 16;
constexpr int32_t kGapL = 24;
constexpr int32_t kGapXl = 40;
constexpr int32_t kRadius = 14;
constexpr int32_t kHairline = 1;

/// Diameter of the header status dot and the page-indicator dots.
constexpr int32_t kStatusDotSize = 10;
constexpr int32_t kPageDotSize = 8;
constexpr int32_t kPageDotActiveWidth = 22;  ///< The active dot is a pill, not a bigger circle.

// ---------------------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------------------

/// Strip an object back to a flat, transparent, non-scrolling container. LVGL's defaults give
/// every object a border, radius, padding and scrollbars, which fight this design; almost every
/// container in the app starts here.
void makePlain(lv_obj_t* obj);

/// Full-screen page root: page background, no padding, no scrolling, flex column.
void applyPageRoot(lv_obj_t* obj);

/// A surface-coloured card with radius, hairline border and internal padding.
lv_obj_t* makeCard(lv_obj_t* parent);

/// A transparent flex row or column with no padding — pure layout, no decoration.
lv_obj_t* makeRow(lv_obj_t* parent);
lv_obj_t* makeColumn(lv_obj_t* parent);

/// Label with font and colour applied. `text` may be null for an initially empty label.
lv_obj_t* makeLabel(lv_obj_t* parent, const char* text, const lv_font_t* font, lv_color_t colour);

/// A pill button with a text label, sized for a finger rather than a mouse.
///
/// Height is kTouchTarget, not LV_SIZE_CONTENT: on glass this is the only dimension that
/// matters, and letting the font decide it produces a 24 px target nobody can hit reliably.
/// The returned object is the button; its label is its only child.
lv_obj_t* makeButton(lv_obj_t* parent, const char* text);

/// Selected / unselected appearance for a button used as a segmented toggle.
///
/// Selected is a filled accent; unselected is a hairline outline. Deliberately not a colour
/// change alone — on a dim panel at an angle, fill versus outline survives where hue does not.
void setButtonSelected(lv_obj_t* button, bool selected);

/// A card that is also a tap target: the same surface as makeCard(), plus a pressed state and
/// LV_OBJ_FLAG_CLICKABLE. Used for the summary page's tiles.
///
/// Callers attach their own LV_EVENT_CLICKED handler. The card keeps flex layout, so children
/// are added exactly as they would be to makeCard().
lv_obj_t* makeTapCard(lv_obj_t* parent);

/// Minimum comfortable touch dimension. 64 px on a 1280x720 panel at arm's length.
constexpr int32_t kTouchTarget = 64;

/// A small filled circle used as a status indicator.
lv_obj_t* makeStatusDot(lv_obj_t* parent);
void setStatusDot(lv_obj_t* dot, DataState state);

/// A 1 px horizontal separator spanning the parent's width.
lv_obj_t* makeSeparator(lv_obj_t* parent);

/// Convert a percentage into the fixed-point units applyHeroScale expects.
///
/// Use this rather than a bare number. `applyHeroScale(label, 300)` reads unmistakably like
/// 300 % and is in fact 117 %, which is exactly the mistake the clock plugin shipped with — the
/// digits rendered at 56 px while the comment beside them claimed 144 px.
constexpr int32_t heroScalePercent(int32_t percent) { return percent * 256 / 100; }

/// Enlarge a label beyond the largest available font using an LVGL transform.
///
/// `scale_256` is FIXED-POINT, not a percentage: 256 = 100 %, 768 = 300 %. Prefer
/// heroScalePercent() at every call site.
///
/// The transform scales the rendered glyph bitmap, so the result is soft at large factors.
/// Anything above ~400 % will look obviously blurry; that is the point at which generating a
/// real font (see the file header) becomes worthwhile.
///
/// Note that the transform does NOT change the label's laid-out size: LVGL reserves the
/// untransformed box and draws the enlarged glyphs over it. Parents therefore need padding to
/// absorb the overflow, or the glyphs are clipped.
void applyHeroScale(lv_obj_t* label, int32_t scale_256);

/// Register the palette on LVGL's default theme so that stock widgets (keyboard, message box,
/// slider) inherit the dark scheme instead of appearing in LVGL's default light blue.
void applyGlobalTheme(lv_display_t* display);

}  // namespace dashboard::theme
