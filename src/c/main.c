#include <pebble.h>
#include <ctype.h>
#include <string.h>

// ---- Persistent storage keys ----
#define PKEY_TEMP             1
#define PKEY_ICON             2
#define PKEY_TEMP_UNITS       3   // 0 = Celsius, 1 = Fahrenheit
#define PKEY_NIGHT_MODE       4
#define PKEY_NIGHT_START      5
#define PKEY_NIGHT_END        6
#define PKEY_NIGHT_UPDATE_INT 7
#define PKEY_WEATHER_INT      8
#define PKEY_BACKLIGHT_COLOR  9
#define PKEY_WEATHER_FETCH_TS 10
#define PKEY_BAT_LAST_PCT     11
#define PKEY_BAT_LAST_TS      12
#define PKEY_BAT_EWMA_MILLI   13
#define PKEY_BAT_EWMA_INIT    14
#define PKEY_BAT_LAST_CHARGE  15
#define PKEY_BAT_CHARGE_PCT   16
#define PKEY_BAT_POWERED      17

// ---- Config defaults ----
#define DEFAULT_TEMP_UNITS 0

// ---- Health goal fallbacks ----
#define FALLBACK_STEP_GOAL 10000
#define FALLBACK_DIST_GOAL 8000   // meters

// ---- Active (seconds badge) tuning ----
#define ACTIVE_TIMEOUT_MS 8000

// ---- Weather icon enum (must match pkjs index.js) ----
enum {
  ICON_CLEAR = 0,
  ICON_CLOUDS,
  ICON_RAIN,
  ICON_SNOW,
  ICON_THUNDER,
  ICON_FOG,
  ICON_COUNT
};

static Window *s_window;
static Layer *s_canvas;

static GFont s_font_rc;       // Khand Medium 24 (battery %, temp, date day)
static GFont s_font_col;      // Khand Medium 20 (right-column metrics, badge)
static GFont s_font_sm;       // Khand Regular 18 (date weekday/month)
static GFont s_font_time;     // Rajdhani Bold 66 (big time)

static GBitmap *s_wx_bitmaps[ICON_COUNT];
static GBitmap *s_status_bt_off;

// ---- Live state ----
static char s_hh_buf[3];      // "15"
static char s_mm_buf[3];      // "27"
static char s_time_buf[6];    // "15:27"
static char s_secs_buf[3];    // "30"
static char s_wday_buf[12];   // "MONDAY"
static char s_mday_buf[3];    // "12"
static char s_mon_buf[12];    // "JANUARY"
static char s_batt_buf[8];    // "95 %"
static char s_temp_buf[8];    // "-17°"
static char s_dist_buf[16];   // "8.22 km"
static char s_steps_buf[10];  // "13309"
static char s_hr_buf[6];      // "166"

static int  s_batt_pct = 0;
static bool s_charging = false;
static bool s_connected = false;
static bool s_quiet_time = false;

static int  s_steps = 0;
static int  s_dist_m = 0;
static int  s_hr = 0;
static int  s_step_goal = FALLBACK_STEP_GOAL;
static int  s_dist_goal = FALLBACK_DIST_GOAL;

static int  s_wx_icon = ICON_RAIN;
static int  s_temp_units = DEFAULT_TEMP_UNITS;

static bool s_active = false;     // seconds badge visible
static AppTimer *s_active_timer = NULL;

// ---- Configuration (mirrored on the phone via the Clay settings page) ----
static bool s_night_mode_enabled  = false;   // default: OFF (user opts in)
static int  s_night_start_hour    = 0;       // 00:00 = midnight
static int  s_night_end_hour      = 6;       // 06:00 = 6 AM
static int  s_night_update_interval_min = 5;
static int  s_weather_interval_min = 30;

// Backlight colour (Pebble Time 2 / Emery — RGB backlight LED).
// System Default leaves the watch's own backlight colour untouched.
typedef enum {
  BacklightSystem   = 0,
  BacklightWhite    = 1,
  BacklightYInMn    = 2,
  BacklightRed      = 3,
  BacklightAmber    = 4,
  BacklightYellow   = 5,
  BacklightGreen    = 6,
  BacklightColorCount
} BacklightColor;
static BacklightColor s_backlight_color = BacklightSystem;

// Packed 0x00RRGGBB values, indexed by BacklightColor. Index 0 (System) unused.
static const uint32_t s_backlight_rgb[BacklightColorCount] = {
  0x000000,   // BacklightSystem  — handled separately, value ignored
  0xFFFFFF,   // BacklightWhite
  0x306AC0,   // BacklightYInMn   — YInMn Blue
  0xFF0000,   // BacklightRed
  0xFFBF00,   // BacklightAmber
  0xFFFF00,   // BacklightYellow
  0x00FF00    // BacklightGreen
};

