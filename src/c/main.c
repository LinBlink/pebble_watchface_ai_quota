#include <pebble.h>

// Define DEMO_DATA to preload sample values for checking the layout in the
// emulator. Note that the emulator's pkjs overwrites them within a second or
// two — for looking at the weather icons themselves, define ICON_PREVIEW to a
// WxIcon index instead, which ignores the data entirely.

// Pebble Time (basalt) only: 144 x 168, 64 colours. Every coordinate below is
// tuned for that screen, so there is no need for the PBL_IF_* platform dances.
#define SCREEN_W 144

#define ROW_COUNT 4
#define WX_COUNT  3

#define NO_TEMP (-999)

typedef struct {
  const char *label;
  bool is_claude;
} RowSpec;

static const RowSpec ROWS[ROW_COUNT] = {
  { "CL 5H", true  },
  { "CL 7D", true  },
  { "MM 5H", false },
  { "MM 7D", false },
};

static const char *WX_LABELS[WX_COUNT] = { "NOW", "+6H", "+24H" };

// Persistent storage keys
#define PERSIST_PCT(i)     (100 + (i) * 2)
#define PERSIST_RESET(i)   (100 + (i) * 2 + 1)
#define PERSIST_TEMP(i)    (200 + (i) * 2)
#define PERSIST_CODE(i)    (200 + (i) * 2 + 1)
#define PERSIST_LAST_SYNC(p) (320 + (p))  // 0 = claude, 1 = minimax
#define PERSIST_TARGET     301
#define PERSIST_THEME      310
#define PERSIST_TIME_FONT  311
#define PERSIST_ALERTED(i) (400 + (i))

// Buzz once when a quota first crosses this. Persisted per row so a restart
// doesn't re-alert, and cleared when the window resets so the next crossing
// buzzes again.
#define ALERT_PCT 80

// Quota values older than this are shown greyed out: the phone is reachable but
// the numbers behind them are no longer trustworthy.
#define STALE_AFTER_SEC (20 * 60)

static int32_t s_pct[ROW_COUNT];     // -1 = no data yet
static int32_t s_reset[ROW_COUNT];   // absolute UTC epoch seconds of next reset
static int32_t s_temp[WX_COUNT];     // NO_TEMP = no data yet
static int32_t s_code[WX_COUNT];     // WMO weather code
// UTC epoch of the last quota push per provider (0 = claude, 1 = minimax), 0 =
// never. Tracked separately because the two providers fail independently — if
// only one of them is actually landing pushes, its rows must grey out on their
// own instead of being propped up by the other provider's traffic.
static int32_t s_last_sync[2];
static bool s_refresh_pending;       // a refresh was asked for and hasn't landed yet
static int32_t s_target_date;        // local midnight of the countdown target, 0 = unset
static bool s_alerted[ROW_COUNT];    // this row has already buzzed for its current window

// Settings pushed from the phone. s_theme picks THEMES[], s_time_font picks the
// clock face font. Defaults match the phone-side DEFAULTS so a fresh install
// looks right even before the first AppMessage lands.
static int32_t s_theme;              // 0 = light, 1 = dark
static int32_t s_time_font;          // 0 = bitham, 1 = consolas

// Every colour on screen comes from one of these two palettes, so the whole
// face flips with a single value. Accent is the warm "attention" colour (sun,
// countdown, low battery, refresh in flight): yellow pops on dark, orange on
// light. The bolt stays yellow on both, since it always sits on the dark storm
// cloud, and the no-data placeholder / storm cloud / unknown icon are the same
// dark grey in either theme.
typedef struct {
  GColor bg;           // window background
  GColor fg;           // primary text (clock, temps, percentages)
  GColor fg_dim;       // secondary text (date, labels, resets)
  GColor stale;        // quota number when the last sync is too old
  GColor separator;    // 1px rules
  GColor gauge_track;  // empty part of the quota bars
  GColor batt_outline; // battery case
  GColor cloud;        // weather cloud bodies
  GColor fog;          // fog strokes
  GColor snow;         // snow strokes
  GColor accent;       // sun, countdown, low battery, pending refresh
  GColor bolt;         // storm bolt
} Theme;

static const Theme THEMES[2] = {
  { // light
    .bg = GColorWhite,      .fg = GColorBlack,
    .fg_dim = GColorDarkGray, .stale = GColorDarkGray,
    .separator = GColorLightGray, .gauge_track = GColorLightGray,
    .batt_outline = GColorDarkGray, .cloud = GColorDarkGray,
    .fog = GColorDarkGray, .snow = GColorDarkGray,
    .accent = GColorOrange, .bolt = GColorYellow,
  },
  { // dark
    .bg = GColorBlack,      .fg = GColorWhite,
    .fg_dim = GColorLightGray, .stale = GColorLightGray,
    .separator = GColorDarkGray, .gauge_track = GColorDarkGray,
    .batt_outline = GColorLightGray, .cloud = GColorLightGray,
    .fog = GColorLightGray, .snow = GColorWhite,
    .accent = GColorYellow, .bolt = GColorYellow,
  },
};

