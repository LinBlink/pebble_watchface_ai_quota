#include <pebble.h>

// Define DEMO_DATA to preload sample values for checking the layout in the
// emulator. Note that the emulator's pkjs overwrites them within a second.

// Pebble Time (basalt) only: 144 x 168, 64 colours. Every coordinate below is
// tuned for that screen, so there is no need for the PBL_IF_* platform dances.
#define SCREEN_W 144

#define ROW_COUNT 4
#define WX_COUNT  3

#define NO_TEMP (-999)

typedef struct {
  const char *label;
  bool is_claude;
  bool is_weekly;
} RowSpec;

// One row per data slot, in on-screen order — back to the original 1:1
// mapping. Claude's rows have no pushed percentage any more (see
// advance_claude_reset/update_rows below): their bar/number instead show how
// much of the current window has *elapsed*, derived from the reset countdown.
static const RowSpec ROWS[ROW_COUNT] = {
  { "CL 5H", true,  false },
  { "CL 7D", true,  true  },
  { "MM 5H", false, false },
  { "MM 7D", false, true  },
};

// Persistent storage keys
#define PERSIST_PCT(i)     (100 + (i) * 2)
#define PERSIST_RESET(i)   (100 + (i) * 2 + 1)
#define PERSIST_TEMP(i)    (200 + (i) * 2)
#define PERSIST_CODE(i)    (200 + (i) * 2 + 1)
#define PERSIST_SUNRISE    206
#define PERSIST_SUNSET     207
#define PERSIST_NEXT_EVENT_START 208
#define PERSIST_NEXT_EVENT_TITLE 209
#define PERSIST_LAST_SYNC(p) (320 + (p))  // 0 = claude, 1 = minimax
#define PERSIST_TARGET     301
#define PERSIST_THEME      310
#define PERSIST_TIME_FONT  311
#define PERSIST_AI_PROVIDER 312
#define PERSIST_ALERTED(i) (400 + (i))
#define PERSIST_RESET_ALERTED(i) (450 + (i))
#define PERSIST_CODEX_PCT(i) (500 + (i) * 2)
#define PERSIST_CODEX_RESET(i) (500 + (i) * 2 + 1)
#define PERSIST_CODEX_ALERTED(i) (520 + (i))
#define PERSIST_CODEX_RESET_ALERTED(i) (530 + (i))
#define PERSIST_GITHUB_COMMITS 540
#define PERSIST_GITHUB_SYNC    541
#define PERSIST_GITHUB_LATEST  542
#define PERSIST_BANK_NAME      550
#define PERSIST_BANK_LAST4     551
#define PERSIST_BANK_BALANCE   552
#define PERSIST_BANK_UPDATED   553
#define PERSIST_CMB_BALANCE    554
#define PERSIST_CMB_UPDATED    555
#define PERSIST_CMB_BASELINE_AT 556
#define PERSIST_CMB_EVENT_INDEX 557
#define PERSIST_ICBC_BALANCE   558
#define PERSIST_ICBC_UPDATED   559
#define CMB_EVENT_HISTORY      16
#define PERSIST_CMB_EVENT(i)   (560 + (i))

// Buzz once when a quota first crosses this. Persisted per row so a restart
// doesn't re-alert, and cleared when the window resets so the next crossing
// buzzes again.
#define ALERT_PCT 80

// Buzz once when a weekly window's reset drops to within a day. Same
// arm/re-arm persistence pattern as ALERT_PCT above.
#define RESET_ALERT_SEC (24 * 60 * 60)

// Quota values older than this are shown greyed out: the phone is reachable but
// the numbers behind them are no longer trustworthy.
#define STALE_AFTER_SEC (20 * 60)

// Claude's two windows (rows 0 and 1) have no live quota fetch at all — the
// phone just calibrates a reset time once, and the firmware itself rolls the
// countdown over to the next window when one expires. That is what makes
// these two immune to the "phone/network went stale" problem entirely.
#define CLAUDE_5H_PERIOD_SEC (5 * 60 * 60)
#define CLAUDE_WK_PERIOD_SEC (7 * 24 * 60 * 60)

// Claude's percentage is elapsed window time, not used quota — a violet that
// doesn't collide with the orange row label, the blue MiniMax rows, or the
// grey/red states a real usage number can be in, so it reads as its own kind
// of thing at a glance instead of looking like a mislabeled quota.
#define CLAUDE_ELAPSED_COLOR GColorVividViolet

static int32_t s_pct[ROW_COUNT];     // -1 = no data yet
static int32_t s_reset[ROW_COUNT];   // absolute UTC epoch seconds of next reset
static int32_t s_codex_pct[2];       // manually calibrated Codex 5h / 7d usage
static int32_t s_codex_reset[2];     // Codex reset timestamps, kept across display switches
static int32_t s_temp[WX_COUNT];     // NO_TEMP = no data yet
static int32_t s_code[WX_COUNT];     // WMO weather code
static int32_t s_sunrise;
static int32_t s_sunset;
static int32_t s_next_event_start;
// UTC epoch of the last quota push per provider (0 = claude, 1 = minimax), 0 =
// never. Tracked separately because the two providers fail independently — if
// only one of them is actually landing pushes, its rows must grey out on their
// own instead of being propped up by the other provider's traffic.
static int32_t s_last_sync[2];
static bool s_refresh_pending;       // a refresh was asked for and hasn't landed yet
static int32_t s_target_date;        // local midnight of the countdown target, 0 = unset
static bool s_alerted[ROW_COUNT];    // this row has already buzzed for its current window
static bool s_reset_alerted[ROW_COUNT]; // this weekly row has already buzzed for its upcoming reset
static bool s_codex_alerted[2];
static bool s_codex_reset_alerted[2];
static int32_t s_github_commits;      // commits authored today, -1 = no data
static int32_t s_github_sync;         // UTC epoch of last successful GitHub fetch
static int32_t s_github_latest;       // UTC epoch of the most recent visible commit
static int32_t s_bank_balance_cents;  // integer cents; -1 = no data
static int32_t s_bank_updated;        // UTC epoch from the SMS parser
static int32_t s_cmb_balance_cents;   // locally maintained from CMB notifications
static int32_t s_cmb_updated;
static int32_t s_cmb_baseline_at;     // ignore notifications at/before calibration
static int32_t s_cmb_event_ids[CMB_EVENT_HISTORY];
static int32_t s_cmb_event_index;
static int32_t s_icbc_balance_cents; // absolute balance from ICBC notifications
static int32_t s_icbc_updated;
static int s_bank_display_index;     // NJ, CMB, ICBC, then total
static bool s_show_lunar;
static int s_weather_page;  // 0 = forecast, 1 = sunrise/sunset, 2 = next event

// Settings pushed from the phone. s_theme picks THEMES[], s_time_font picks the
// clock face font. Defaults match the phone-side DEFAULTS so a fresh install
// looks right even before the first AppMessage lands.
static int32_t s_theme;              // 0 = light, 1 = dark
static int32_t s_time_font;          // 0 = bitham, 1 = consolas
static int32_t s_ai_provider;        // 0 = Claude, 1 = ChatGPT Codex

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
static GFont s_bank_font;              // compact Chinese bank labels
static GFont s_lunar_font;             // compact Chinese lunar date
static TextLayer *s_time_layer;
static TextLayer *s_date_layer;
static TextLayer *s_countdown_layer;
static TextLayer *s_birthday_layer;
static TextLayer *s_dday_layer;
static TextLayer *s_wx_temp_layers[WX_COUNT];
static Layer *s_wx_icons_layer;
static TextLayer *s_bank_meta_layer;
static TextLayer *s_bank_balance_layer;
static TextLayer *s_bank_sum_layer;
static AppTimer *s_bank_timer;
static AppTimer *s_date_timer;
static AppTimer *s_weather_timer;
static TextLayer *s_separators[3];
static Layer *s_status_layer;
static Layer *s_rows_layer;
static TextLayer *s_label_layers[ROW_COUNT];
static TextLayer *s_pct_layers[ROW_COUNT];
static TextLayer *s_reset_layers[ROW_COUNT];