// Face accent colour tracks the backlight colour so the face matches the
// glow at night: big time, hour ticks, gauges, BT + quiet-time icons.
// System Default keeps the face's own red accent.
static GColor accent_color(void) {
  if (s_backlight_color == BacklightSystem) return GColorRed;
  uint32_t rgb = s_backlight_rgb[s_backlight_color];
  return GColorFromRGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

// Apply the configured backlight colour. The override only lasts while the
// app is foregrounded, so this is called on launch and on every settings
// change. On non-RGB-backlight platforms the light_* calls are no-ops.
static void apply_backlight_color(void) {
  if (s_backlight_color == BacklightSystem) {
    light_set_system_color();
  } else {
    light_set_color_rgb888(s_backlight_rgb[s_backlight_color]);
  }
}

// Tracking state for the night-idle gate and the weather refresh schedule
static time_t s_last_tap_time      = 0;
static time_t s_last_weather_fetch = 0;

// Battery life estimator state
// EWMA stored as %/hour × 1000 (fixed-point) to avoid float in persist.
// Sample acceptance: dt_hours in [0.05, 12.0] and dpct > 0.
static uint8_t  s_bat_last_pct       = 0;
static time_t   s_bat_last_ts        = 0;
static int32_t  s_bat_ewma_milli     = 0;     // 0 == uninitialized
static bool     s_bat_ewma_init      = false;
static time_t   s_bat_last_charge_ts = 0;     // Last transition off external power.
static uint8_t  s_bat_charge_pct     = 100;   // Battery % when last unplugged.

static int32_t isqrt64(int64_t x);
static bool is_night_idle(struct tm *now);

static void apply_visual_fixture(void) {
#if defined(VISUAL_FIXTURE)
  strcpy(s_hh_buf, "15");
  strcpy(s_mm_buf, "27");
  strcpy(s_time_buf, "15:27");
  strcpy(s_secs_buf, "30");
  strcpy(s_wday_buf, "MONDAY");
  strcpy(s_mday_buf, "12");
  strcpy(s_mon_buf, "JANUARY");
  strcpy(s_batt_buf, "95 %");
  strcpy(s_temp_buf, "-17°");
  strcpy(s_dist_buf, "8.22 km");
  strcpy(s_steps_buf, "13309");
  strcpy(s_hr_buf, "166");

  s_batt_pct = 95;
  s_charging = true;
#if defined(VISUAL_FIXTURE_BT_OFF)
  s_connected = false;
#else
  s_connected = true;
#endif
#if defined(VISUAL_FIXTURE_QUIET)
  s_quiet_time = true;
#endif
  s_steps = 13309;
  s_dist_m = 8220;
  s_hr = 166;
  s_step_goal = 16000;
  s_dist_goal = 10000;
  s_wx_icon = ICON_RAIN;
  s_active = true;
#endif
}

// =====================================================================
// Drawing helpers
// =====================================================================

static void draw_dashed_gauge(GContext *ctx, int x, int y, int w, int fill_pct,
                              GColor line_color) {
  if (fill_pct < 0) fill_pct = 0;
  if (fill_pct > 100) fill_pct = 100;
  graphics_context_set_stroke_color(ctx, line_color);
  graphics_context_set_stroke_width(ctx, 2);
  const int dash = 5, gap = 4;
  for (int dx = 0; dx < w; dx += dash + gap) {
    int x2 = dx + dash;
    if (x2 > w) x2 = w;
    graphics_draw_line(ctx, GPoint(x + dx, y), GPoint(x + x2, y));
  }
  int dot_x = x + (w * fill_pct) / 100;
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, GPoint(dot_x, y), 4);
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_circle(ctx, GPoint(dot_x, y), 4);
}

static void draw_metric_gauge(GContext *ctx, int x, int y, int w, int fill_pct) {
  if (fill_pct < 0) fill_pct = 0;
  if (fill_pct > 100) fill_pct = 100;

  graphics_context_set_stroke_color(ctx, accent_color());
  graphics_context_set_stroke_width(ctx, 1);
  const int dash = 5, gap = 3;
  for (int dx = 0; dx < w; dx += dash + gap) {
    int x2 = dx + dash;
    if (x2 > w) x2 = w;
    graphics_draw_line(ctx, GPoint(x + dx, y), GPoint(x + x2, y));
  }

  int dot_x = x + (w * fill_pct) / 100;
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, GPoint(dot_x, y), 4);
  graphics_context_set_stroke_color(ctx, GColorDarkGray);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_circle(ctx, GPoint(dot_x, y), 4);
}

// Tick marks around an inset rounded rectangle with consistent perimeter spacing.
static void draw_tick_ring(GContext *ctx, GRect b) {
  graphics_context_set_stroke_color(ctx, GColorLightGray);

  const int left = 10;
  const int top = 10;
  const int right = b.size.w - 10;
  const int bottom = b.size.h - 12;
  const int radius = 24;
  const int straight_w = right - left - 2 * radius;
  const int straight_h = bottom - top - 2 * radius;
  const int corner_len = radius * 157 / 100; // pi/2 approximation
  const int perimeter = 2 * (straight_w + straight_h) + 4 * corner_len;

  for (int i = 0; i < 60; i++) {
    int len = (i % 5 == 0) ? 11 : 6;
    int p = (i * perimeter) / 60;
    int x0, y0, dx, dy;

    if (p < straight_w) {
      x0 = left + radius + p; y0 = top; dx = 0; dy = 1;
    } else if ((p -= straight_w) < corner_len) {
      int32_t a = TRIG_MAX_ANGLE * 3 / 4 + (TRIG_MAX_ANGLE / 4) * p / corner_len;
      x0 = right - radius + (int)(cos_lookup(a) * radius / TRIG_MAX_RATIO);
      y0 = top + radius + (int)(sin_lookup(a) * radius / TRIG_MAX_RATIO);
      dx = (right - radius) - x0; dy = (top + radius) - y0;
    } else if ((p -= corner_len) < straight_h) {
      x0 = right; y0 = top + radius + p; dx = -1; dy = 0;
    } else if ((p -= straight_h) < corner_len) {
      int32_t a = 0 + (TRIG_MAX_ANGLE / 4) * p / corner_len;
      x0 = right - radius + (int)(cos_lookup(a) * radius / TRIG_MAX_RATIO);
      y0 = bottom - radius + (int)(sin_lookup(a) * radius / TRIG_MAX_RATIO);
      dx = (right - radius) - x0; dy = (bottom - radius) - y0;
    } else if ((p -= corner_len) < straight_w) {
      x0 = right - radius - p; y0 = bottom; dx = 0; dy = -1;
    } else if ((p -= straight_w) < corner_len) {
      int32_t a = TRIG_MAX_ANGLE / 4 + (TRIG_MAX_ANGLE / 4) * p / corner_len;
      x0 = left + radius + (int)(cos_lookup(a) * radius / TRIG_MAX_RATIO);
      y0 = bottom - radius + (int)(sin_lookup(a) * radius / TRIG_MAX_RATIO);
      dx = (left + radius) - x0; dy = (bottom - radius) - y0;
    } else if ((p -= corner_len) < straight_h) {
      x0 = left; y0 = bottom - radius - p; dx = 1; dy = 0;
    } else {
      p -= straight_h;
      int32_t a = TRIG_MAX_ANGLE / 2 + (TRIG_MAX_ANGLE / 4) * p / corner_len;
      x0 = left + radius + (int)(cos_lookup(a) * radius / TRIG_MAX_RATIO);
      y0 = top + radius + (int)(sin_lookup(a) * radius / TRIG_MAX_RATIO);
      dx = (left + radius) - x0; dy = (top + radius) - y0;
    }

    int mag = isqrt64((int64_t)dx * dx + (int64_t)dy * dy);
    if (!mag) { mag = 1; }
    int x1 = x0 + dx * len / mag;
    int y1 = y0 + dy * len / mag;

    // Hour ticks take the accent colour; minute ticks stay dim gray so the
    // hour positions keep visual hierarchy.
    graphics_context_set_stroke_color(ctx, (i % 5 == 0) ? accent_color() : GColorLightGray);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, GPoint(x0, y0), GPoint(x1, y1));
  }
}