static Window *s_window;
static GFont s_consolas_font;          // loaded at window_load, freed at unload
static TextLayer *s_time_layer;
static TextLayer *s_date_layer;
static TextLayer *s_countdown_layer;
static TextLayer *s_ampm_layer;
static TextLayer *s_dday_layer;
static TextLayer *s_wx_label_layers[WX_COUNT];
static TextLayer *s_wx_temp_layers[WX_COUNT];
static Layer *s_wx_icons_layer;
static TextLayer *s_separators[2];
static Layer *s_status_layer;
static Layer *s_rows_layer;
static TextLayer *s_label_layers[ROW_COUNT];
static TextLayer *s_pct_layers[ROW_COUNT];
static TextLayer *s_reset_layers[ROW_COUNT];

static char s_time_buf[8];
static char s_date_buf[16];
static char s_countdown_buf[10];
static char s_ampm_buf[6];
static char s_dday_buf[8];
static char s_wx_temp_buf[WX_COUNT][8];
static char s_pct_buf[ROW_COUNT][8];
static char s_reset_buf[ROW_COUNT][10];

// ------------------------------------------------------------------- layout

// BITHAM_34 rather than 42, because the four indicators live in this row's
// corners and 42 does not leave room for them. Size the margins against the
// WIDEST clock, not a typical one: "1" is a narrow glyph, so "11:41" measures
// ~83px while "20:08" measures ~90px — budget from the latter or the battery
// collides with the hour on one minute in ten.
#define TIME_Y        -4
#define TIME_H        40

#define BATT_X        2
#define BATT_Y        3
#define BATT_W        19
#define BATT_H        10
#define BT_X          130
#define BT_Y          2
#define AMPM_X        2
#define AMPM_Y        17
#define AMPM_W        24
#define DDAY_X        119
#define DDAY_Y        17
#define DDAY_W        22
#define CORNER_H      16

#define DATE_Y        36
#define DATE_H        20
#define DATE_X        3
#define DATE_W        90
#define CD_W          48

#define SEP1_Y        57

// Weather: icon and temperature sit side by side on one line per column. The
// old second caption line ("Cloud", "Drizl") cost 15px and was the least
// legible thing on the screen; the icon says the same in less space, and the
// reclaimed height goes to the quota rows below.
#define WX_COL_W      (SCREEN_W / WX_COUNT)
#define WX_LABEL_Y    58
#define WX_LABEL_H    13
#define WX_ICON_Y     74
#define WX_ICON_SZ    22
#define WX_ICON_X     1
#define WX_TEMP_X     23
#define WX_TEMP_W     25
#define WX_TEMP_H     20
#define SEP2_Y        98

// 17px per row leaves 15px of text above a 2px bar. The text sits 1px high of
// the row origin so glyph baselines clear the bar — at y+0 the 14px font's
// baseline lands exactly on it and every label reads as struck through.
#define ROWS_Y        100
#define ROW_H         17
#define BAR_H         2
#define TEXT_DY       (-1)
#define PCT_DY        (-4)
#define R_LABEL_X     3
#define R_LABEL_W     42
#define R_PCT_W       44
#define R_RESET_W     52

// ------------------------------------------------------------ weather icons
//
// Drawn from primitives rather than shipped as images. Eight 22x22 PNGs would
// be eight resources to keep aligned with each other, and each would need its
// own palette; drawing them means the sun is the accent colour and the rain
// blue for free, and the whole set costs no space in the .pbw.

typedef enum {
  WX_SUN, WX_SUN_CLOUD, WX_CLOUD, WX_FOG,
  WX_DRIZZLE, WX_RAIN, WX_SNOW, WX_STORM, WX_UNKNOWN
} WxIcon;

// WMO weather interpretation codes, collapsed to the shapes worth drawing.
static WxIcon wmo_icon(int32_t code) {
  if (code < 0) return WX_UNKNOWN;
  if (code == 0 || code == 1) return WX_SUN;   // clear, mainly clear
  if (code == 2) return WX_SUN_CLOUD;          // partly cloudy
  if (code == 3) return WX_CLOUD;
  if (code == 45 || code == 48) return WX_FOG;
  if (code >= 51 && code <= 57) return WX_DRIZZLE;
  if (code >= 61 && code <= 67) return WX_RAIN;
  if (code >= 71 && code <= 77) return WX_SNOW;
  if (code >= 80 && code <= 82) return WX_RAIN;   // showers read as rain here
  if (code == 85 || code == 86) return WX_SNOW;
  if (code >= 95) return WX_STORM;
  return WX_UNKNOWN;
}

// Ray directions at 1/10 scale. The diagonals are 7 rather than 10 so every
// tip lands on a circle instead of the corners of a square.
static const GPoint SUN_RAYS[8] = {
  {0,-10},{7,-7},{10,0},{7,7},{0,10},{-7,7},{-10,0},{-7,-7}
};

static GPath *s_bolt;
static const GPathInfo BOLT_INFO = {
  .num_points = 6,
  .points = (GPoint []) {{5,0},{0,8},{4,8},{2,13},{9,5},{5,5}}
};

static void draw_sun(GContext *ctx, GPoint c, int r, bool rays) {
  graphics_context_set_fill_color(ctx, THEMES[s_theme].accent);
  graphics_fill_circle(ctx, c, r);
  if (!rays) return;
  graphics_context_set_stroke_color(ctx, THEMES[s_theme].accent);
  for (int i = 0; i < 8; i++) {
    GPoint d = SUN_RAYS[i];
    graphics_draw_line(ctx,
      GPoint(c.x + d.x * (r + 2) / 10, c.y + d.y * (r + 2) / 10),
      GPoint(c.x + d.x * (r + 5) / 10, c.y + d.y * (r + 5) / 10));
  }
}

