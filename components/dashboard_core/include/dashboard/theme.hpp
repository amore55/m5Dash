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

/// A small filled circle used as a status indicator.
lv_obj_t* makeStatusDot(lv_obj_t* parent);
void setStatusDot(lv_obj_t* dot, DataState state);

/// A 1 px horizontal separator spanning the parent's width.
lv_obj_t* makeSeparator(lv_obj_t* parent);

/// Enlarge a label beyond the largest available font using an LVGL transform.
/// `scale_256` is fixed-point: 256 = 100 %, 768 = 300 %.
///
/// The transform scales the rendered glyph bitmap, so the result is soft at large factors.
/// Anything above ~400 % will look obviously blurry; that is the point at which generating a
/// real font (see the file header) becomes worthwhile.
void applyHeroScale(lv_obj_t* label, int32_t scale_256);

/// Register the palette on LVGL's default theme so that stock widgets (keyboard, message box,
/// slider) inherit the dark scheme instead of appearing in LVGL's default light blue.
void applyGlobalTheme(lv_display_t* display);

}  // namespace dashboard::theme