// --- small vector glyphs (white), ~18px box at (x,y) top-left ---

static void glyph_bt(GContext *ctx, int x, int y) {
  graphics_context_set_stroke_color(ctx, accent_color());
  graphics_context_set_stroke_width(ctx, 1);
  GPoint top = GPoint(x + 6, y);
  GPoint bot = GPoint(x + 6, y + 16);
  graphics_draw_line(ctx, top, bot);
  graphics_draw_line(ctx, top, GPoint(x + 11, y + 4));
  graphics_draw_line(ctx, GPoint(x + 11, y + 4), GPoint(x + 1, y + 12));
  graphics_draw_line(ctx, GPoint(x + 1, y + 4), GPoint(x + 11, y + 12));
  graphics_draw_line(ctx, GPoint(x + 11, y + 12), bot);
}

// Crescent moon for Quiet Time: filled disc with an offset background-colored
// disc carving out the crescent (same trick as pebble-inimal's moon icons).
static void glyph_moon(GContext *ctx, int x, int y) {
  graphics_context_set_fill_color(ctx, accent_color());
  graphics_fill_circle(ctx, GPoint(x + 6, y + 8), 6);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_circle(ctx, GPoint(x + 9, y + 6), 6);
}

static void glyph_bolt(GContext *ctx, int x, int y) {
  graphics_context_set_fill_color(ctx, accent_color());
  GPathInfo info = {
    .num_points = 6,
    .points = (GPoint[]){ {x + 7, y}, {x + 5, y + 7}, {x + 10, y + 7},
                          {x + 3, y + 16}, {x + 5, y + 9}, {x + 0, y + 9} }
  };
  GPath *p = gpath_create(&info);
  gpath_draw_filled(ctx, p);
  gpath_destroy(p);
}

static void draw_status_bitmap(GContext *ctx, GBitmap *bitmap, GRect rect) {
  if (!bitmap) return;
  graphics_context_set_compositing_mode(ctx, GCompOpSet);
  graphics_draw_bitmap_in_rect(ctx, bitmap, rect);
}

static void glyph_pin(GContext *ctx, int x, int y) {
  graphics_context_set_fill_color(ctx, GColorLightGray);
  graphics_fill_circle(ctx, GPoint(x + 6, y + 4), 4);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_circle(ctx, GPoint(x + 6, y + 4), 2);
  graphics_context_set_fill_color(ctx, GColorLightGray);
  GPathInfo info = {
    .num_points = 3,
    .points = (GPoint[]){ {x + 3, y + 7}, {x + 9, y + 7}, {x + 6, y + 13} }
  };
  GPath *p = gpath_create(&info);
  gpath_draw_filled(ctx, p);
  gpath_destroy(p);
}

static void glyph_runner(GContext *ctx, int x, int y) {
  graphics_context_set_stroke_color(ctx, GColorLightGray);
  graphics_context_set_fill_color(ctx, GColorLightGray);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_fill_circle(ctx, GPoint(x + 8, y + 2), 2);
  graphics_draw_line(ctx, GPoint(x + 7, y + 5), GPoint(x + 6, y + 8));
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_line(ctx, GPoint(x + 7, y + 6), GPoint(x + 11, y + 9));
  graphics_draw_line(ctx, GPoint(x + 7, y + 6), GPoint(x + 3, y + 8));
  graphics_draw_line(ctx, GPoint(x + 6, y + 8), GPoint(x + 10, y + 13));
  graphics_draw_line(ctx, GPoint(x + 6, y + 8), GPoint(x + 2, y + 12));
}

static void glyph_heart(GContext *ctx, int x, int y) {
  graphics_context_set_fill_color(ctx, GColorLightGray);
  graphics_fill_circle(ctx, GPoint(x + 4, y + 4), 3);
  graphics_fill_circle(ctx, GPoint(x + 9, y + 4), 3);
  GPathInfo info = {
    .num_points = 3,
    .points = (GPoint[]){ {x + 1, y + 5}, {x + 12, y + 5}, {x + 6, y + 13} }
  };
  GPath *p = gpath_create(&info);
  gpath_draw_filled(ctx, p);
  gpath_destroy(p);
}

// =====================================================================
// Main canvas render
// =====================================================================
static int text_w(const char *s, GFont f) {
  return graphics_text_layout_get_content_size(
      s, f, GRect(0, 0, 200, 90), GTextOverflowModeFill, GTextAlignmentLeft).w;
}

static int32_t isqrt64(int64_t x) {
  int64_t op = x;
  int64_t res = 0;
  int64_t one = 1LL << 62;
  while (one > op) one >>= 2;
  while (one != 0) {
    if (op >= res + one) {
      op -= res + one;
      res += 2 * one;
    }
    res >>= 1;
    one >>= 2;
  }
  return (int32_t)res;
}