// Three discs on a slab, in a 22-wide box `h` tall anchored at (x, y).
static void draw_cloud(GContext *ctx, int x, int y, int h, GColor color) {
  int base = y + h - 5;
  graphics_context_set_fill_color(ctx, color);
  graphics_fill_circle(ctx, GPoint(x + 7,  base), 5);
  graphics_fill_circle(ctx, GPoint(x + 12, base - 3), 6);
  graphics_fill_circle(ctx, GPoint(x + 17, base), 4);
  graphics_fill_rect(ctx, GRect(x + 3, base, 15, 5), 0, GCornerNone);
}

// Slanted strokes under a cloud; `len` separates drizzle from rain.
static void draw_rain(GContext *ctx, int x, int y, int len) {
  graphics_context_set_stroke_color(ctx, GColorPictonBlue);
  for (int i = 0; i < 3; i++) {
    int sx = x + 6 + i * 5;
    graphics_draw_line(ctx, GPoint(sx, y), GPoint(sx - 2, y + len));
  }
}

static void draw_snow(GContext *ctx, int x, int y) {
  graphics_context_set_stroke_color(ctx, THEMES[s_theme].snow);
  for (int i = 0; i < 3; i++) {
    int sx = x + 6 + i * 5;
    graphics_draw_line(ctx, GPoint(sx - 2, y + 2), GPoint(sx + 2, y + 2));
    graphics_draw_line(ctx, GPoint(sx, y), GPoint(sx, y + 4));
  }
}

// Staggered horizontal bars — the one condition with no cloud silhouette.
static void draw_fog(GContext *ctx, int x, int y) {
  graphics_context_set_stroke_color(ctx, THEMES[s_theme].fog);
  for (int i = 0; i < 4; i++) {
    int ly = y + 5 + i * 4;
    int inset = (i % 2) ? 5 : 2;
    graphics_draw_line(ctx, GPoint(x + inset, ly), GPoint(x + 20 - inset, ly));
  }
}

static void draw_wx_icon(GContext *ctx, int x, int y, WxIcon icon) {
  switch (icon) {
    case WX_SUN:
      draw_sun(ctx, GPoint(x + 11, y + 11), 6, true);
      break;
    case WX_SUN_CLOUD:
      // Sun first, so the cloud occludes it rather than the other way round.
      draw_sun(ctx, GPoint(x + 15, y + 6), 4, true);
      draw_cloud(ctx, x, y + 7, 14, THEMES[s_theme].cloud);
      break;
    case WX_CLOUD:
      draw_cloud(ctx, x, y + 4, 16, THEMES[s_theme].cloud);
      break;
    case WX_FOG:
      draw_fog(ctx, x, y);
      break;
    case WX_DRIZZLE:
      draw_cloud(ctx, x, y, 14, THEMES[s_theme].cloud);
      draw_rain(ctx, x, y + 16, 3);
      break;
    case WX_RAIN:
      draw_cloud(ctx, x, y, 14, THEMES[s_theme].cloud);
      draw_rain(ctx, x, y + 14, 6);
      break;
    case WX_SNOW:
      draw_cloud(ctx, x, y, 13, THEMES[s_theme].cloud);
      draw_snow(ctx, x, y + 14);
      break;
    case WX_STORM:
      // Darker cloud, so the yellow bolt in front of it carries the contrast.
      draw_cloud(ctx, x, y, 12, GColorDarkGray);
      graphics_context_set_fill_color(ctx, THEMES[s_theme].bolt);
      gpath_move_to(s_bolt, GPoint(x + 7, y + 8));
      gpath_draw_filled(ctx, s_bolt);
      break;
    default:
      graphics_context_set_fill_color(ctx, GColorDarkGray);
      graphics_fill_rect(ctx, GRect(x + 6, y + 10, 10, 2), 0, GCornerNone);
      break;
  }
}

static void wx_icons_update_proc(Layer *layer, GContext *ctx) {
  for (int i = 0; i < WX_COUNT; i++) {
#ifdef ICON_PREVIEW
    WxIcon icon = (WxIcon)((ICON_PREVIEW + i) % (WX_UNKNOWN + 1));
#else
    WxIcon icon = (s_temp[i] == NO_TEMP) ? WX_UNKNOWN : wmo_icon(s_code[i]);
#endif
    draw_wx_icon(ctx, i * WX_COL_W + WX_ICON_X, 0, icon);
  }
}

static GColor row_color(int i) {
  return ROWS[i].is_claude ? GColorOrange : GColorPictonBlue;
}

// ---------------------------------------------------------------- formatting

// Remaining time until reset, kept as short as possible so the font can stay big.
static void fmt_remaining(char *out, size_t out_len, int32_t seconds_left) {
  if (seconds_left <= 0) {
    snprintf(out, out_len, "now");
    return;
  }
  int32_t mins = (seconds_left + 59) / 60;
  if (mins < 60) {
    snprintf(out, out_len, "%ldm", (long)mins);
  } else if (mins < 60 * 24) {
    snprintf(out, out_len, "%ldh%02ld", (long)(mins / 60), (long)(mins % 60));
  } else {
    int32_t hours = mins / 60;
    snprintf(out, out_len, "%ldd%ldh", (long)(hours / 24), (long)(hours % 24));
  }
}

