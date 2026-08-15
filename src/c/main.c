#include <pebble.h>

// Define DEMO_DATA to preload sample values for checking the layout in the emulator.

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
#define PERSIST_PCT(i)    (100 + (i) * 2)
#define PERSIST_RESET(i)  (100 + (i) * 2 + 1)
#define PERSIST_TEMP(i)   (200 + (i) * 2)
#define PERSIST_CODE(i)   (200 + (i) * 2 + 1)
#define PERSIST_LAST_SYNC 300

// Quota values older than this are shown greyed out: the phone is reachable but
// the numbers behind them are no longer trustworthy.
#define STALE_AFTER_SEC (20 * 60)

static int32_t s_pct[ROW_COUNT];     // -1 = no data yet
static int32_t s_reset[ROW_COUNT];   // absolute UTC epoch seconds of next reset
static int32_t s_temp[WX_COUNT];     // NO_TEMP = no data yet
static int32_t s_code[WX_COUNT];     // WMO weather code
static int32_t s_last_sync;          // UTC epoch of the last quota push, 0 = never
static bool s_refresh_pending;       // a refresh was asked for and hasn't landed yet

static Window *s_window;
static TextLayer *s_time_layer;
static TextLayer *s_date_layer;
static TextLayer *s_countdown_layer;
static TextLayer *s_wx_label_layers[WX_COUNT];
static TextLayer *s_wx_temp_layers[WX_COUNT];
static TextLayer *s_wx_cond_layers[WX_COUNT];
static TextLayer *s_separators[2];
static Layer *s_status_layer;
static Layer *s_rows_layer;
static TextLayer *s_label_layers[ROW_COUNT];
static TextLayer *s_pct_layers[ROW_COUNT];
static TextLayer *s_reset_layers[ROW_COUNT];

static char s_time_buf[8];
static char s_date_buf[16];
static char s_countdown_buf[10];
static char s_wx_temp_buf[WX_COUNT][8];
static char s_pct_buf[ROW_COUNT][8];
static char s_reset_buf[ROW_COUNT][10];

// ------------------------------------------------------------------- layout

#define TIME_Y        -8
#define TIME_H        46

#define DATE_Y        36
#define DATE_H        20
#define DATE_X        3
#define DATE_W        90
#define CD_W          48

#define SEP1_Y        57
#define WX_Y          58
#define WX_COL_W      (SCREEN_W / WX_COUNT)
#define WX_LABEL_H    15
#define WX_TEMP_DY    11
#define WX_TEMP_H     21
#define WX_COND_DY    28
#define WX_COND_H     15
#define SEP2_Y        101

#define ROWS_Y        103
#define ROW_H         16
#define BAR_H         2
#define R_LABEL_X     3
#define R_LABEL_W     38
#define R_PCT_W       46
#define R_RESET_W     54

// ------------------------------------------------------------- weather codes