// Draw the date row: thin gray weekday + big white day number + thin gray month,
// centered as a group. Mimics "MONDAY 12 JANUARY".
static void draw_date(GContext *ctx, GRect b) {
  const int gap = 6;
  int w_wday = text_w(s_wday_buf, s_font_sm);
  int w_mday = text_w(s_mday_buf, s_font_rc);
  int w_mon  = text_w(s_mon_buf, s_font_sm);
  int total = w_wday + gap + w_mday + gap + w_mon;
  int x = (b.size.w - total) / 2;
  int y_small = 23;
  int y_big = 17;

  graphics_context_set_text_color(ctx, GColorLightGray);
  graphics_draw_text(ctx, s_wday_buf, s_font_sm, GRect(x, y_small, w_wday + 2, 20),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  x += w_wday + gap;
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, s_mday_buf, s_font_rc, GRect(x, y_big, w_mday + 2, 28),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  x += w_mday + gap;
  graphics_context_set_text_color(ctx, GColorLightGray);
  graphics_draw_text(ctx, s_mon_buf, s_font_sm, GRect(x, y_small, w_mon + 2, 20),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
}

static void draw_seconds_badge(GContext *ctx) {
  if (!s_active) return;

  GRect badge = GRect(157, 78, 34, 28);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, badge, 8, GCornersAll);
  graphics_context_set_stroke_color(ctx, GColorLightGray);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_round_rect(ctx, badge, 8);
  graphics_context_set_text_color(ctx, GColorLightGray);
  graphics_draw_text(ctx, s_secs_buf, s_font_col,
                     GRect(badge.origin.x + 1, badge.origin.y + 1, badge.size.w, badge.size.h),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

static void canvas_update(Layer *layer, GContext *ctx) {
  apply_visual_fixture();

  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  draw_tick_ring(ctx, b);

  // Date row
  draw_date(ctx, b);

  // Gauge 1: battery (dot near right when full)
  draw_dashed_gauge(ctx, 62, 48, 78, s_batt_pct, accent_color());

  // Status row: BT + quiet-time moon + charging bolt + battery %, centered
  // as a group
  {
    const int icon_w = 14;
    const int icon_gap = 6;
    const int text_gap = 9;
    int tw = text_w(s_batt_buf, s_font_rc);
    int icon_count = 1 + (s_quiet_time ? 1 : 0) + (s_charging ? 1 : 0);
    int icons_w = icon_count * icon_w + (icon_count - 1) * icon_gap;
    int total = icons_w + text_gap + tw;
    int x = (b.size.w - total) / 2;
    int y = 54;
    if (s_connected) {
      glyph_bt(ctx, x, y + 6);
    } else {
      draw_status_bitmap(ctx, s_status_bt_off, GRect(x + 1, y + 7, 14, 14));
    }
    x += icon_w;
    if (s_quiet_time) {
      x += icon_gap;
      glyph_moon(ctx, x, y + 6);
      x += icon_w;
    }
    if (s_charging)  {
      x += icon_gap;
      glyph_bolt(ctx, x, y + 6);
      x += icon_w;
    }
    x += text_gap;
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, s_batt_buf, s_font_rc, GRect(x, y, tw + 4, 28),
                       GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  }

  // Big time: one centered system numeric font line for a cleaner face.
  // Colour follows the configured backlight colour.
  graphics_context_set_text_color(ctx, accent_color());
  graphics_draw_text(ctx, s_time_buf, s_font_time, GRect(14, 62, 160, 78),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);

  // Seconds badge overlays the right edge of the time in active fixture/state.
  draw_seconds_badge(ctx);

  // Weather icon + temp (bottom-left). Bitmap is drawn at its full 60x60 size
  // (graphics_draw_bitmap_in_rect clips instead of scaling).
  if (s_wx_icon >= 0 && s_wx_icon < ICON_COUNT && s_wx_bitmaps[s_wx_icon]) {
    graphics_context_set_compositing_mode(ctx, GCompOpSet);
    graphics_draw_bitmap_in_rect(ctx, s_wx_bitmaps[s_wx_icon], GRect(28, 124, 60, 60));
  }
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, s_temp_buf, s_font_rc, GRect(44, 178, 60, 28),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);

  // Right column: distance / steps / heart-rate. Rows pitched to fit above the
  // bottom edge, each metric followed by its gauge (HR has none).
  int rx_glyph = 100;
  int rx_text = 116;
  const int gauge_x = rx_glyph + 1;
  const int gauge_w = 62;
  // distance
  glyph_pin(ctx, rx_glyph, 138);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, s_dist_buf, s_font_col, GRect(rx_text, 132, 76, 22),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  draw_metric_gauge(ctx, gauge_x, 158, gauge_w,
                    s_dist_goal ? (s_dist_m * 100 / s_dist_goal) : 0);
  // steps
  glyph_runner(ctx, rx_glyph, 164);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, s_steps_buf, s_font_col, GRect(rx_text, 158, 64, 22),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  draw_metric_gauge(ctx, gauge_x, 184, gauge_w,
                    s_step_goal ? (s_steps * 100 / s_step_goal) : 0);
  // heart rate (no gauge)
  glyph_heart(ctx, rx_glyph, 190);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, s_hr_buf, s_font_col, GRect(rx_text, 184, 54, 22),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
}

// =====================================================================
// Data updates
// =====================================================================
static void update_time_state(struct tm *t) {
  strftime(s_hh_buf, sizeof(s_hh_buf), clock_is_24h_style() ? "%H" : "%I", t);
  strftime(s_mm_buf, sizeof(s_mm_buf), "%M", t);
  snprintf(s_time_buf, sizeof(s_time_buf), "%s:%s", s_hh_buf, s_mm_buf);
  strftime(s_secs_buf, sizeof(s_secs_buf), "%S", t);

  strftime(s_wday_buf, sizeof(s_wday_buf), "%A", t);
  strftime(s_mon_buf, sizeof(s_mon_buf), "%B", t);
  for (char *p = s_wday_buf; *p; p++) *p = toupper((int)*p);
  for (char *p = s_mon_buf; *p; p++) *p = toupper((int)*p);
  snprintf(s_mday_buf, sizeof(s_mday_buf), "%d", t->tm_mday);
}

// ---- Battery life estimator (ported from pebble-inimal) ----
// Update EWMA discharge rate from a fresh battery reading.
// Smoothing factor alpha = 0.2 (numerator 1, denom 5).
static void battery_estimator_update(BatteryChargeState state) {
  time_t now = time(NULL);

  if (state.is_charging || state.is_plugged) {
    // Freeze EWMA while externally powered. Reset only the dt anchor so the
    // first post-power sample doesn't span time spent on the charger.
    s_bat_last_pct = state.charge_percent;
    s_bat_last_ts  = now;
    return;
  }

  if (s_bat_last_ts == 0) {
    // First reading after install: just anchor.
    s_bat_last_pct = state.charge_percent;
    s_bat_last_ts  = now;
    return;
  }

  int32_t dt_sec = (int32_t)(now - s_bat_last_ts);
  int32_t dpct   = (int32_t)s_bat_last_pct - (int32_t)state.charge_percent;

  if (dt_sec <= 0 || dt_sec > 12 * 3600) {
    // Persisted samples can be stale after a long app/watch downtime, and
    // device time can move backward. Drop the old sample and start fresh.
    s_bat_last_pct = state.charge_percent;
    s_bat_last_ts  = now;
    return;
  }

  // Reject: noise floor (<3 min) or no drop yet.
  if (dt_sec < 180 || dpct <= 0) {
    if (dpct < 0) {
      // Battery rose without is_charging set (firmware glitch). Re-anchor.
      s_bat_last_pct = state.charge_percent;
      s_bat_last_ts  = now;
    }
    return;
  }

  // rate_milli = dpct * 1000 * 3600 / dt_sec  (avoids float)
  int32_t rate_milli = (dpct * 3600 * 1000) / dt_sec;

  if (!s_bat_ewma_init) {
    s_bat_ewma_milli = rate_milli;
    s_bat_ewma_init  = true;
  } else {
    // EWMA: new = alpha*rate + (1-alpha)*old, alpha = 1/5
    s_bat_ewma_milli = (rate_milli + 4 * s_bat_ewma_milli) / 5;
  }

  s_bat_last_pct = state.charge_percent;
  s_bat_last_ts  = now;
}

static int32_t battery_since_charge_rate_milli(void) {
  if (s_charging || s_bat_last_charge_ts == 0) {
    return 0;
  }

  time_t now = time(NULL);
  int32_t dt_sec = (int32_t)(now - s_bat_last_charge_ts);
  if (dt_sec < 180) {
    return 0;
  }

  int32_t start_pct = s_bat_charge_pct > 0 ? s_bat_charge_pct : 100;
  int32_t dpct = start_pct - (int32_t)s_batt_pct;
  if (dpct <= 0) {
    return 0;
  }

  return (dpct * 3600 * 1000 + dt_sec / 2) / dt_sec;
}

// Shared battery duration formatting. Produces "Xd Yh" or "Yh Zm".
static void battery_duration_format(int32_t total_min, char *buf, size_t n) {
  if (total_min < 0) total_min = 0;
  int days  = total_min / (60 * 24);
  int hours = (total_min / 60) % 24;
  int mins  = total_min % 60;

  if (days > 0) {
    snprintf(buf, n, "%dd %dh", days, hours);
  } else {
    snprintf(buf, n, "%dh %dm", hours, mins);
  }
}

// Format current battery-life estimate into buf.
// Produces "charging", "—", "Xd Yh", or "Yh Zm".
static void battery_estimator_format(char *buf, size_t n) {
  if (s_charging) {
    snprintf(buf, n, "charging");
    return;
  }

  int32_t rate_milli = battery_since_charge_rate_milli();
  if (rate_milli <= 0 && s_bat_ewma_init && s_bat_ewma_milli > 0) {
    rate_milli = s_bat_ewma_milli;
  }
  if (rate_milli <= 0) {
    snprintf(buf, n, "—");
    return;
  }
  // total_minutes = pct * 60 * 1000 / rate_milli
  int32_t total_min = ((int32_t)s_batt_pct * 60 * 1000) / rate_milli;
  battery_duration_format(total_min, buf, n);
}

static void battery_since_last_charge_format(char *buf, size_t n) {
  if (s_charging) {
    snprintf(buf, n, "charging");
    return;
  }
  if (s_bat_last_charge_ts == 0) {
    snprintf(buf, n, "—");
    return;
  }
  time_t now = time(NULL);
  int32_t total_min = (int32_t)((now - s_bat_last_charge_ts) / 60);
  battery_duration_format(total_min, buf, n);
}

static void update_battery(BatteryChargeState st) {
  bool was_powered = s_charging;
  bool is_powered = st.is_charging || st.is_plugged;

  battery_estimator_update(st);
  s_batt_pct = st.charge_percent;
  if (was_powered && !is_powered) {
    s_bat_last_charge_ts = time(NULL);
    s_bat_charge_pct = st.charge_percent;
    persist_write_int(PKEY_BAT_LAST_CHARGE, (int)s_bat_last_charge_ts);
    persist_write_int(PKEY_BAT_CHARGE_PCT, (int)s_bat_charge_pct);
  }
  if (was_powered != is_powered) {
    persist_write_bool(PKEY_BAT_POWERED, is_powered);
  }
  s_charging = is_powered;
  snprintf(s_batt_buf, sizeof(s_batt_buf), "%d %%", s_batt_pct);
  if (s_canvas) layer_mark_dirty(s_canvas);
}

static void update_connection(bool connected) {
  s_connected = connected;
  layer_mark_dirty(s_canvas);
}

// Quiet Time has no subscription API — poll on ticks and taps and redraw
// only when the state flips.
static void refresh_quiet_time(void) {
  bool active = quiet_time_is_active();
  if (active != s_quiet_time) {
    s_quiet_time = active;
    if (s_canvas) layer_mark_dirty(s_canvas);
  }
}

static void update_temp_text(void) {
  int t = persist_exists(PKEY_TEMP) ? persist_read_int(PKEY_TEMP) : 0;
  int shown = (s_temp_units == 1) ? (t * 9 / 5 + 32) : t;   // C stored; convert if F
  snprintf(s_temp_buf, sizeof(s_temp_buf), "%d°", shown);   // no unit letter, like reference
}

#if defined(PBL_HEALTH)
static int health_goal(HealthMetric metric, int fallback) {
  time_t start = time_start_of_today();
  time_t end = start + SECONDS_PER_DAY;
  HealthServiceAccessibilityMask acc =
      health_service_metric_averaged_accessible(metric, start, end,
                                                HealthServiceTimeScopeDaily);
  if (acc & HealthServiceAccessibilityMaskAvailable) {
    HealthValue v = health_service_aggregate_averaged(
        metric, start, end, HealthAggregationSum, HealthServiceTimeScopeDaily);
    if (v > 0) return (int)v;
  }
  return fallback;
}

static void update_health(void) {
  time_t start = time_start_of_today();
  time_t now = time(NULL);

  if (health_service_metric_accessible(HealthMetricStepCount, start, now)
      & HealthServiceAccessibilityMaskAvailable) {
    s_steps = (int)health_service_sum_today(HealthMetricStepCount);
  }
  if (health_service_metric_accessible(HealthMetricWalkedDistanceMeters, start, now)
      & HealthServiceAccessibilityMaskAvailable) {
    s_dist_m = (int)health_service_sum_today(HealthMetricWalkedDistanceMeters);
  }
  if (health_service_metric_accessible(HealthMetricHeartRateBPM, now - 60, now)
      & HealthServiceAccessibilityMaskAvailable) {
    s_hr = (int)health_service_peek_current_value(HealthMetricHeartRateBPM);
  }

  s_step_goal = health_goal(HealthMetricStepCount, FALLBACK_STEP_GOAL);
  s_dist_goal = health_goal(HealthMetricWalkedDistanceMeters, FALLBACK_DIST_GOAL);

  snprintf(s_steps_buf, sizeof(s_steps_buf), "%d", s_steps);
  snprintf(s_hr_buf, sizeof(s_hr_buf), "%d", s_hr);
  int km_whole = s_dist_m / 1000;
  int km_frac = (s_dist_m % 1000) / 10;
  snprintf(s_dist_buf, sizeof(s_dist_buf), "%d.%02d km", km_whole, km_frac);
}

static void health_handler(HealthEventType type, void *ctx) {
  // Slow health updates during night idle: skip the query and redraw; the
  // next visible tick (or a wake tap) refreshes the stats.
  time_t now_t = time(NULL);
  struct tm *now = localtime(&now_t);
  if (now && is_night_idle(now)) return;
  update_health();
  layer_mark_dirty(s_canvas);
}
#else
static void update_health(void) {
  snprintf(s_steps_buf, sizeof(s_steps_buf), "%d", s_steps);
  snprintf(s_hr_buf, sizeof(s_hr_buf), "%d", s_hr);
  snprintf(s_dist_buf, sizeof(s_dist_buf), "0.00 km");
}
#endif

// =====================================================================
// Tick + active/seconds badge + night idle
// =====================================================================

// Night-idle = setting on AND current hour falls inside the user's night
// window AND no accel taps for 20 minutes. Window can wrap past midnight
// (e.g. 22:00–06:00 means 22..23 OR 0..5).
static bool hour_in_night_window(int hour, int start, int end) {
  if (start == end)  return false;          // empty window
  if (start <  end)  return hour >= start && hour < end;
  return hour >= start || hour < end;       // wrap-around
}

static bool is_valid_night_update_interval(int minutes) {
  return minutes == 3 || minutes == 5 || minutes == 10 || minutes == 15;
}

static bool is_night_idle(struct tm *now) {
  if (!s_night_mode_enabled) return false;
  if (!hour_in_night_window(now->tm_hour, s_night_start_hour, s_night_end_hour)) {
    return false;
  }
  time_t current = time(NULL);
  if ((current - s_last_tap_time) < 20 * 60) return false;
  return true;
}

static void tick_handler(struct tm *t, TimeUnits units) {
  refresh_quiet_time();
  bool night_idle = is_night_idle(t);

  // Skip periodic display work between configured night-idle ticks. Never
  // gate while the seconds badge is active (a tap just woke us, so the
  // 20-minute idle test fails anyway — this is belt and braces).
  if (night_idle && !s_active && (t->tm_min % s_night_update_interval_min != 0)) {
    return;
  }

  update_time_state(t);
  layer_mark_dirty(s_canvas);

  if (units & MINUTE_UNIT) {
    // Time-based weather refresh: respects the user-configured interval and
    // works correctly for intervals longer than an hour. Skipped entirely
    // while the phone is disconnected — a queued send would only burn radio
    // time failing; the first connected tick past the interval fetches.
    // Paused during night idle.
    time_t now = time(NULL);
    if (!night_idle && s_connected &&
        now - s_last_weather_fetch >= s_weather_interval_min * 60) {
      DictionaryIterator *iter;
      if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
        dict_write_uint8(iter, MESSAGE_KEY_REQUEST_WEATHER, 1);
        if (app_message_outbox_send() == APP_MSG_OK) {
          s_last_weather_fetch = now;
        }
      }
    }
  }
}

static void go_passive(void *data) {
  s_active = false;
  s_active_timer = NULL;
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  layer_mark_dirty(s_canvas);
}

static void go_active(void) {
  s_active = true;
  tick_timer_service_subscribe(SECOND_UNIT, tick_handler);
  if (s_active_timer) app_timer_cancel(s_active_timer);
  s_active_timer = app_timer_register(ACTIVE_TIMEOUT_MS, go_passive, NULL);
  // refresh seconds immediately
  time_t now = time(NULL);
  update_time_state(localtime(&now));
  layer_mark_dirty(s_canvas);
}

static void tap_handler(AccelAxisType axis, int32_t direction) {
  // Record this tap so the night-idle gate knows the wrist just moved.
  // go_active() refreshes the time immediately, so a display that was up to
  // <night interval> minutes stale corrects the moment the user looks.
  s_last_tap_time = time(NULL);
  refresh_quiet_time();
  go_active();
}

// =====================================================================
// AppMessage (weather + settings from the Clay config page)
// =====================================================================
static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *t_temp = dict_find(iter, MESSAGE_KEY_TEMP);
  Tuple *t_icon = dict_find(iter, MESSAGE_KEY_ICON);
  Tuple *t_units = dict_find(iter, MESSAGE_KEY_TEMP_UNITS);

  if (t_units) {
    s_temp_units = t_units->value->int32 ? 1 : 0;
    persist_write_int(PKEY_TEMP_UNITS, s_temp_units);
  }
  if (t_temp) {
    persist_write_int(PKEY_TEMP, t_temp->value->int32); // Celsius
  }
  if (t_icon) {
    int icon = t_icon->value->int32;
    if (icon < 0 || icon >= ICON_COUNT) icon = ICON_CLOUDS;
    s_wx_icon = icon;
    persist_write_int(PKEY_ICON, s_wx_icon);
  }

  // Settings from the configuration page
  Tuple *night = dict_find(iter, MESSAGE_KEY_NIGHT_MODE_ENABLED);
  if (night) {
    s_night_mode_enabled = (night->value->int32 != 0);
    persist_write_bool(PKEY_NIGHT_MODE, s_night_mode_enabled);
    APP_LOG(APP_LOG_LEVEL_INFO, "Night mode: %d", s_night_mode_enabled);
  }
  Tuple *nstart = dict_find(iter, MESSAGE_KEY_NIGHT_START_HOUR);
  if (nstart) {
    int v = (int)nstart->value->int32;
    if (v >= 0 && v <= 23) {
      s_night_start_hour = v;
      persist_write_int(PKEY_NIGHT_START, s_night_start_hour);
      APP_LOG(APP_LOG_LEVEL_INFO, "Night start: %d:00", v);
    }
  }
  Tuple *nend = dict_find(iter, MESSAGE_KEY_NIGHT_END_HOUR);
  if (nend) {
    int v = (int)nend->value->int32;
    if (v >= 0 && v <= 23) {
      s_night_end_hour = v;
      persist_write_int(PKEY_NIGHT_END, s_night_end_hour);
      APP_LOG(APP_LOG_LEVEL_INFO, "Night end: %d:00", v);
    }
  }
  Tuple *night_interval = dict_find(iter, MESSAGE_KEY_NIGHT_UPDATE_INTERVAL);
  if (night_interval) {
    int v = (int)night_interval->value->int32;
    if (is_valid_night_update_interval(v)) {
      s_night_update_interval_min = v;
      persist_write_int(PKEY_NIGHT_UPDATE_INT, s_night_update_interval_min);
      APP_LOG(APP_LOG_LEVEL_INFO, "Night update interval: %d min", v);
    }
  }
  Tuple *interval = dict_find(iter, MESSAGE_KEY_WEATHER_INTERVAL);
  if (interval) {
    int v = (int)interval->value->int32;
    if (v >= 5 && v <= 720) {            // sanity bounds
      s_weather_interval_min = v;
      persist_write_int(PKEY_WEATHER_INT, s_weather_interval_min);
      APP_LOG(APP_LOG_LEVEL_INFO, "Weather interval: %d min", v);
    }
  }
  Tuple *backlight = dict_find(iter, MESSAGE_KEY_BACKLIGHT_COLOR);
  if (backlight) {
    int v = (int)backlight->value->int32;
    if (v >= 0 && v < BacklightColorCount) {
      s_backlight_color = (BacklightColor)v;
      persist_write_int(PKEY_BACKLIGHT_COLOR, (int)s_backlight_color);
      apply_backlight_color();
      APP_LOG(APP_LOG_LEVEL_INFO, "Backlight colour: %d", v);
    }
  }

  // The config page asks for fresh battery stats before it opens.
  Tuple *bat_req = dict_find(iter, MESSAGE_KEY_REQUEST_BATTERY_INFO);
  if (bat_req) {
    char est[24];
    char since_charge[24];
    battery_estimator_format(est, sizeof(est));
    battery_since_last_charge_format(since_charge, sizeof(since_charge));
    DictionaryIterator *out;
    if (app_message_outbox_begin(&out) == APP_MSG_OK) {
      dict_write_cstring(out, MESSAGE_KEY_BATTERY_ESTIMATE, est);
      dict_write_cstring(out, MESSAGE_KEY_BATTERY_SINCE_CHARGE, since_charge);
      dict_write_int32  (out, MESSAGE_KEY_BATTERY_RATE_MILLI,
                         battery_since_charge_rate_milli());
      app_message_outbox_send();
    }
  }

  update_temp_text();
  layer_mark_dirty(s_canvas);
}