static char s_time_buf[8];
static char s_date_buf[32];
static char s_countdown_buf[10];
static char s_birthday_buf[12];
static char s_dday_buf[8];
static char s_wx_temp_buf[WX_COUNT][8];
static char s_sunrise_buf[12];
static char s_sunset_buf[12];
static char s_next_event_title[40];
static char s_next_event_buf[52];
static char s_bank_balance_buf[20];
static char s_bank_sum_buf[20];
static char s_pct_buf[ROW_COUNT][12];
static char s_reset_buf[ROW_COUNT][12];
static uint32_t s_whimsy;

static void update_date(struct tm *t);

// A tiny deterministic bit of whimsy keeps independent panels from marching
// in lockstep while staying close enough to a one-second cadence.
static uint32_t next_whimsy_delay(void) {
  s_whimsy = s_whimsy * 1664525u + 1013904223u;
  return 850 + (s_whimsy % 301);  // 0.85–1.15 seconds
}

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
#define GHD_X         112
#define GHD_Y         3
#define GHD_W         28
#define GHD_H         13
#define BIRTHDAY_X    2
#define BIRTHDAY_Y    17
#define BIRTHDAY_W    24
#define DDAY_X        119
#define DDAY_Y        17
#define DDAY_W        22
#define CORNER_H      16

#define DATE_Y        36
#define DATE_LUNAR_Y  39
#define DATE_H        20
#define DATE_X        3
#define DATE_W        90
#define CD_W          48

#define SEP1_Y        57

// Three compact icon/temperature columns represent now, +6h and +24h by
// position while reserving the next full line for the bank balance.
#define WX_COL_W      (SCREEN_W / WX_COUNT)
#define WX_Y          58
#define WX_H          20
#define WX_TEMP_X     20
#define WX_TEMP_W     28
#define WX_TEMP_H     20
#define SUN_COL_W      (SCREEN_W / 2)
#define SEP_WX_Y      78

#define BANK_Y        79
#define BANK_H        20
#define BANK_META_X   3
#define BANK_META_W   40
#define BANK_VALUE_X  43
#define BANK_VALUE_W  42
#define BANK_SUM_X    87
#define BANK_SUM_W    54
#define SEP2_Y        99

// 17px per row leaves 15px of text above a 2px bar. The text sits 1px high of
// the row origin so glyph baselines clear the bar — at y+0 the 14px font's
// baseline lands exactly on it and every label reads as struck through.
#define ROWS_Y        100
#define ROW_H         17
#define BAR_H         4
#define TEXT_DY       (-1)
#define PCT_DY        (-4)
#define R_LABEL_X     3
#define R_LABEL_W     42
#define R_PCT_W       44
#define R_RESET_W     52

// ------------------------------------------------------------ weather icons

typedef enum {
  WX_SUN, WX_SUN_CLOUD, WX_CLOUD, WX_FOG,
  WX_DRIZZLE, WX_RAIN, WX_SNOW, WX_STORM, WX_UNKNOWN
} WxIcon;

static WxIcon wmo_icon(int32_t code) {
  if (code < 0) return WX_UNKNOWN;
  if (code <= 1) return WX_SUN;
  if (code == 2) return WX_SUN_CLOUD;
  if (code == 3) return WX_CLOUD;
  if (code == 45 || code == 48) return WX_FOG;
  if (code >= 51 && code <= 57) return WX_DRIZZLE;
  if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return WX_RAIN;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return WX_SNOW;
  if (code >= 95) return WX_STORM;
  return WX_UNKNOWN;
}

static void draw_small_sun(GContext *ctx, int x, int y) {
  GColor color = THEMES[s_theme].accent;
  graphics_context_set_fill_color(ctx, color);
  graphics_context_set_stroke_color(ctx, color);
  graphics_fill_circle(ctx, GPoint(x + 9, y + 9), 3);
  graphics_draw_line(ctx, GPoint(x + 9, y + 3), GPoint(x + 9, y + 1));
  graphics_draw_line(ctx, GPoint(x + 9, y + 15), GPoint(x + 9, y + 17));
  graphics_draw_line(ctx, GPoint(x + 3, y + 9), GPoint(x + 1, y + 9));
  graphics_draw_line(ctx, GPoint(x + 15, y + 9), GPoint(x + 17, y + 9));
}

static void draw_small_cloud(GContext *ctx, int x, int y, GColor color) {
  int base = y + 11;
  graphics_context_set_fill_color(ctx, color);
  graphics_fill_circle(ctx, GPoint(x + 5, base), 3);
  graphics_fill_circle(ctx, GPoint(x + 9, base - 2), 4);
  graphics_fill_circle(ctx, GPoint(x + 13, base), 3);
  graphics_fill_rect(ctx, GRect(x + 3, base, 12, 4), 0, GCornerNone);
}

static void draw_small_wx_icon(GContext *ctx, int x, WxIcon icon) {
  x += 1;
  switch (icon) {
    case WX_SUN:
      draw_small_sun(ctx, x, 1);
      break;
    case WX_SUN_CLOUD:
      draw_small_sun(ctx, x + 4, -1);
      draw_small_cloud(ctx, x, 4, THEMES[s_theme].cloud);
      break;
    case WX_CLOUD:
      draw_small_cloud(ctx, x, 3, THEMES[s_theme].cloud);
      break;
    case WX_FOG:
      graphics_context_set_stroke_color(ctx, THEMES[s_theme].fog);
      graphics_draw_line(ctx, GPoint(x + 2, 5), GPoint(x + 16, 5));
      graphics_draw_line(ctx, GPoint(x + 4, 9), GPoint(x + 14, 9));
      graphics_draw_line(ctx, GPoint(x + 2, 13), GPoint(x + 16, 13));
      break;
    case WX_DRIZZLE:
    case WX_RAIN:
      draw_small_cloud(ctx, x, 0, THEMES[s_theme].cloud);
      graphics_context_set_stroke_color(ctx, GColorPictonBlue);
      for (int i = 0; i < 3; i++) {
        int drop_x = x + 5 + i * 4;
        int len = icon == WX_RAIN ? 4 : 2;
        graphics_draw_line(ctx, GPoint(drop_x, 14), GPoint(drop_x - 1, 14 + len));
      }
      break;
    case WX_SNOW:
      draw_small_cloud(ctx, x, 0, THEMES[s_theme].cloud);
      graphics_context_set_fill_color(ctx, THEMES[s_theme].snow);
      for (int i = 0; i < 3; i++) {
        graphics_fill_circle(ctx, GPoint(x + 5 + i * 4, 16), 1);
      }
      break;
    case WX_STORM:
      draw_small_cloud(ctx, x, 0, GColorDarkGray);
      graphics_context_set_stroke_color(ctx, THEMES[s_theme].bolt);
      graphics_draw_line(ctx, GPoint(x + 10, 12), GPoint(x + 7, 16));
      graphics_draw_line(ctx, GPoint(x + 7, 16), GPoint(x + 11, 16));
      graphics_draw_line(ctx, GPoint(x + 11, 16), GPoint(x + 9, 19));
      break;
    default:
      graphics_context_set_stroke_color(ctx, THEMES[s_theme].fg_dim);
      graphics_draw_line(ctx, GPoint(x + 5, 10), GPoint(x + 13, 10));
      break;
  }
}