// WMO weather interpretation codes, shortened to fit a 48px column.
static const char *wmo_text(int32_t code) {
  if (code < 0) return "--";
  if (code == 0) return "Clear";
  if (code == 1) return "Fair";
  if (code == 2) return "PtCld";
  if (code == 3) return "Cloud";
  if (code == 45 || code == 48) return "Fog";
  if (code >= 51 && code <= 57) return "Drizl";
  if (code >= 61 && code <= 67) return "Rain";
  if (code >= 71 && code <= 77) return "Snow";
  if (code >= 80 && code <= 82) return "Shwr";
  if (code == 85 || code == 86) return "Snow";
  if (code >= 95) return "Storm";
  return "?";
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

static bool data_is_stale(time_t now) {
  return s_last_sync <= 0 || (int32_t)now - s_last_sync > STALE_AFTER_SEC;
}

static void update_rows(time_t now) {
  bool stale = data_is_stale(now);

  for (int i = 0; i < ROW_COUNT; i++) {
    if (s_pct[i] < 0) {
      snprintf(s_pct_buf[i], sizeof(s_pct_buf[i]), "--");
      text_layer_set_text_color(s_pct_layers[i], GColorDarkGray);
    } else {
      int32_t p = s_pct[i];
      if (p > 100) p = 100;
      snprintf(s_pct_buf[i], sizeof(s_pct_buf[i]), "%ld%%", (long)p);
      // Grey means "this number may have moved since we last heard from the phone".
      text_layer_set_text_color(s_pct_layers[i],
                                stale ? GColorLightGray : (p >= 90 ? GColorRed : GColorWhite));
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
    text_layer_set_text(s_wx_cond_layers[i], wmo_text(s_temp[i] == NO_TEMP ? -1 : s_code[i]));
  }
}

// Minutes until the next 22:00 local, which is what the countdown line shows.
static void update_countdown(struct tm *t) {
  int mins_left = 22 * 60 - (t->tm_hour * 60 + t->tm_min);
  if (mins_left < 0) mins_left += 24 * 60;
  fmt_remaining(s_countdown_buf, sizeof(s_countdown_buf), mins_left * 60);
  text_layer_set_text(s_countdown_layer, s_countdown_buf);
}

static void update_clock(struct tm *t) {
  strftime(s_time_buf, sizeof(s_time_buf),
           clock_is_24h_style() ? "%H:%M" : "%I:%M", t);
  // Drop a leading zero in 12h mode so the big font has room.
  if (!clock_is_24h_style() && s_time_buf[0] == '0') {
    memmove(s_time_buf, s_time_buf + 1, strlen(s_time_buf));
  }
  text_layer_set_text(s_time_layer, s_time_buf);

  strftime(s_date_buf, sizeof(s_date_buf), "%a %m-%d", t);
  text_layer_set_text(s_date_layer, s_date_buf);
}

static void update_ui(void) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  update_clock(t);
  update_countdown(t);
  update_weather();
  update_rows(now);
}

// -------------------------------------------------------------------- render

static void rows_update_proc(Layer *layer, GContext *ctx) {
  int bar_w = SCREEN_W - R_LABEL_X * 2;

  for (int i = 0; i < ROW_COUNT; i++) {
    int y = i * ROW_H + ROW_H - BAR_H;

    graphics_context_set_fill_color(ctx, GColorDarkGray);
    graphics_fill_rect(ctx, GRect(R_LABEL_X, y, bar_w, BAR_H), 0, GCornerNone);

    if (s_pct[i] >= 0) {
      int32_t p = s_pct[i] > 100 ? 100 : s_pct[i];
      graphics_context_set_fill_color(ctx, row_color(i));
      graphics_fill_rect(ctx, GRect(R_LABEL_X, y, (bar_w * (int)p) / 100, BAR_H), 0, GCornerNone);
    }
  }
}

// A dot in the top-right corner: red when the phone is out of range (nothing can
// refresh until it is back), amber while a refresh is in flight, hidden otherwise.
static void status_update_proc(Layer *layer, GContext *ctx) {
  GColor color;
  if (!connection_service_peek_pebble_app_connection()) {
    color = GColorRed;
  } else if (s_refresh_pending) {
    color = GColorYellow;
  } else {
    return;
  }
  graphics_context_set_fill_color(ctx, color);
  graphics_fill_circle(ctx, GPoint(3, 3), 3);
}

// A 1px rule: an empty TextLayer with a background is cheaper than another
// custom Layer with its own update_proc.
static TextLayer *separator(Layer *root, int y) {
  TextLayer *tl = text_layer_create(GRect(0, y, SCREEN_W, 1));
  text_layer_set_background_color(tl, GColorDarkGray);
  layer_add_child(root, text_layer_get_layer(tl));
  return tl;
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

static void inbox_received(DictionaryIterator *it, void *context) {
  apply_quota(it, MESSAGE_KEY_CLAUDE_5H_PCT,  MESSAGE_KEY_CLAUDE_5H_RESET,  0);
  apply_quota(it, MESSAGE_KEY_CLAUDE_WK_PCT,  MESSAGE_KEY_CLAUDE_WK_RESET,  1);
  apply_quota(it, MESSAGE_KEY_MINIMAX_5H_PCT, MESSAGE_KEY_MINIMAX_5H_RESET, 2);
  apply_quota(it, MESSAGE_KEY_MINIMAX_WK_PCT, MESSAGE_KEY_MINIMAX_WK_RESET, 3);

  apply_weather(it, MESSAGE_KEY_WX_TEMP_NOW, MESSAGE_KEY_WX_CODE_NOW, 0);
  apply_weather(it, MESSAGE_KEY_WX_TEMP_6H,  MESSAGE_KEY_WX_CODE_6H,  1);
  apply_weather(it, MESSAGE_KEY_WX_TEMP_24H, MESSAGE_KEY_WX_CODE_24H, 2);

  // Only a quota push counts as a sync — a weather-only message says nothing
  // about how fresh the percentages are.
  if (dict_find(it, MESSAGE_KEY_CLAUDE_5H_PCT) || dict_find(it, MESSAGE_KEY_MINIMAX_5H_PCT)) {
    store_int(PERSIST_LAST_SYNC, &s_last_sync, (int32_t)time(NULL));
  }

  s_refresh_pending = false;
  layer_mark_dirty(s_status_layer);
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
  s_last_sync = persist_exists(PERSIST_LAST_SYNC) ? persist_read_int(PERSIST_LAST_SYNC) : 0;
}

// ------------------------------------------------------------------- window

static TextLayer *make_text(Layer *root, GRect frame, const char *font_key,
                            GTextAlignment align, GColor color) {
  TextLayer *tl = text_layer_create(frame);
  text_layer_set_font(tl, fonts_get_system_font(font_key));
  text_layer_set_text_alignment(tl, align);
  text_layer_set_background_color(tl, GColorClear);
  text_layer_set_text_color(tl, color);
  layer_add_child(root, text_layer_get_layer(tl));
  return tl;
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  window_set_background_color(window, GColorBlack);

  s_time_layer = make_text(root, GRect(0, TIME_Y, SCREEN_W, TIME_H),
                           FONT_KEY_BITHAM_42_BOLD, GTextAlignmentCenter, GColorWhite);

  s_date_layer = make_text(root, GRect(DATE_X, DATE_Y, DATE_W, DATE_H),
                           FONT_KEY_GOTHIC_18_BOLD, GTextAlignmentLeft, GColorLightGray);

  // Countdown to 22:00, in yellow so it reads as a deadline rather than a clock.
  s_countdown_layer = make_text(root, GRect(SCREEN_W - CD_W - DATE_X, DATE_Y, CD_W, DATE_H),
                                FONT_KEY_GOTHIC_18_BOLD, GTextAlignmentRight, GColorYellow);

  s_status_layer = layer_create(GRect(SCREEN_W - 10, 3, 7, 7));
  layer_set_update_proc(s_status_layer, status_update_proc);
  layer_add_child(root, s_status_layer);

  s_separators[0] = separator(root, SEP1_Y);

  for (int i = 0; i < WX_COUNT; i++) {
    int x = i * WX_COL_W;
    s_wx_label_layers[i] = make_text(root, GRect(x, WX_Y, WX_COL_W, WX_LABEL_H),
                                     FONT_KEY_GOTHIC_14, GTextAlignmentCenter, GColorDarkGray);
    text_layer_set_text(s_wx_label_layers[i], WX_LABELS[i]);

    s_wx_temp_layers[i] = make_text(root, GRect(x, WX_Y + WX_TEMP_DY, WX_COL_W, WX_TEMP_H),
                                    FONT_KEY_GOTHIC_18_BOLD, GTextAlignmentCenter, GColorWhite);
    text_layer_set_text(s_wx_temp_layers[i], "--");

    s_wx_cond_layers[i] = make_text(root, GRect(x, WX_Y + WX_COND_DY, WX_COL_W, WX_COND_H),
                                    FONT_KEY_GOTHIC_14, GTextAlignmentCenter, GColorLightGray);
    text_layer_set_text(s_wx_cond_layers[i], "--");
  }

  s_separators[1] = separator(root, SEP2_Y);

  s_rows_layer = layer_create(GRect(0, ROWS_Y, SCREEN_W, ROW_H * ROW_COUNT));
  layer_set_update_proc(s_rows_layer, rows_update_proc);
  layer_add_child(root, s_rows_layer);

  for (int i = 0; i < ROW_COUNT; i++) {
    int y = i * ROW_H;

    s_label_layers[i] = make_text(s_rows_layer, GRect(R_LABEL_X, y, R_LABEL_W, ROW_H),
                                  FONT_KEY_GOTHIC_14_BOLD, GTextAlignmentLeft, row_color(i));
    text_layer_set_text(s_label_layers[i], ROWS[i].label);

    s_pct_layers[i] = make_text(s_rows_layer, GRect(R_LABEL_X + R_LABEL_W, y - 3, R_PCT_W, ROW_H + 3),
                                FONT_KEY_GOTHIC_18_BOLD, GTextAlignmentRight, GColorWhite);
    text_layer_set_text(s_pct_layers[i], "--");

    s_reset_layers[i] = make_text(s_rows_layer,
                                  GRect(R_LABEL_X + R_LABEL_W + R_PCT_W, y, R_RESET_W, ROW_H),
                                  FONT_KEY_GOTHIC_14, GTextAlignmentRight, GColorLightGray);
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
    text_layer_destroy(s_wx_cond_layers[i]);
  }
  layer_destroy(s_rows_layer);
  layer_destroy(s_status_layer);
  text_layer_destroy(s_separators[0]);
  text_layer_destroy(s_separators[1]);
  text_layer_destroy(s_countdown_layer);
  text_layer_destroy(s_date_layer);
  text_layer_destroy(s_time_layer);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_clock(tick_time);
  update_countdown(tick_time);
  update_rows(time(NULL));

  // Nudge the phone as soon as the data would otherwise go grey, so a watchface
  // left on the wrist keeps itself current without waiting for the JS interval.
  if (data_is_stale(time(NULL))) request_refresh();
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

static void init(void) {
  load_persisted();
#ifdef DEMO_DATA
  time_t n = time(NULL);
  s_pct[0] = 87;  s_reset[0] = n + 2 * 3600 + 13 * 60;
  s_pct[1] = 42;  s_reset[1] = n + 4 * 86400 + 5 * 3600;
  s_pct[2] = 100; s_reset[2] = n + 47 * 60;
  s_pct[3] = 6;   s_reset[3] = n + 6 * 86400;
  s_temp[0] = 21; s_code[0] = 0;
  s_temp[1] = 19; s_code[1] = 61;
  s_temp[2] = -8; s_code[2] = 3;
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

  window_stack_push(s_window, true);
  request_refresh();
}

static void deinit(void) {
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