static bool data_is_stale(time_t now, int provider) {
  int32_t last = s_last_sync[provider];
  return last <= 0 || (int32_t)now - last > STALE_AFTER_SEC;
}

static void update_rows(time_t now) {
  for (int i = 0; i < ROW_COUNT; i++) {
    bool stale = data_is_stale(now, ROWS[i].is_claude ? 0 : 1);

    if (s_pct[i] < 0) {
      snprintf(s_pct_buf[i], sizeof(s_pct_buf[i]), "--");
      // The placeholder is the same dark grey in both themes.
      text_layer_set_text_color(s_pct_layers[i], GColorDarkGray);
    } else {
      int32_t p = s_pct[i];
      if (p > 100) p = 100;
      snprintf(s_pct_buf[i], sizeof(s_pct_buf[i]), "%ld%%", (long)p);
      // Grey means "this number may have moved since we last heard from the phone".
      text_layer_set_text_color(s_pct_layers[i],
                                stale ? THEMES[s_theme].stale
                                      : (p >= 90 ? GColorRed : THEMES[s_theme].fg));
    }
    text_layer_set_text(s_pct_layers[i], s_pct_buf[i]);

    if (s_reset[i] <= 0) {
      snprintf(s_reset_buf[i], sizeof(s_reset_buf[i]), "--");
    } else {
      fmt_remaining(s_reset_buf[i], sizeof(s_reset_buf[i]), s_reset[i] - (int32_t)now);
    }
    text_layer_set_text(s_reset_layers[i], s_reset_buf[i]);
  }
  layer_mark_dirty(s_rows_layer);
}

static void update_weather(void) {
  for (int i = 0; i < WX_COUNT; i++) {
    if (s_temp[i] == NO_TEMP) {
      snprintf(s_wx_temp_buf[i], sizeof(s_wx_temp_buf[i]), "--");
    } else {
      snprintf(s_wx_temp_buf[i], sizeof(s_wx_temp_buf[i]), "%ld°", (long)s_temp[i]);
    }
    text_layer_set_text(s_wx_temp_layers[i], s_wx_temp_buf[i]);
  }
  layer_mark_dirty(s_wx_icons_layer);
}

// Minutes until the next 22:00 local, which is what the countdown line shows.
static void update_countdown(struct tm *t) {
  int mins_left = 22 * 60 - (t->tm_hour * 60 + t->tm_min);
  if (mins_left < 0) mins_left += 24 * 60;
  fmt_remaining(s_countdown_buf, sizeof(s_countdown_buf), mins_left * 60);
  text_layer_set_text(s_countdown_layer, s_countdown_buf);
}

// Local midnight of the day `t` falls in.
static time_t local_midnight(time_t t) {
  struct tm tm = *localtime(&t);
  tm.tm_hour = 0;
  tm.tm_min = 0;
  tm.tm_sec = 0;
  return mktime(&tm);
}

// Whole days from today to the target, counted in local calendar days so the
// number ticks over at midnight rather than at the time of day it was set.
static void update_dday(time_t now) {
  if (s_target_date <= 0) {
#ifdef WIDEST_CLOCK
    text_layer_set_text(s_dday_layer, "365");
#else
    text_layer_set_text(s_dday_layer, "");
#endif
    return;
  }
  // Half a day of slack, so a DST shift can't round a whole day off; then floor
  // rather than truncate, which would round the wrong way once the date passes.
  int32_t diff = s_target_date - (int32_t)local_midnight(now) + 43200;
  int days = (diff >= 0) ? (int)(diff / 86400) : -(int)((-diff + 86399) / 86400);
  snprintf(s_dday_buf, sizeof(s_dday_buf), "%d", days);
  text_layer_set_text(s_dday_layer, s_dday_buf);
}

static void update_clock(struct tm *t) {
  strftime(s_time_buf, sizeof(s_time_buf),
           clock_is_24h_style() ? "%H:%M" : "%I:%M", t);
  // Drop a leading zero in 12h mode so the big font has room.
  if (!clock_is_24h_style() && s_time_buf[0] == '0') {
    memmove(s_time_buf, s_time_buf + 1, strlen(s_time_buf));
  }
#ifdef WIDEST_CLOCK
  // Worst-case glyph widths, for checking the corner indicators can't collide.
  snprintf(s_time_buf, sizeof(s_time_buf), "20:08");
#endif
  text_layer_set_text(s_time_layer, s_time_buf);

  // Nothing to disambiguate on a 24h clock, so the corner stays empty there.
  if (clock_is_24h_style()) {
    s_ampm_buf[0] = '\0';
  } else {
    strftime(s_ampm_buf, sizeof(s_ampm_buf), "%p", t);
  }
#ifdef WIDEST_CLOCK
  snprintf(s_ampm_buf, sizeof(s_ampm_buf), "PM");
#endif
  text_layer_set_text(s_ampm_layer, s_ampm_buf);

  strftime(s_date_buf, sizeof(s_date_buf), "%a %m-%d", t);
  text_layer_set_text(s_date_layer, s_date_buf);
}