// =====================================================================
// Lifecycle
// =====================================================================
static void load_persisted(void) {
  s_temp_units = persist_exists(PKEY_TEMP_UNITS)
                 ? persist_read_int(PKEY_TEMP_UNITS) : DEFAULT_TEMP_UNITS;
  s_wx_icon = persist_exists(PKEY_ICON) ? persist_read_int(PKEY_ICON) : ICON_RAIN;

  // Configuration (set via the Clay settings page)
  if (persist_exists(PKEY_NIGHT_MODE)) {
    s_night_mode_enabled = persist_read_bool(PKEY_NIGHT_MODE);
  }
  if (persist_exists(PKEY_NIGHT_START)) {
    s_night_start_hour = persist_read_int(PKEY_NIGHT_START);
  }
  if (persist_exists(PKEY_NIGHT_END)) {
    s_night_end_hour = persist_read_int(PKEY_NIGHT_END);
  }
  if (persist_exists(PKEY_NIGHT_UPDATE_INT)) {
    int interval = persist_read_int(PKEY_NIGHT_UPDATE_INT);
    if (is_valid_night_update_interval(interval)) {
      s_night_update_interval_min = interval;
    }
  }
  if (persist_exists(PKEY_WEATHER_INT)) {
    s_weather_interval_min = persist_read_int(PKEY_WEATHER_INT);
  }
  if (persist_exists(PKEY_BACKLIGHT_COLOR)) {
    int c = persist_read_int(PKEY_BACKLIGHT_COLOR);
    if (c >= 0 && c < BacklightColorCount) {
      s_backlight_color = (BacklightColor)c;
    }
  }

  // Restore the last weather-fetch timestamp so a relaunch honors the
  // configured interval instead of firing a fresh fetch immediately.
  if (persist_exists(PKEY_WEATHER_FETCH_TS)) {
    s_last_weather_fetch = (time_t)persist_read_int(PKEY_WEATHER_FETCH_TS);
    // Guard against a clock moved backwards (timezone / manual set):
    // a future timestamp would suppress fetches indefinitely.
    if (s_last_weather_fetch > time(NULL)) s_last_weather_fetch = 0;
  }

  // Battery estimator state across launches.
  if (persist_exists(PKEY_BAT_EWMA_INIT)) {
    s_bat_ewma_init = persist_read_bool(PKEY_BAT_EWMA_INIT);
  }
  if (persist_exists(PKEY_BAT_EWMA_MILLI)) {
    s_bat_ewma_milli = persist_read_int(PKEY_BAT_EWMA_MILLI);
    if (s_bat_ewma_milli <= 0) s_bat_ewma_init = false;
  }
  if (persist_exists(PKEY_BAT_LAST_PCT)) {
    s_bat_last_pct = (uint8_t)persist_read_int(PKEY_BAT_LAST_PCT);
  }
  if (persist_exists(PKEY_BAT_LAST_TS)) {
    s_bat_last_ts = (time_t)persist_read_int(PKEY_BAT_LAST_TS);
  }
  if (persist_exists(PKEY_BAT_LAST_CHARGE)) {
    s_bat_last_charge_ts = (time_t)persist_read_int(PKEY_BAT_LAST_CHARGE);
  }
  if (persist_exists(PKEY_BAT_CHARGE_PCT)) {
    s_bat_charge_pct = (uint8_t)persist_read_int(PKEY_BAT_CHARGE_PCT);
    if (s_bat_charge_pct == 0 || s_bat_charge_pct > 100) s_bat_charge_pct = 100;
  }
  if (persist_exists(PKEY_BAT_POWERED)) {
    // Restore powered state so an unplug that happened while the watchface
    // wasn't running is still detected as a charge transition.
    s_charging = persist_read_bool(PKEY_BAT_POWERED);
  }

  update_temp_text();
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);

  s_font_rc = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_KHAND_MEDIUM_24));
  s_font_col = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_KHAND_MEDIUM_20));
  s_font_sm = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_KHAND_REGULAR_18));
  s_font_time = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_RAJDHANI_BOLD_66));

  for (int i = 0; i < ICON_COUNT; i++) s_wx_bitmaps[i] = NULL;
  s_wx_bitmaps[ICON_CLEAR]   = gbitmap_create_with_resource(RESOURCE_ID_WX_CLEAR);
  s_wx_bitmaps[ICON_CLOUDS]  = gbitmap_create_with_resource(RESOURCE_ID_WX_CLOUDS);
  s_wx_bitmaps[ICON_RAIN]    = gbitmap_create_with_resource(RESOURCE_ID_WX_RAIN);
  s_wx_bitmaps[ICON_SNOW]    = gbitmap_create_with_resource(RESOURCE_ID_WX_SNOW);
  s_wx_bitmaps[ICON_THUNDER] = gbitmap_create_with_resource(RESOURCE_ID_WX_THUNDER);
  s_wx_bitmaps[ICON_FOG]     = gbitmap_create_with_resource(RESOURCE_ID_WX_FOG);
  s_status_bt_off = gbitmap_create_with_resource(RESOURCE_ID_STATUS_BT_OFF);

  s_canvas = layer_create(b);
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);
}