static void wx_icons_update_proc(Layer *layer, GContext *ctx) {
  if (s_weather_page != 0) return;
  for (int i = 0; i < WX_COUNT; i++) {
    WxIcon icon = s_temp[i] == NO_TEMP ? WX_UNKNOWN : wmo_icon(s_code[i]);
    draw_small_wx_icon(ctx, i * WX_COL_W, icon);
  }
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

// Rows 0/1 (CL 5H, CL 7D) are calibrated once and never re-synced, so nothing
// else will ever push them forward once they hit zero — do it here instead,
// every minute, so the countdown always reflects "time left in the *current*
// window" rather than freezing at "now" forever after the first expiry.
static void advance_claude_reset(time_t now) {
  for (int i = 0; i < ROW_COUNT; i++) {
    if (!ROWS[i].is_claude || s_reset[i] <= 0) continue;
    int32_t period = ROWS[i].is_weekly ? CLAUDE_WK_PERIOD_SEC : CLAUDE_5H_PERIOD_SEC;
    bool changed = false;
    while (s_reset[i] <= (int32_t)now) {
      s_reset[i] += period;
      changed = true;
    }
    if (changed) persist_write_int(PERSIST_RESET(i), s_reset[i]);
  }

  for (int i = 0; i < 2; i++) {
    if (s_codex_reset[i] <= 0) continue;
    int32_t period = i == 1 ? CLAUDE_WK_PERIOD_SEC : CLAUDE_5H_PERIOD_SEC;
    bool changed = false;
    while (s_codex_reset[i] <= (int32_t)now) {
      s_codex_reset[i] += period;
      changed = true;
    }
    if (changed) {
      s_codex_pct[i] = 0;
      persist_write_int(PERSIST_CODEX_PCT(i), 0);
      persist_write_int(PERSIST_CODEX_RESET(i), s_codex_reset[i]);
    }
  }
}

// Claude has no pushed usage percentage — instead, how much of the current
// window has *elapsed* since the last reset, as a 0-100 bar fill. Derived
// straight from the reset countdown, so it needs no separate "window start"
// bookkeeping: elapsed = period - remaining.
static int32_t claude_elapsed_pct(int i, time_t now) {
  if (s_reset[i] <= 0) return -1;
  int32_t period = ROWS[i].is_weekly ? CLAUDE_WK_PERIOD_SEC : CLAUDE_5H_PERIOD_SEC;
  int32_t remaining = s_reset[i] - (int32_t)now;
  int32_t elapsed = period - remaining;
  if (elapsed < 0) elapsed = 0;
  if (elapsed > period) elapsed = period;
  return (elapsed * 100) / period;
}

static int32_t active_pct(int i, time_t now) {
  if (i >= 2) return s_pct[i];
  return s_ai_provider ? s_codex_pct[i] : claude_elapsed_pct(i, now);
}

static int32_t active_reset(int i) {
  return i < 2 && s_ai_provider ? s_codex_reset[i] : s_reset[i];
}

static void update_provider_labels(void) {
  text_layer_set_text(s_label_layers[0], s_ai_provider ? "CX 5H" : "CL 5H");
  text_layer_set_text(s_label_layers[1], s_ai_provider ? "CX 7D" : "CL 7D");
}

static void update_rows(time_t now) {
  update_provider_labels();
  for (int i = 0; i < ROW_COUNT; i++) {
    bool is_claude = i < 2 && !s_ai_provider;
    int32_t p = active_pct(i, now);
    bool stale = (i >= 2 && data_is_stale(now, 1)) ||
                 (i < 2 && s_ai_provider && data_is_stale(now, 0));

    if (p < 0) {
      snprintf(s_pct_buf[i], sizeof(s_pct_buf[i]), "--");
      // The placeholder is the same dark grey in both themes.
      text_layer_set_text_color(s_pct_layers[i], GColorDarkGray);
    } else {
      if (p > 100) p = 100;
      snprintf(s_pct_buf[i], sizeof(s_pct_buf[i]), "%ld%%", (long)p);
      // Claude's number is a different kind of thing from MiniMax's — elapsed
      // window time, not used quota — so it gets its own colour throughout,
      // never the grey/red that mean "stale" or "running low" for a real
      // usage percentage. Grey doesn't apply here anyway (computed fresh every
      // redraw), and high-elapsed just means "about to reset", not a warning.
      text_layer_set_text_color(s_pct_layers[i],
                                is_claude ? CLAUDE_ELAPSED_COLOR
                                : (stale ? THEMES[s_theme].stale
                                         : (p >= 90 ? GColorRed : THEMES[s_theme].fg)));
    }
    text_layer_set_text(s_pct_layers[i], s_pct_buf[i]);

    int32_t reset = active_reset(i);
    if (reset <= 0) {
      snprintf(s_reset_buf[i], sizeof(s_reset_buf[i]), "--");
    } else {
      fmt_remaining(s_reset_buf[i], sizeof(s_reset_buf[i]), reset - (int32_t)now);
    }
    text_layer_set_text(s_reset_layers[i], s_reset_buf[i]);
  }
  layer_mark_dirty(s_rows_layer);
}

static void update_weather(void) {
  if (s_weather_page == 2) {
    if (s_next_event_start > (int32_t)time(NULL) && s_next_event_title[0]) {
      time_t event_at = s_next_event_start;
      struct tm *event_time = localtime(&event_at);
      strftime(s_next_event_buf, sizeof(s_next_event_buf), "%H:%M ", event_time);
      strncat(s_next_event_buf, s_next_event_title,
              sizeof(s_next_event_buf) - strlen(s_next_event_buf) - 1);
    } else {
      snprintf(s_next_event_buf, sizeof(s_next_event_buf), "--:-- FREE");
    }
    layer_set_frame(text_layer_get_layer(s_wx_temp_layers[0]),
                    GRect(3, WX_Y, SCREEN_W - 6, WX_TEMP_H));
    text_layer_set_text_alignment(s_wx_temp_layers[0], GTextAlignmentLeft);
    text_layer_set_text(s_wx_temp_layers[0], s_next_event_buf);
    text_layer_set_text(s_wx_temp_layers[1], "");
    text_layer_set_text(s_wx_temp_layers[2], "");
    layer_mark_dirty(s_wx_icons_layer);
    return;
  }
  if (s_weather_page == 1) {
    if (s_sunrise > 0) {
      time_t sunrise_at = s_sunrise;
      struct tm *sunrise = localtime(&sunrise_at);
      strftime(s_sunrise_buf, sizeof(s_sunrise_buf), "UP %H:%M", sunrise);
    } else {
      snprintf(s_sunrise_buf, sizeof(s_sunrise_buf), "UP --:--");
    }
    if (s_sunset > 0) {
      time_t sunset_at = s_sunset;
      struct tm *sunset = localtime(&sunset_at);
      strftime(s_sunset_buf, sizeof(s_sunset_buf), "DN %H:%M", sunset);
    } else {
      snprintf(s_sunset_buf, sizeof(s_sunset_buf), "DN --:--");
    }
    layer_set_frame(text_layer_get_layer(s_wx_temp_layers[0]),
                    GRect(0, WX_Y, SUN_COL_W, WX_TEMP_H));
    layer_set_frame(text_layer_get_layer(s_wx_temp_layers[1]),
                    GRect(SUN_COL_W, WX_Y, SUN_COL_W, WX_TEMP_H));
    text_layer_set_text_alignment(s_wx_temp_layers[0], GTextAlignmentCenter);
    text_layer_set_text_alignment(s_wx_temp_layers[1], GTextAlignmentCenter);
    text_layer_set_text(s_wx_temp_layers[0], s_sunrise_buf);
    text_layer_set_text(s_wx_temp_layers[1], s_sunset_buf);
    text_layer_set_text(s_wx_temp_layers[2], "");
    layer_mark_dirty(s_wx_icons_layer);
    return;
  }
  for (int i = 0; i < WX_COUNT; i++) {
    if (s_temp[i] == NO_TEMP) {
      snprintf(s_wx_temp_buf[i], sizeof(s_wx_temp_buf[i]), "--");
    } else {
      snprintf(s_wx_temp_buf[i], sizeof(s_wx_temp_buf[i]), "%ld°", (long)s_temp[i]);
    }
    int x = i * WX_COL_W;
    layer_set_frame(text_layer_get_layer(s_wx_temp_layers[i]),
                    GRect(x + WX_TEMP_X, WX_Y, WX_TEMP_W, WX_TEMP_H));
    text_layer_set_text_alignment(s_wx_temp_layers[i], GTextAlignmentCenter);
    text_layer_set_text(s_wx_temp_layers[i], s_wx_temp_buf[i]);
  }
  layer_mark_dirty(s_wx_icons_layer);
}

static void format_bank_balance(char *buf, size_t size, int64_t cents) {
  bool negative = cents < 0;
  uint64_t absolute = negative ? (uint64_t)(-cents) : (uint64_t)cents;
  if (absolute >= 100000) {
    uint64_t hundredths_k = (absolute + 500) / 1000;
    snprintf(buf, size, "%s%lu.%02luK", negative ? "-" : "",
             (unsigned long)(hundredths_k / 100),
             (unsigned long)(hundredths_k % 100));
  } else {
    snprintf(buf, size, "%s%lu", negative ? "-" : "",
             (unsigned long)(absolute / 100));
  }
}

static void update_bank(void) {
  static const char *labels[] = { "南京银行", "招商银行", "工商银行" };
  int32_t balance = -1;
  int32_t updated = 0;
  bool has_balance = false;
  int known = 0;
  int64_t total = 0;

  text_layer_set_text(s_bank_meta_layer, labels[s_bank_display_index]);
  if (s_bank_display_index == 0) {
    balance = s_bank_balance_cents;
    updated = s_bank_updated;
    has_balance = updated > 0;
  } else if (s_bank_display_index == 1) {
    balance = s_cmb_balance_cents;
    updated = s_cmb_updated;
    has_balance = s_cmb_baseline_at > 0;
  } else if (s_bank_display_index == 2) {
    balance = s_icbc_balance_cents;
    updated = s_icbc_updated;
    has_balance = updated > 0;
  }

  if (s_bank_updated > 0) {
    total += s_bank_balance_cents;
    known++;
  }
  if (s_cmb_baseline_at > 0) {
    total += s_cmb_balance_cents;
    known++;
  }
  if (s_icbc_updated > 0) {
    total += s_icbc_balance_cents;
    known++;
  }

  if (!has_balance) {
    snprintf(s_bank_balance_buf, sizeof(s_bank_balance_buf), "--");
  } else {
    int64_t value = s_bank_display_index == 3 ? total : balance;
    format_bank_balance(s_bank_balance_buf, sizeof(s_bank_balance_buf), value);
  }
  text_layer_set_text(s_bank_balance_layer, s_bank_balance_buf);

  bool stale = updated <= 0 || (int32_t)time(NULL) - updated > 24 * 60 * 60;
  text_layer_set_text_color(s_bank_balance_layer,
                            stale ? THEMES[s_theme].stale : THEMES[s_theme].fg);

  if (known) {
    format_bank_balance(s_bank_sum_buf, sizeof(s_bank_sum_buf), total);
  } else {
    snprintf(s_bank_sum_buf, sizeof(s_bank_sum_buf), "--");
  }
  text_layer_set_text(s_bank_sum_layer, s_bank_sum_buf);
  int32_t newest = s_bank_updated;
  if (s_cmb_updated > newest) newest = s_cmb_updated;
  if (s_icbc_updated > newest) newest = s_icbc_updated;
  stale = newest <= 0 || (int32_t)time(NULL) - newest > 24 * 60 * 60;
  text_layer_set_text_color(s_bank_sum_layer,
                            stale ? THEMES[s_theme].stale : THEMES[s_theme].fg);
}

static void bank_timer_handler(void *context) {
  s_bank_timer = NULL;
  s_bank_display_index = (s_bank_display_index + 1) % 3;
  update_bank();
  s_bank_timer = app_timer_register(next_whimsy_delay(), bank_timer_handler, NULL);
}

static void date_timer_handler(void *context) {
  s_date_timer = NULL;
  s_show_lunar = !s_show_lunar;
  time_t now = time(NULL);
  update_date(localtime(&now));
  s_date_timer = app_timer_register(next_whimsy_delay(), date_timer_handler, NULL);
}

static void weather_timer_handler(void *context) {
  s_weather_timer = NULL;
  s_weather_page = (s_weather_page + 1) % 3;
  update_weather();
  s_weather_timer = app_timer_register(next_whimsy_delay(), weather_timer_handler, NULL);
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

// Lunar data for 2000-2039. The watch only needs the compact month/day label;
// dates outside this small table fall back to the Gregorian display.
static const uint32_t LUNAR_INFO[] = {
  0x0c960, 0x0d954, 0x0d4a0, 0x0da50, 0x07552, 0x056a0, 0x0abb7, 0x025d0,
  0x092d0, 0x0cab5, 0x0a950, 0x0b4a0, 0x0baa4, 0x0ad50, 0x055d9, 0x04ba0,
  0x0a5b0, 0x15176, 0x052b0, 0x0a930, 0x07954, 0x06aa0, 0x0ad50, 0x05b52,
  0x04b60, 0x0a6e6, 0x0a4e0, 0x0d260, 0x0ea65, 0x0d530, 0x05aa0, 0x076a3,
  0x096d0, 0x04afb, 0x04ad0, 0x0a4d0, 0x1d0b6, 0x0d250, 0x0d520, 0x0dd45
};

static int lunar_leap_month(int year) {
  return LUNAR_INFO[year - 2000] & 0xf;
}

static int lunar_leap_days(int year) {
  uint32_t info = LUNAR_INFO[year - 2000];
  return lunar_leap_month(year) ? ((info & 0x10000) ? 30 : 29) : 0;
}

static int lunar_month_days(int year, int month) {
  return (LUNAR_INFO[year - 2000] & (0x10000 >> month)) ? 30 : 29;
}

static int lunar_year_days(int year) {
  int days = 348;
  uint32_t info = LUNAR_INFO[year - 2000];
  for (uint32_t mask = 0x8000; mask > 0x8; mask >>= 1)
    if (info & mask) days++;
  return days + lunar_leap_days(year);
}

static void format_lunar_date(struct tm *t) {
#if 0
  static const char *months[] = {
    "", "正", "二", "三", "四", "五", "六", "七", "八", "九", "十", "冬", "腊"
  };
  static const char *days[] = {
    "", "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
    "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "廿",
    "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十"
  };
#endif
  int year = t->tm_year + 1900;
  // 2000-02-05 is lunar 2000-01-01.
  int offset = -35;
  if (year < 2000 || year >= 2040) {
    snprintf(s_date_buf, sizeof(s_date_buf), "--");
    return;
  }
  for (int y = 2000; y < year; y++) {
    offset += (y % 4 == 0 && y % 100 != 0) || y % 400 == 0 ? 366 : 365;
  }
  static const int month_days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  for (int m = 1; m < t->tm_mon + 1; m++) {
    offset += month_days[m - 1];
    if (m == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) offset++;
  }
  offset += t->tm_mday - 1;
  int lunar_year = 2000;
  while (lunar_year < 2039 && offset >= lunar_year_days(lunar_year)) {
    offset -= lunar_year_days(lunar_year++);
  }
  int lunar_month = 1;
  bool leap = false;
  while (lunar_month <= 12) {
    int days_in_month = leap ? lunar_leap_days(lunar_year)
                             : lunar_month_days(lunar_year, lunar_month);
    if (offset < days_in_month) break;
    offset -= days_in_month;
    if (lunar_leap_month(lunar_year) == lunar_month && !leap) {
      leap = true;
    } else {
      if (leap) leap = false;
      lunar_month++;
    }
  }
  int lunar_day = offset + 1;
  // Use a compact numeric label here: Pebble's small Chinese font can clip
  // repeated edge glyphs in a four-character lunar date.
  snprintf(s_date_buf, sizeof(s_date_buf), "L%02d-%02d",
           lunar_month, lunar_day);
}

static void update_date(struct tm *t) {
  if (s_show_lunar) {
    format_lunar_date(t);
    text_layer_set_font(s_date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
    layer_set_frame(text_layer_get_layer(s_date_layer),
                    GRect(DATE_X, DATE_Y, DATE_W, DATE_H));
  } else {
    strftime(s_date_buf, sizeof(s_date_buf), "%a %m-%d", t);
    text_layer_set_font(s_date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
    layer_set_frame(text_layer_get_layer(s_date_layer),
                    GRect(DATE_X, DATE_Y, DATE_W, DATE_H));
  }
  text_layer_set_text(s_date_layer, s_date_buf);
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

// Whole local-calendar days until the next March 28. The birthday itself is
// day zero; after it passes, the target advances to the following year.
static void update_birthday(time_t now) {
  time_t today_midnight = local_midnight(now);
  struct tm birthday = *localtime(&now);
  birthday.tm_mon = 2;  // struct tm months are zero-based
  birthday.tm_mday = 28;
  birthday.tm_hour = 0;
  birthday.tm_min = 0;
  birthday.tm_sec = 0;
  time_t target = mktime(&birthday);
  if (target < today_midnight) {
    birthday.tm_year++;
    target = mktime(&birthday);
  }
  int days = (int)((target - today_midnight + 43200) / 86400);
  snprintf(s_birthday_buf, sizeof(s_birthday_buf), "%d", days);
#ifdef WIDEST_CLOCK
  snprintf(s_birthday_buf, sizeof(s_birthday_buf), "365");
#endif
  text_layer_set_text(s_birthday_layer, s_birthday_buf);
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

  update_date(t);
}

static void update_ui(void) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  update_clock(t);
  update_countdown(t);
  update_dday(now);
  update_birthday(now);
  update_weather();
  update_bank();
  advance_claude_reset(now);
  update_rows(now);
}

// -------------------------------------------------------------------- render

static GColor row_color(int i) {
  if (i == 0) return GColorJaegerGreen;
  if (i >= 2) return GColorPictonBlue;
  return s_ai_provider ? GColorGreen : GColorOrange;
}

static void rows_update_proc(Layer *layer, GContext *ctx) {
  // The gauge starts after the label rather than at the screen edge. Run it the
  // full width and it crosses the label's glyphs at exactly baseline height,
  // which reads as a strikethrough; starting it here also lines the gauge up
  // with the number it is describing.
  int bar_x = R_LABEL_X + R_LABEL_W;
  int bar_w = SCREEN_W - R_LABEL_X - bar_x;
  time_t now = time(NULL);

  for (int i = 0; i < ROW_COUNT; i++) {
    bool is_claude = i < 2 && !s_ai_provider;
    int y = i * ROW_H + ROW_H - BAR_H;
    int32_t p = active_pct(i, now);

    graphics_context_set_fill_color(ctx, THEMES[s_theme].gauge_track);
    graphics_fill_rect(ctx, GRect(bar_x, y, bar_w, BAR_H), 0, GCornerNone);

    if (p >= 0) {
      if (p > 100) p = 100;
      // Red is a MiniMax-only "running low" warning — for Claude, a full bar
      // just means "about to reset", so it stays the row colour throughout.
      graphics_context_set_fill_color(ctx, (!is_claude && p >= 90) ? GColorRed : row_color(i));
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
  int w = st.is_charging ? inner : (inner * st.charge_percent) / 100;
  if (w > 0) {
    graphics_context_set_fill_color(ctx, fill);
    graphics_fill_rect(ctx, GRect(x + 1, y + 1, w, BATT_H - 2), 0, GCornerNone);
  }
  if (st.is_charging) {
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_line(ctx, GPoint(x + 12, y + 1), GPoint(x + 8, y + 5));
    graphics_draw_line(ctx, GPoint(x + 8, y + 5), GPoint(x + 12, y + 5));
    graphics_draw_line(ctx, GPoint(x + 12, y + 5), GPoint(x + 8, y + 9));
    graphics_context_set_stroke_width(ctx, 1);
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

static void draw_bluetooth_disconnected(GContext *ctx, int x, int y) {
  draw_bluetooth(ctx, x, y);
  graphics_context_set_stroke_color(ctx, GColorRed);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_line(ctx, GPoint(x - 1, y), GPoint(x + 12, y + 13));
  graphics_context_set_stroke_width(ctx, 1);
}

static void status_update_proc(Layer *layer, GContext *ctx) {
  if (connection_service_peek_pebble_app_connection()) {
    draw_battery(ctx, BATT_X, BATT_Y);
  } else {
    draw_bluetooth_disconnected(ctx, BATT_X, BATT_Y);
  }

  char ghd_buf[12];
  if (s_github_commits < 0) {
    snprintf(ghd_buf, sizeof(ghd_buf), "--");
  } else {
    snprintf(ghd_buf, sizeof(ghd_buf), "%ld", (long)s_github_commits);
  }
  graphics_context_set_text_color(ctx, THEMES[s_theme].fg_dim);
  graphics_draw_text(ctx, ghd_buf,
                     fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(GHD_X, GHD_Y, GHD_W, GHD_H),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentRight,
                     NULL);
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
  text_layer_set_text_color(s_birthday_layer, t->fg_dim);
  text_layer_set_text_color(s_dday_layer, t->fg);

  for (int i = 0; i < WX_COUNT; i++)
    text_layer_set_text_color(s_wx_temp_layers[i], t->fg);
  text_layer_set_text_color(s_bank_meta_layer, t->fg_dim);
  text_layer_set_text_color(s_bank_sum_layer, t->fg);

  text_layer_set_background_color(s_separators[0], t->separator);
  text_layer_set_background_color(s_separators[1], t->separator);
  text_layer_set_background_color(s_separators[2], t->separator);

  for (int i = 0; i < ROW_COUNT; i++) {
    text_layer_set_text_color(s_label_layers[i], row_color(i));
    text_layer_set_text_color(s_reset_layers[i], t->fg_dim);
  }

  // update_rows() owns the percentage colour (placeholder / stale / normal),
  // so re-run it to pick up the new palette, then redraw the drawn layers.
  update_rows(time(NULL));
  update_bank();
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

// Claude's two rows only ever get a reset time (calibration), never a
// percentage — there is no MESSAGE_KEY_CLAUDE_*_PCT any more.
static void apply_reset_only(DictionaryIterator *it, uint32_t reset_key, int idx) {
  Tuple *r = dict_find(it, reset_key);
  if (r) store_int(PERSIST_RESET(idx), &s_reset[idx], r->value->int32);
}

static void apply_codex_quota(DictionaryIterator *it, uint32_t pct_key,
                              uint32_t reset_key, int idx) {
  Tuple *p = dict_find(it, pct_key);
  Tuple *r = dict_find(it, reset_key);
  if (p) store_int(PERSIST_CODEX_PCT(idx), &s_codex_pct[idx], p->value->int32);
  if (r) store_int(PERSIST_CODEX_RESET(idx), &s_codex_reset[idx], r->value->int32);
}

static void apply_weather(DictionaryIterator *it, uint32_t temp_key, uint32_t code_key, int idx) {
  Tuple *t = dict_find(it, temp_key);
  Tuple *c = dict_find(it, code_key);
  if (t) store_int(PERSIST_TEMP(idx), &s_temp[idx], t->value->int32);
  if (c) store_int(PERSIST_CODE(idx), &s_code[idx], c->value->int32);
}

static bool cmb_event_seen(int32_t event_id) {
  for (int i = 0; i < CMB_EVENT_HISTORY; i++) {
    if (s_cmb_event_ids[i] == event_id) return true;
  }
  return false;
}

static void remember_cmb_event(int32_t event_id) {
  int slot = s_cmb_event_index % CMB_EVENT_HISTORY;
  s_cmb_event_ids[slot] = event_id;
  persist_write_int(PERSIST_CMB_EVENT(slot), event_id);
  s_cmb_event_index = (slot + 1) % CMB_EVENT_HISTORY;
  persist_write_int(PERSIST_CMB_EVENT_INDEX, s_cmb_event_index);
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
    if (i < 2 && !s_ai_provider) continue;
    int32_t pct = i < 2 ? s_codex_pct[i] : s_pct[i];
    bool *alerted = i < 2 ? &s_codex_alerted[i] : &s_alerted[i];
    uint32_t key = i < 2 ? PERSIST_CODEX_ALERTED(i) : PERSIST_ALERTED(i);
    if (pct < 0) continue;
    bool over = pct >= ALERT_PCT;
    if (over == *alerted) continue;
    if (over) crossed = true;
    *alerted = over;
    persist_write_bool(key, over);
  }

  // One buzz even if several rows cross in the same push. Quiet time is the
  // user having said "not now", so it wins.
  if (crossed && !quiet_time_is_active()) vibes_double_pulse();
}

// Long buzz once a weekly (7D) window's reset drops to within a day out. This
// is evaluated against the clock, not against incoming pushes, since the
// countdown moves on its own even between syncs — so it must run every tick,
// not just from inbox_received. Re-arms once the window actually rolls over
// (or a fresh push shows the reset pushed back out past a day), so the next
// week's approach buzzes again.
static void check_reset_alerts(time_t now) {
  bool crossed = false;

  for (int i = 0; i < ROW_COUNT; i++) {
    if (!ROWS[i].is_weekly) continue;
    int32_t reset = active_reset(i);
    if (reset <= 0) continue;
    bool *alerted = i < 2 && s_ai_provider ? &s_codex_reset_alerted[i]
                                           : &s_reset_alerted[i];
    uint32_t key = i < 2 && s_ai_provider ? PERSIST_CODEX_RESET_ALERTED(i)
                                          : PERSIST_RESET_ALERTED(i);
    int32_t remaining = reset - (int32_t)now;
    bool within = remaining > 0 && remaining <= RESET_ALERT_SEC;
    if (within == *alerted) continue;
    if (within) crossed = true;
    *alerted = within;
    persist_write_bool(key, within);
  }

  if (crossed && !quiet_time_is_active()) vibes_long_pulse();
}

static void inbox_received(DictionaryIterator *it, void *context) {
  apply_reset_only(it, MESSAGE_KEY_CLAUDE_5H_RESET, 0);
  apply_reset_only(it, MESSAGE_KEY_CLAUDE_WK_RESET, 1);
  apply_codex_quota(it, MESSAGE_KEY_CODEX_5H_PCT, MESSAGE_KEY_CODEX_5H_RESET, 0);
  apply_codex_quota(it, MESSAGE_KEY_CODEX_WK_PCT, MESSAGE_KEY_CODEX_WK_RESET, 1);
  apply_quota(it, MESSAGE_KEY_MINIMAX_5H_PCT, MESSAGE_KEY_MINIMAX_5H_RESET, 2);
  apply_quota(it, MESSAGE_KEY_MINIMAX_WK_PCT, MESSAGE_KEY_MINIMAX_WK_RESET, 3);

  Tuple *github = dict_find(it, MESSAGE_KEY_GITHUB_TODAY_COMMITS);
  if (github) {
    store_int(PERSIST_GITHUB_COMMITS, &s_github_commits, github->value->int32);
    store_int(PERSIST_GITHUB_SYNC, &s_github_sync, (int32_t)time(NULL));
  }
  Tuple *github_latest = dict_find(it, MESSAGE_KEY_GITHUB_LATEST_COMMIT_AT);
  if (github_latest) {
    store_int(PERSIST_GITHUB_LATEST, &s_github_latest,
              github_latest->value->int32);
  }

  Tuple *bank_balance = dict_find(it, MESSAGE_KEY_BANK_BALANCE_CENTS);
  Tuple *bank_updated = dict_find(it, MESSAGE_KEY_BANK_UPDATED_AT);
  if (bank_balance && bank_updated &&
      bank_updated->value->int32 >= s_bank_updated) {
    store_int(PERSIST_BANK_BALANCE, &s_bank_balance_cents,
              bank_balance->value->int32);
    store_int(PERSIST_BANK_UPDATED, &s_bank_updated,
              bank_updated->value->int32);
  }

  Tuple *cmb_balance = dict_find(it, MESSAGE_KEY_CMB_BALANCE_CENTS);
  Tuple *cmb_delta = dict_find(it, MESSAGE_KEY_CMB_BALANCE_DELTA_CENTS);
  Tuple *cmb_event_id = dict_find(it, MESSAGE_KEY_CMB_EVENT_ID);
  Tuple *cmb_event_at = dict_find(it, MESSAGE_KEY_CMB_EVENT_AT);
  if (cmb_balance && cmb_event_at &&
      cmb_event_at->value->int32 >= s_cmb_updated) {
    int32_t at = cmb_event_at->value->int32;
    store_int(PERSIST_CMB_BALANCE, &s_cmb_balance_cents,
              cmb_balance->value->int32);
    store_int(PERSIST_CMB_UPDATED, &s_cmb_updated, at);
    store_int(PERSIST_CMB_BASELINE_AT, &s_cmb_baseline_at, at);
  } else if (cmb_delta && cmb_event_id && cmb_event_at &&
             s_cmb_baseline_at > 0) {
    int32_t id = cmb_event_id->value->int32;
    int32_t at = cmb_event_at->value->int32;
    if (id && at > s_cmb_baseline_at && !cmb_event_seen(id)) {
      int64_t next = (int64_t)s_cmb_balance_cents + cmb_delta->value->int32;
      if (next >= INT32_MIN && next <= INT32_MAX) {
        store_int(PERSIST_CMB_BALANCE, &s_cmb_balance_cents, (int32_t)next);
        store_int(PERSIST_CMB_UPDATED, &s_cmb_updated, at);
        remember_cmb_event(id);
      }
    }
  }

  Tuple *icbc_balance = dict_find(it, MESSAGE_KEY_ICBC_BALANCE_CENTS);
  Tuple *icbc_updated = dict_find(it, MESSAGE_KEY_ICBC_UPDATED_AT);
  if (icbc_balance && icbc_updated &&
      icbc_updated->value->int32 >= s_icbc_updated) {
    store_int(PERSIST_ICBC_BALANCE, &s_icbc_balance_cents,
              icbc_balance->value->int32);
    store_int(PERSIST_ICBC_UPDATED, &s_icbc_updated,
              icbc_updated->value->int32);
  }

  apply_weather(it, MESSAGE_KEY_WX_TEMP_NOW, MESSAGE_KEY_WX_CODE_NOW, 0);
  apply_weather(it, MESSAGE_KEY_WX_TEMP_6H, MESSAGE_KEY_WX_CODE_6H, 1);
  apply_weather(it, MESSAGE_KEY_WX_TEMP_24H, MESSAGE_KEY_WX_CODE_24H, 2);
  Tuple *sunrise = dict_find(it, MESSAGE_KEY_WX_SUNRISE);
  Tuple *sunset = dict_find(it, MESSAGE_KEY_WX_SUNSET);
  if (sunrise) store_int(PERSIST_SUNRISE, &s_sunrise, sunrise->value->int32);
  if (sunset) store_int(PERSIST_SUNSET, &s_sunset, sunset->value->int32);
  Tuple *next_event_start = dict_find(it, MESSAGE_KEY_NEXT_EVENT_START);
  Tuple *next_event_title = dict_find(it, MESSAGE_KEY_NEXT_EVENT_TITLE);
  if (next_event_start) {
    store_int(PERSIST_NEXT_EVENT_START, &s_next_event_start,
              next_event_start->value->int32);
  }
  if (next_event_title) {
    snprintf(s_next_event_title, sizeof(s_next_event_title), "%s",
             next_event_title->value->cstring);
    persist_write_string(PERSIST_NEXT_EVENT_TITLE, s_next_event_title);
  }

  Tuple *target = dict_find(it, MESSAGE_KEY_TARGET_DATE);
  if (target) store_int(PERSIST_TARGET, &s_target_date, target->value->int32);

  // Appearance settings; anything present overrides the persisted value.
  Tuple *theme = dict_find(it, MESSAGE_KEY_THEME);
  if (theme) store_int(PERSIST_THEME, &s_theme, theme->value->int32);
  Tuple *time_font = dict_find(it, MESSAGE_KEY_TIME_FONT);
  if (time_font) store_int(PERSIST_TIME_FONT, &s_time_font, time_font->value->int32);
  Tuple *provider = dict_find(it, MESSAGE_KEY_AI_PROVIDER);
  if (provider) {
    int32_t value = provider->value->int32 ? 1 : 0;
    store_int(PERSIST_AI_PROVIDER, &s_ai_provider, value);
  }

  // Only a quota push counts as a sync — a weather-only message says nothing
  // about how fresh the percentages are. Claude does not sync live, while
  // Codex and MiniMax are tracked independently.
  time_t now = time(NULL);
  if (dict_find(it, MESSAGE_KEY_CODEX_5H_PCT) || dict_find(it, MESSAGE_KEY_CODEX_WK_PCT)) {
    store_int(PERSIST_LAST_SYNC(0), &s_last_sync[0], (int32_t)now);
  }
  if (dict_find(it, MESSAGE_KEY_MINIMAX_5H_PCT) || dict_find(it, MESSAGE_KEY_MINIMAX_WK_PCT)) {
    store_int(PERSIST_LAST_SYNC(1), &s_last_sync[1], (int32_t)now);
  }

  s_refresh_pending = false;
  layer_mark_dirty(s_status_layer);
  check_quota_alerts();
  check_reset_alerts(now);
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
  for (int i = 0; i < 2; i++) {
    s_codex_pct[i] = persist_exists(PERSIST_CODEX_PCT(i))
                       ? persist_read_int(PERSIST_CODEX_PCT(i)) : -1;
    s_codex_reset[i] = persist_exists(PERSIST_CODEX_RESET(i))
                         ? persist_read_int(PERSIST_CODEX_RESET(i)) : 0;
  }
  for (int i = 0; i < WX_COUNT; i++) {
    s_temp[i] = persist_exists(PERSIST_TEMP(i)) ? persist_read_int(PERSIST_TEMP(i)) : NO_TEMP;
    s_code[i] = persist_exists(PERSIST_CODE(i)) ? persist_read_int(PERSIST_CODE(i)) : -1;
  }
  s_sunrise = persist_exists(PERSIST_SUNRISE) ? persist_read_int(PERSIST_SUNRISE) : 0;
  s_sunset = persist_exists(PERSIST_SUNSET) ? persist_read_int(PERSIST_SUNSET) : 0;
  s_next_event_start = persist_exists(PERSIST_NEXT_EVENT_START)
                         ? persist_read_int(PERSIST_NEXT_EVENT_START) : 0;
  if (persist_exists(PERSIST_NEXT_EVENT_TITLE)) {
    persist_read_string(PERSIST_NEXT_EVENT_TITLE, s_next_event_title,
                        sizeof(s_next_event_title));
  } else {
    s_next_event_title[0] = '\0';
  }
  for (int p = 0; p < 2; p++) {
    s_last_sync[p] = persist_exists(PERSIST_LAST_SYNC(p)) ? persist_read_int(PERSIST_LAST_SYNC(p)) : 0;
  }
  s_target_date = persist_exists(PERSIST_TARGET) ? persist_read_int(PERSIST_TARGET) : 0;
  s_theme = persist_exists(PERSIST_THEME) ? persist_read_int(PERSIST_THEME) : 0;
  s_time_font = persist_exists(PERSIST_TIME_FONT) ? persist_read_int(PERSIST_TIME_FONT) : 0;
  s_ai_provider = persist_exists(PERSIST_AI_PROVIDER)
                    ? (persist_read_int(PERSIST_AI_PROVIDER) ? 1 : 0) : 0;
  s_github_commits = persist_exists(PERSIST_GITHUB_COMMITS)
                       ? persist_read_int(PERSIST_GITHUB_COMMITS) : -1;
  s_github_sync = persist_exists(PERSIST_GITHUB_SYNC)
                    ? persist_read_int(PERSIST_GITHUB_SYNC) : 0;
  s_github_latest = persist_exists(PERSIST_GITHUB_LATEST)
                      ? persist_read_int(PERSIST_GITHUB_LATEST) : 0;
  s_bank_balance_cents = persist_exists(PERSIST_BANK_BALANCE)
                           ? persist_read_int(PERSIST_BANK_BALANCE) : -1;
  s_bank_updated = persist_exists(PERSIST_BANK_UPDATED)
                     ? persist_read_int(PERSIST_BANK_UPDATED) : 0;
  s_cmb_balance_cents = persist_exists(PERSIST_CMB_BALANCE)
                          ? persist_read_int(PERSIST_CMB_BALANCE) : -1;
  s_cmb_updated = persist_exists(PERSIST_CMB_UPDATED)
                    ? persist_read_int(PERSIST_CMB_UPDATED) : 0;
  s_cmb_baseline_at = persist_exists(PERSIST_CMB_BASELINE_AT)
                        ? persist_read_int(PERSIST_CMB_BASELINE_AT) : 0;
  s_cmb_event_index = persist_exists(PERSIST_CMB_EVENT_INDEX)
                        ? persist_read_int(PERSIST_CMB_EVENT_INDEX) : 0;
  for (int i = 0; i < CMB_EVENT_HISTORY; i++) {
    s_cmb_event_ids[i] = persist_exists(PERSIST_CMB_EVENT(i))
                           ? persist_read_int(PERSIST_CMB_EVENT(i)) : 0;
  }
  s_icbc_balance_cents = persist_exists(PERSIST_ICBC_BALANCE)
                           ? persist_read_int(PERSIST_ICBC_BALANCE) : -1;
  s_icbc_updated = persist_exists(PERSIST_ICBC_UPDATED)
                     ? persist_read_int(PERSIST_ICBC_UPDATED) : 0;
  for (int i = 0; i < ROW_COUNT; i++) {
    s_alerted[i] = persist_exists(PERSIST_ALERTED(i)) && persist_read_bool(PERSIST_ALERTED(i));
    s_reset_alerted[i] = persist_exists(PERSIST_RESET_ALERTED(i)) &&
                          persist_read_bool(PERSIST_RESET_ALERTED(i));
  }
  for (int i = 0; i < 2; i++) {
    s_codex_alerted[i] = persist_exists(PERSIST_CODEX_ALERTED(i)) &&
                          persist_read_bool(PERSIST_CODEX_ALERTED(i));
    s_codex_reset_alerted[i] = persist_exists(PERSIST_CODEX_RESET_ALERTED(i)) &&
                                persist_read_bool(PERSIST_CODEX_RESET_ALERTED(i));
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
  s_bank_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_BANK_LABELS_11));
  s_lunar_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LUNAR_LABELS_11));

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

  // Days until the next March 28, directly below the battery indicator.
  s_birthday_layer = make_text(root,
                               GRect(BIRTHDAY_X, BIRTHDAY_Y, BIRTHDAY_W, CORNER_H),
                               fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                               GTextAlignmentLeft, THEMES[s_theme].fg_dim);
  text_layer_set_text(s_birthday_layer, "");

  // Just the number, per the brief — the target date lives in the phone's
  // settings page and is the only place it needs spelling out.
  s_dday_layer = make_text(root, GRect(DDAY_X, DDAY_Y, DDAY_W, CORNER_H),
                           fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                           GTextAlignmentRight, THEMES[s_theme].fg);
  text_layer_set_text(s_dday_layer, "");

  s_separators[0] = separator(root, SEP1_Y);

  s_wx_icons_layer = layer_create(GRect(0, WX_Y, SCREEN_W, WX_H));
  layer_set_update_proc(s_wx_icons_layer, wx_icons_update_proc);
  layer_add_child(root, s_wx_icons_layer);

  for (int i = 0; i < WX_COUNT; i++) {
    int x = i * WX_COL_W;
    s_wx_temp_layers[i] = make_text(root,
                                    GRect(x + WX_TEMP_X, WX_Y,
                                          WX_TEMP_W, WX_TEMP_H),
                                    fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                                    GTextAlignmentCenter, THEMES[s_theme].fg);
    text_layer_set_text(s_wx_temp_layers[i], "--");
  }

  s_separators[1] = separator(root, SEP_WX_Y);

  s_bank_meta_layer = make_text(root,
                                GRect(BANK_META_X, BANK_Y + 3, BANK_META_W, BANK_H),
                                s_bank_font,
                                GTextAlignmentLeft, THEMES[s_theme].fg_dim);
  text_layer_set_text(s_bank_meta_layer, "南京银行");

  s_bank_balance_layer = make_text(root,
                                   GRect(BANK_VALUE_X, BANK_Y - 2,
                                         BANK_VALUE_W, BANK_H + 3),
                                   fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                                   GTextAlignmentRight, THEMES[s_theme].fg);
  text_layer_set_text(s_bank_balance_layer, "--");

  s_bank_sum_layer = make_text(root,
                               GRect(BANK_SUM_X, BANK_Y - 2, BANK_SUM_W, BANK_H + 3),
                               fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                               GTextAlignmentRight, THEMES[s_theme].fg);
  text_layer_set_text(s_bank_sum_layer, "--");

  s_separators[2] = separator(root, SEP2_Y);

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
  s_whimsy = (uint32_t)time(NULL) ^ 0xa1c0deu;
  s_bank_timer = app_timer_register(next_whimsy_delay(), bank_timer_handler, NULL);
  s_date_timer = app_timer_register(next_whimsy_delay(), date_timer_handler, NULL);
  s_weather_timer = app_timer_register(next_whimsy_delay(), weather_timer_handler, NULL);
}

static void window_unload(Window *window) {
  if (s_bank_timer) {
    app_timer_cancel(s_bank_timer);
    s_bank_timer = NULL;
  }
  if (s_date_timer) {
    app_timer_cancel(s_date_timer);
    s_date_timer = NULL;
  }
  if (s_weather_timer) {
    app_timer_cancel(s_weather_timer);
    s_weather_timer = NULL;
  }
  for (int i = 0; i < ROW_COUNT; i++) {
    text_layer_destroy(s_label_layers[i]);
    text_layer_destroy(s_pct_layers[i]);
    text_layer_destroy(s_reset_layers[i]);
  }
  for (int i = 0; i < WX_COUNT; i++) {
    text_layer_destroy(s_wx_temp_layers[i]);
  }
  layer_destroy(s_wx_icons_layer);
  text_layer_destroy(s_bank_meta_layer);
  text_layer_destroy(s_bank_balance_layer);
  text_layer_destroy(s_bank_sum_layer);
  fonts_unload_custom_font(s_lunar_font);
  fonts_unload_custom_font(s_bank_font);
  fonts_unload_custom_font(s_consolas_font);
  layer_destroy(s_rows_layer);
  layer_destroy(s_status_layer);
  text_layer_destroy(s_separators[0]);
  text_layer_destroy(s_separators[1]);
  text_layer_destroy(s_separators[2]);
  text_layer_destroy(s_dday_layer);
  text_layer_destroy(s_birthday_layer);
  text_layer_destroy(s_countdown_layer);
  text_layer_destroy(s_date_layer);
  text_layer_destroy(s_time_layer);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_clock(tick_time);
  update_countdown(tick_time);
  time_t n = time(NULL);
  update_dday(n);
  update_birthday(n);
  advance_claude_reset(n);
  update_rows(n);

  // Nudge the phone as soon as a displayed live provider would otherwise go
  // grey. Claude is locally derived and therefore excluded.
  if (data_is_stale(n, 1) || (s_ai_provider && data_is_stale(n, 0)) ||
      s_github_sync <= 0 || (int32_t)n - s_github_sync > STALE_AFTER_SEC) {
    request_refresh();
  }

  check_reset_alerts(n);
}

// Flick of the wrist forces an immediate fetch.
static void tap_handler(AccelAxisType axis, int32_t direction) {
  request_refresh();
}

// The select button switches the 7D row between Claude and Codex. GitHub stays
// fixed in row 0; both AI providers keep independent persisted 7D data.
static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_ai_provider = s_ai_provider ? 0 : 1;
  persist_write_int(PERSIST_AI_PROVIDER, s_ai_provider);
  apply_theme();
  update_ui();

  if (!connection_service_peek_pebble_app_connection()) return;
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) return;
  dict_write_uint8(out, MESSAGE_KEY_AI_PROVIDER, (uint8_t)s_ai_provider);
  app_message_outbox_send();
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
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
  // s_pct[0]/[1] (Claude) are intentionally left at -1 — the bar/number for
  // those two rows is computed from s_reset via claude_elapsed_pct(), not
  // pushed data.
  s_reset[0] = n + 2 * 3600 + 13 * 60;
  s_reset[1] = n + 4 * 86400 + 5 * 3600;
  s_codex_pct[0] = 24; s_codex_reset[0] = n + 3 * 3600;
  s_codex_pct[1] = 61; s_codex_reset[1] = n + 2 * 86400;
  s_pct[2] = 100; s_reset[2] = n + 47 * 60;
  s_pct[3] = 6;   s_reset[3] = n + 6 * 86400;
  // Codes chosen to exercise three different icon shapes at once; change them
  // to 2 / 45 / 71 / 95 to eyeball the rest.
  s_temp[0] = 21; s_code[0] = 2;    // partly cloudy
  s_temp[1] = 19; s_code[1] = 51;   // drizzle
  s_temp[2] = -8; s_code[2] = -1;   // unknown
#endif

  s_window = window_create();
  window_set_click_config_provider(s_window, click_config_provider);
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