static void update_ui(void) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  update_clock(t);
  update_countdown(t);
  update_dday(now);
  update_weather();
  update_rows(now);
}

// -------------------------------------------------------------------- render

static void rows_update_proc(Layer *layer, GContext *ctx) {
  // The gauge starts after the label rather than at the screen edge. Run it the
  // full width and it crosses the label's glyphs at exactly baseline height,
  // which reads as a strikethrough; starting it here also lines the gauge up
  // with the number it is describing.
  int bar_x = R_LABEL_X + R_LABEL_W;
  int bar_w = SCREEN_W - R_LABEL_X - bar_x;

  for (int i = 0; i < ROW_COUNT; i++) {
    int y = i * ROW_H + ROW_H - BAR_H;

    graphics_context_set_fill_color(ctx, THEMES[s_theme].gauge_track);
    graphics_fill_rect(ctx, GRect(bar_x, y, bar_w, BAR_H), 0, GCornerNone);

    if (s_pct[i] >= 0) {
      int32_t p = s_pct[i] > 100 ? 100 : s_pct[i];
      // The bar turns red with the number, so the alarm reads even if your eye
      // never reaches the digits.
      graphics_context_set_fill_color(ctx, p >= 90 ? GColorRed : row_color(i));
      graphics_fill_rect(ctx, GRect(bar_x, y, (bar_w * (int)p) / 100, BAR_H), 0, GCornerNone);
    }
  }
}

// Charge as a filled cell rather than a number: charge_percent only moves in
// steps of 10, so the digits would imply a precision the reading doesn't have.
static void draw_battery(GContext *ctx, int x, int y) {
  BatteryChargeState st = battery_state_service_peek();

  graphics_context_set_stroke_color(ctx, THEMES[s_theme].batt_outline);
  graphics_draw_rect(ctx, GRect(x, y, BATT_W, BATT_H));
  graphics_context_set_fill_color(ctx, THEMES[s_theme].batt_outline);
  graphics_fill_rect(ctx, GRect(x + BATT_W, y + 3, 2, 4), 0, GCornerNone);

  GColor fill;
  if (st.is_charging) {
    fill = GColorCyan;
  } else if (st.charge_percent <= 10) {
    fill = GColorRed;
  } else if (st.charge_percent <= 30) {
    fill = THEMES[s_theme].accent;
  } else {
    fill = GColorGreen;
  }
  int inner = BATT_W - 2;
  int w = (inner * st.charge_percent) / 100;
  if (w > 0) {
    graphics_context_set_fill_color(ctx, fill);
    graphics_fill_rect(ctx, GRect(x + 1, y + 1, w, BATT_H - 2), 0, GCornerNone);
  }
}

// The Bluetooth rune as one polyline: the two long diagonals cross the spine,
// which is what makes the shape read at 10px.
static const GPoint BT_PATH[6] = {
  {3,4},{9,10},{6,13},{6,1},{9,4},{3,10}
};

// One glyph, three states — they are all statements about the same phone link,
// so splitting them across two indicators would just cost space. Disconnection
// outranks a pending refresh: while out of range nothing can land anyway.
static void draw_bluetooth(GContext *ctx, int x, int y) {
  GColor color;
  if (!connection_service_peek_pebble_app_connection()) {
    color = GColorRed;
  } else if (s_refresh_pending) {
    color = THEMES[s_theme].accent;
  } else {
    color = GColorPictonBlue;
  }
  graphics_context_set_stroke_color(ctx, color);
  for (int i = 0; i < 5; i++) {
    graphics_draw_line(ctx,
      GPoint(x + BT_PATH[i].x,     y + BT_PATH[i].y),
      GPoint(x + BT_PATH[i + 1].x, y + BT_PATH[i + 1].y));
  }
}

static void status_update_proc(Layer *layer, GContext *ctx) {
  draw_battery(ctx, BATT_X, BATT_Y);
  draw_bluetooth(ctx, BT_X, BT_Y);
}

// A 1px rule: an empty TextLayer with a background is cheaper than another
// custom Layer with its own update_proc.
static TextLayer *separator(Layer *root, int y) {
  TextLayer *tl = text_layer_create(GRect(0, y, SCREEN_W, 1));
  text_layer_set_background_color(tl, THEMES[s_theme].separator);
  layer_add_child(root, text_layer_get_layer(tl));
  return tl;
}

// The clock font is a setting: Bitham 34 (default) or the bundled Consolas-like
// digits. Custom fonts are loaded by handle rather than by FONT_KEY string.
static GFont time_font(void) {
  return s_time_font ? s_consolas_font
                     : fonts_get_system_font(FONT_KEY_BITHAM_34_MEDIUM_NUMBERS);
}