static void window_unload(Window *window) {
  layer_destroy(s_canvas);
  for (int i = 0; i < ICON_COUNT; i++) {
    if (s_wx_bitmaps[i]) gbitmap_destroy(s_wx_bitmaps[i]);
  }
  if (s_status_bt_off) gbitmap_destroy(s_status_bt_off);
  fonts_unload_custom_font(s_font_rc);
  fonts_unload_custom_font(s_font_col);
  fonts_unload_custom_font(s_font_sm);
  fonts_unload_custom_font(s_font_time);
}

static void init(void) {
  load_persisted();

  // Treat startup as a recent "tap" so we don't immediately enter night-idle
  // (e.g., during a watch reboot at 3am).
  s_last_tap_time = time(NULL);

  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  // Backlight override only persists while foregrounded — re-apply on launch.
  apply_backlight_color();

  time_t now = time(NULL);
  update_time_state(localtime(&now));
  update_battery(battery_state_service_peek());
  update_connection(connection_service_peek_pebble_app_connection());
  refresh_quiet_time();
  update_health();

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  battery_state_service_subscribe(update_battery);
  connection_service_subscribe((ConnectionHandlers){
    .pebble_app_connection_handler = update_connection,
  });
#if defined(PBL_HEALTH)
  health_service_events_subscribe(health_handler, NULL);
#endif
  accel_tap_service_subscribe(tap_handler);

  app_message_register_inbox_received(inbox_received);
  app_message_open(app_message_inbox_size_maximum(), app_message_outbox_size_maximum());
}

static void deinit(void) {
  // Battery estimator + weather schedule state changes too often to persist
  // inline; write it once on exit (settings persist inline on receipt).
  persist_write_int (PKEY_WEATHER_FETCH_TS, (int)s_last_weather_fetch);
  persist_write_int (PKEY_BAT_LAST_PCT,   (int)s_bat_last_pct);
  persist_write_int (PKEY_BAT_LAST_TS,    (int)s_bat_last_ts);
  persist_write_int (PKEY_BAT_EWMA_MILLI, (int)s_bat_ewma_milli);
  persist_write_bool(PKEY_BAT_EWMA_INIT,  s_bat_ewma_init);
  persist_write_int (PKEY_BAT_LAST_CHARGE, (int)s_bat_last_charge_ts);
  persist_write_int (PKEY_BAT_CHARGE_PCT,  (int)s_bat_charge_pct);
  persist_write_bool(PKEY_BAT_POWERED,     s_charging);

  tick_timer_service_unsubscribe();
  battery_state_service_unsubscribe();
  connection_service_unsubscribe();
#if defined(PBL_HEALTH)
  health_service_events_unsubscribe();
#endif
  accel_tap_service_unsubscribe();
  if (s_window) window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
