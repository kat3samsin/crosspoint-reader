# Readest Theme Design Contract

A one-page contract for the Readest theme. Every screen the theme renders MUST honor
these rules so the UI reads as one system. Values live in `ReadestMetrics` / `ThemeMetrics`
(`src/components/themes/readest/ReadestTheme.*`) and `ReadestLayout`
(`src/components/themes/readest/ReadestLayout.h`).

## Type Roles

| Role | Font | Line height | Weight |
|------|------|-------------|--------|
| Row / title text | `UI_12_FONT_ID` | 26px | Bold for titles and the selected row; regular otherwise |
| Subtitle / label / hint | `SMALL_FONT_ID` | 20px | Regular |
| Reader content & cover art | Serif display | (content-driven) | Reserved — never used for chrome |

- `kTitleFontId == UI_12_FONT_ID`, `kSubtitleFontId == kGuideFontId == SMALL_FONT_ID`.
- The serif display face is reserved for reader body text and cover typography. Do NOT
  use it for headers, tabs, menus, lists, hints, or stats.
- Titles and the currently-selected row are **bold**. Everything else is regular weight.

## Spacing

- **4px spacing scale.** All gaps, insets, and padding are multiples of 4px.
- **Side padding: 20px** (`contentSidePadding`) from the physical screen edge to chrome.
- **Interactive inset: 20px** (`kInteractiveInsetX`) — the horizontal inset applied to
  interactive rows/controls inside the padded content column.

## Selection & Emphasis

- **Selection is expressed ONLY as a solid black inversion**: `fillRect` the row/tile in
  black, then draw its text inverted (white-on-black). No outlines, no gray, no dither.
- **Dividers are 1px solid black lines** drawn between resting (unselected) rows.
- **Dither is reserved for texture and disabled rows** — never for selection or focus.
  A disabled row may be dithered; a selected row is always solid black.

## Load-Bearing Metrics

| Metric | Value (px) |
|--------|-----------|
| `headerHeight` | 58 |
| `tabBarHeight` | 46 |
| `listRowHeight` | 48 |
| `listWithSubtitleRowHeight` | 68 |
| `menuRowHeight` | 46 |
| `buttonHintsHeight` | 40 |
| `homeCoverHeight` | 390 |
| `contentSidePadding` | 20 |

These values are load-bearing: layouts (and their unit tests in
`test/readest_theme_layout/`) assume them. Change one only with a matching test update and
a scan of every screen that reads it.