// Push the active palette and clock font into every layer. Idempotent, so it is
// fine to call on every inbox message — the layers were built with the persisted
// theme at window_load, and this only re-colours the ones that changed.
static void apply_theme(void) {
  const Theme *t = &THEMES[s_theme];

  window_set_background_color(s_window, t->bg);
  text_layer_set_text_color(s_time_layer, t->fg);
  text_layer_set_font(s_time_layer, time_font());

  text_layer_set_text_color(s_date_layer, t->fg_dim);
  text_layer_set_text_color(s_countdown_layer, t->accent);
  text_layer_set_text_color(s_ampm_layer, t->fg_dim);
  text_layer_set_text_color(s_dday_layer, t->fg);

  for (int i = 0; i < WX_COUNT; i++) {
    text_layer_set_text_color(s_wx_label_layers[i], t->fg_dim);
    text_layer_set_text_color(s_wx_temp_layers[i], t->fg);
  }

  text_layer_set_background_color(s_separators[0], t->separator);
  text_layer_set_background_color(s_separators[1], t->separator);

  for (int i = 0; i < ROW_COUNT; i++) {
    text_layer_set_text_color(s_label_layers[i], row_color(i));
    text_layer_set_text_color(s_reset_layers[i], t->fg_dim);
  }

  // update_rows() owns the percentage colour (placeholder / stale / normal),
  // so re-run it to pick up the new palette, then redraw the drawn layers.
  update_rows(time(NULL));
  layer_mark_dirty(s_status_layer);
  layer_mark_dirty(s_wx_icons_layer);
}

// ------------------------------------------------------------------ app msg

static void store_int(uint32_t persist_key, int32_t *slot, int32_t value) {
  *slot = value;
  persist_write_int(persist_key, value);
}

static void apply_quota(DictionaryIterator *it, uint32_t pct_key, uint32_t reset_key, int idx) {
  Tuple *p = dict_find(it, pct_key);
  Tuple *r = dict_find(it, reset_key);
  if (p) store_int(PERSIST_PCT(idx), &s_pct[idx], p->value->int32);
  if (r) store_int(PERSIST_RESET(idx), &s_reset[idx], r->value->int32);
}

static void apply_weather(DictionaryIterator *it, uint32_t temp_key, uint32_t code_key, int idx) {
  Tuple *t = dict_find(it, temp_key);
  Tuple *c = dict_find(it, code_key);
  if (t) store_int(PERSIST_TEMP(idx), &s_temp[idx], t->value->int32);
  if (c) store_int(PERSIST_CODE(idx), &s_code[idx], c->value->int32);
}

// Ask the phone to fetch right now instead of waiting for its next poll.
static void request_refresh(void) {
  if (!connection_service_peek_pebble_app_connection()) return;

  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) return;
  dict_write_uint8(out, MESSAGE_KEY_REQUEST_REFRESH, 1);
  if (app_message_outbox_send() == APP_MSG_OK) {
    s_refresh_pending = true;
    layer_mark_dirty(s_status_layer);
  }
}

// Buzz once as a quota crosses ALERT_PCT, and re-arm when the window resets and
// the number drops back. The armed/fired state is persisted, so reinstalling or
// rebooting mid-window doesn't buzz for a threshold already crossed.
static void check_quota_alerts(void) {
  bool crossed = false;

  for (int i = 0; i < ROW_COUNT; i++) {
    if (s_pct[i] < 0) continue;
    bool over = s_pct[i] >= ALERT_PCT;
    if (over == s_alerted[i]) continue;
    if (over) crossed = true;
    s_alerted[i] = over;
    persist_write_bool(PERSIST_ALERTED(i), over);
  }

  // One buzz even if several rows cross in the same push. Quiet time is the
  // user having said "not now", so it wins.
  if (crossed && !quiet_time_is_active()) vibes_double_pulse();
}

static void inbox_received(DictionaryIterator *it, void *context) {
  apply_quota(it, MESSAGE_KEY_CLAUDE_5H_PCT,  MESSAGE_KEY_CLAUDE_5H_RESET,  0);
  apply_quota(it, MESSAGE_KEY_CLAUDE_WK_PCT,  MESSAGE_KEY_CLAUDE_WK_RESET,  1);
  apply_quota(it, MESSAGE_KEY_MINIMAX_5H_PCT, MESSAGE_KEY_MINIMAX_5H_RESET, 2);
  apply_quota(it, MESSAGE_KEY_MINIMAX_WK_PCT, MESSAGE_KEY_MINIMAX_WK_RESET, 3);

  apply_weather(it, MESSAGE_KEY_WX_TEMP_NOW, MESSAGE_KEY_WX_CODE_NOW, 0);
  apply_weather(it, MESSAGE_KEY_WX_TEMP_6H,  MESSAGE_KEY_WX_CODE_6H,  1);
  apply_weather(it, MESSAGE_KEY_WX_TEMP_24H, MESSAGE_KEY_WX_CODE_24H, 2);

  Tuple *target = dict_find(it, MESSAGE_KEY_TARGET_DATE);
  if (target) store_int(PERSIST_TARGET, &s_target_date, target->value->int32);

  // Appearance settings; anything present overrides the persisted value.
  Tuple *theme = dict_find(it, MESSAGE_KEY_THEME);
  if (theme) store_int(PERSIST_THEME, &s_theme, theme->value->int32);
  Tuple *time_font = dict_find(it, MESSAGE_KEY_TIME_FONT);
  if (time_font) store_int(PERSIST_TIME_FONT, &s_time_font, time_font->value->int32);

  // Only a quota push counts as a sync — a weather-only message says nothing
  // about how fresh the percentages are. Tracked per provider: MiniMax landing
  // fine must not paper over Claude silently failing to push, or vice versa.
  time_t now = time(NULL);
  if (dict_find(it, MESSAGE_KEY_CLAUDE_5H_PCT) || dict_find(it, MESSAGE_KEY_CLAUDE_WK_PCT)) {
    store_int(PERSIST_LAST_SYNC(0), &s_last_sync[0], (int32_t)now);
  }
  if (dict_find(it, MESSAGE_KEY_MINIMAX_5H_PCT) || dict_find(it, MESSAGE_KEY_MINIMAX_WK_PCT)) {
    store_int(PERSIST_LAST_SYNC(1), &s_last_sync[1], (int32_t)now);
  }

  s_refresh_pending = false;
  layer_mark_dirty(s_status_layer);
  check_quota_alerts();
  apply_theme();
  update_ui();
}

static void inbox_dropped(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "inbox dropped: %d", (int)reason);
}

static void load_persisted(void) {
  for (int i = 0; i < ROW_COUNT; i++) {
    s_pct[i]   = persist_exists(PERSIST_PCT(i))   ? persist_read_int(PERSIST_PCT(i))   : -1;
    s_reset[i] = persist_exists(PERSIST_RESET(i)) ? persist_read_int(PERSIST_RESET(i)) : 0;
  }
  for (int i = 0; i < WX_COUNT; i++) {
    s_temp[i] = persist_exists(PERSIST_TEMP(i)) ? persist_read_int(PERSIST_TEMP(i)) : NO_TEMP;
    s_code[i] = persist_exists(PERSIST_CODE(i)) ? persist_read_int(PERSIST_CODE(i)) : -1;
  }
  for (int p = 0; p < 2; p++) {
    s_last_sync[p] = persist_exists(PERSIST_LAST_SYNC(p)) ? persist_read_int(PERSIST_LAST_SYNC(p)) : 0;
  }
  s_target_date = persist_exists(PERSIST_TARGET) ? persist_read_int(PERSIST_TARGET) : 0;
  s_theme = persist_exists(PERSIST_THEME) ? persist_read_int(PERSIST_THEME) : 0;
  s_time_font = persist_exists(PERSIST_TIME_FONT) ? persist_read_int(PERSIST_TIME_FONT) : 0;
  for (int i = 0; i < ROW_COUNT; i++) {
    s_alerted[i] = persist_exists(PERSIST_ALERTED(i)) && persist_read_bool(PERSIST_ALERTED(i));
  }
}

// ------------------------------------------------------------------- window

static TextLayer *make_text(Layer *root, GRect frame, GFont font,
                            GTextAlignment align, GColor color) {
  TextLayer *tl = text_layer_create(frame);
  text_layer_set_font(tl, font);
  text_layer_set_text_alignment(tl, align);
  text_layer_set_background_color(tl, GColorClear);
  text_layer_set_text_color(tl, color);
  layer_add_child(root, text_layer_get_layer(tl));
  return tl;
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  window_set_background_color(window, THEMES[s_theme].bg);

  s_consolas_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_CONSOLAS_38));

  s_time_layer = make_text(root, GRect(0, TIME_Y, SCREEN_W, TIME_H),
                           time_font(), GTextAlignmentCenter, THEMES[s_theme].fg);

  s_date_layer = make_text(root, GRect(DATE_X, DATE_Y, DATE_W, DATE_H),
                           fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                           GTextAlignmentLeft, THEMES[s_theme].fg_dim);

  // Countdown to 22:00, in the accent colour so it reads as a deadline rather
  // than a clock (yellow on dark, orange on light).
  s_countdown_layer = make_text(root, GRect(SCREEN_W - CD_W - DATE_X, DATE_Y, CD_W, DATE_H),
                                fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                                GTextAlignmentRight, THEMES[s_theme].accent);

  // Battery and Bluetooth share one layer spanning the top strip, so their
  // coordinates stay screen coordinates and stay comparable with the clock's.
  s_status_layer = layer_create(GRect(0, 0, SCREEN_W, CORNER_H));
  layer_set_update_proc(s_status_layer, status_update_proc);
  layer_add_child(root, s_status_layer);

  s_ampm_layer = make_text(root, GRect(AMPM_X, AMPM_Y, AMPM_W, CORNER_H),
                           fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                           GTextAlignmentLeft, THEMES[s_theme].fg_dim);
  text_layer_set_text(s_ampm_layer, "");

  // Just the number, per the brief — the target date lives in the phone's
  // settings page and is the only place it needs spelling out.
  s_dday_layer = make_text(root, GRect(DDAY_X, DDAY_Y, DDAY_W, CORNER_H),
                           fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                           GTextAlignmentRight, THEMES[s_theme].fg);
  text_layer_set_text(s_dday_layer, "");

  s_separators[0] = separator(root, SEP1_Y);

  s_bolt = gpath_create(&BOLT_INFO);

  for (int i = 0; i < WX_COUNT; i++) {
    int x = i * WX_COL_W;
    s_wx_label_layers[i] = make_text(root, GRect(x, WX_LABEL_Y, WX_COL_W, WX_LABEL_H),
                                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                                     GTextAlignmentCenter, THEMES[s_theme].fg_dim);
    text_layer_set_text(s_wx_label_layers[i], WX_LABELS[i]);

    s_wx_temp_layers[i] = make_text(root,
                                    GRect(x + WX_TEMP_X, WX_ICON_Y + 1, WX_TEMP_W, WX_TEMP_H),
                                    fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                                    GTextAlignmentCenter, THEMES[s_theme].fg);
    text_layer_set_text(s_wx_temp_layers[i], "--");
  }

  s_wx_icons_layer = layer_create(GRect(0, WX_ICON_Y, SCREEN_W, WX_ICON_SZ));
  layer_set_update_proc(s_wx_icons_layer, wx_icons_update_proc);
  layer_add_child(root, s_wx_icons_layer);

  s_separators[1] = separator(root, SEP2_Y);

  s_rows_layer = layer_create(GRect(0, ROWS_Y, SCREEN_W, ROW_H * ROW_COUNT));
  layer_set_update_proc(s_rows_layer, rows_update_proc);
  layer_add_child(root, s_rows_layer);

  for (int i = 0; i < ROW_COUNT; i++) {
    int y = i * ROW_H;

    s_label_layers[i] = make_text(s_rows_layer, GRect(R_LABEL_X, y + TEXT_DY, R_LABEL_W, ROW_H),
                                  fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                                  GTextAlignmentLeft, row_color(i));
    text_layer_set_text(s_label_layers[i], ROWS[i].label);

    s_pct_layers[i] = make_text(s_rows_layer,
                                GRect(R_LABEL_X + R_LABEL_W, y + PCT_DY, R_PCT_W, ROW_H + 4),
                                fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                                GTextAlignmentRight, THEMES[s_theme].fg);
    text_layer_set_text(s_pct_layers[i], "--");

    s_reset_layers[i] = make_text(s_rows_layer,
                                  GRect(R_LABEL_X + R_LABEL_W + R_PCT_W, y + TEXT_DY, R_RESET_W, ROW_H),
                                  fonts_get_system_font(FONT_KEY_GOTHIC_14),
                                  GTextAlignmentRight, THEMES[s_theme].fg_dim);
    text_layer_set_text(s_reset_layers[i], "--");
  }

  update_ui();
}

static void window_unload(Window *window) {
  for (int i = 0; i < ROW_COUNT; i++) {
    text_layer_destroy(s_label_layers[i]);
    text_layer_destroy(s_pct_layers[i]);
    text_layer_destroy(s_reset_layers[i]);
  }
  for (int i = 0; i < WX_COUNT; i++) {
    text_layer_destroy(s_wx_label_layers[i]);
    text_layer_destroy(s_wx_temp_layers[i]);
  }
  layer_destroy(s_wx_icons_layer);
  gpath_destroy(s_bolt);
  fonts_unload_custom_font(s_consolas_font);
  layer_destroy(s_rows_layer);
  layer_destroy(s_status_layer);
  text_layer_destroy(s_separators[0]);
  text_layer_destroy(s_separators[1]);
  text_layer_destroy(s_dday_layer);
  text_layer_destroy(s_ampm_layer);
  text_layer_destroy(s_countdown_layer);
  text_layer_destroy(s_date_layer);
  text_layer_destroy(s_time_layer);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_clock(tick_time);
  update_countdown(tick_time);
  update_dday(time(NULL));
  update_rows(time(NULL));

  // Nudge the phone as soon as either provider's data would otherwise go grey,
  // so a watchface left on the wrist keeps itself current without waiting for
  // the JS interval.
  time_t n = time(NULL);
  if (data_is_stale(n, 0) || data_is_stale(n, 1)) request_refresh();
}

// Flick of the wrist forces an immediate fetch.
static void tap_handler(AccelAxisType axis, int32_t direction) {
  request_refresh();
}

// Coming back into range is the moment a refresh is most likely to succeed.
static void connection_handler(bool connected) {
  layer_mark_dirty(s_status_layer);
  if (connected) request_refresh();
}

static void battery_handler(BatteryChargeState state) {
  layer_mark_dirty(s_status_layer);
}

static void init(void) {
  load_persisted();
#ifdef DEMO_DATA
  time_t n = time(NULL);
  s_pct[0] = 87;  s_reset[0] = n + 2 * 3600 + 13 * 60;
  s_pct[1] = 42;  s_reset[1] = n + 4 * 86400 + 5 * 3600;
  s_pct[2] = 100; s_reset[2] = n + 47 * 60;
  s_pct[3] = 6;   s_reset[3] = n + 6 * 86400;
  // Codes chosen to exercise three different icon shapes at once; change them
  // to 2 / 45 / 71 / 95 to eyeball the rest.
  s_temp[0] = 21; s_code[0] = 2;    // partly cloudy
  s_temp[1] = 19; s_code[1] = 51;   // drizzle
  s_temp[2] = -8; s_code[2] = -1;   // unknown
#endif

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload
  });

  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_open(256, 64);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  accel_tap_service_subscribe(tap_handler);
  connection_service_subscribe((ConnectionHandlers) {
    .pebble_app_connection_handler = connection_handler
  });
  battery_state_service_subscribe(battery_handler);

  window_stack_push(s_window, true);
  request_refresh();
}

static void deinit(void) {
  battery_state_service_unsubscribe();
  connection_service_unsubscribe();
  accel_tap_service_unsubscribe();
  tick_timer_service_unsubscribe();
  app_message_deregister_callbacks();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
