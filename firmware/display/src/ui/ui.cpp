#include "ui.h"
#include "state.h"
#include "bsp/bsp_display.h"
#include "bsp/bsp_audio.h"
#include "net/net_rest.h"
#include "net/net_events.h"
#include "net/player.h"
#include "net/live.h"
#include <time.h>
#include <LittleFS.h>

// ---------- 命令队列 ----------
enum CmdType {
  CMD_REFRESH_HOME,
  CMD_DRINKING_STARTED,
  CMD_CAT_CONFIRMED,
  CMD_CAT_UNKNOWN,
  CMD_EVENT_DISCARDED,
  CMD_OFFLINE,
  CMD_PLAYER_END,
  CMD_PLAYER_ERROR,
};
struct Cmd {
  CmdType type;
  int event_id = -1;
  char cat_id[32] = {0};
  char text[48] = {0};
  bool flag = false;
};
static QueueHandle_t cmd_q = NULL;

static void post(const Cmd &c) {
  if (cmd_q) xQueueSend(cmd_q, &c, 0);
}
void ui_cmd_refresh_home() { Cmd c{CMD_REFRESH_HOME}; post(c); }
void ui_cmd_drinking_started(int id) { Cmd c{CMD_DRINKING_STARTED}; c.event_id = id; post(c); }
void ui_cmd_cat_confirmed(int id, const String &cat) {
  Cmd c{CMD_CAT_CONFIRMED};
  c.event_id = id;
  strncpy(c.cat_id, cat.c_str(), sizeof(c.cat_id) - 1);
  post(c);
}
void ui_cmd_cat_unknown(int id) { Cmd c{CMD_CAT_UNKNOWN}; c.event_id = id; post(c); }
void ui_cmd_event_discarded(int id) { Cmd c{CMD_EVENT_DISCARDED}; c.event_id = id; post(c); }
void ui_cmd_set_offline(bool off, const char *reason) {
  Cmd c{CMD_OFFLINE};
  c.flag = off;
  if (reason) strncpy(c.text, reason, sizeof(c.text) - 1);
  post(c);
}
void ui_cmd_player_end() { Cmd c{CMD_PLAYER_END}; post(c); }
void ui_cmd_player_error() { Cmd c{CMD_PLAYER_ERROR}; post(c); }

// ---------- 帧缓冲 ----------
static uint16_t *player_bufs[2] = {nullptr, nullptr};
static uint16_t *live_bufs[2] = {nullptr, nullptr};
static volatile int player_show_idx = 0, live_show_idx = 0;
static volatile bool player_dirty = false, live_dirty = false;

uint16_t *ui_player_write_buffer() { return player_bufs[1 - player_show_idx]; }
uint16_t *ui_live_write_buffer() { return live_bufs[1 - live_show_idx]; }
void ui_player_commit_frame() { player_dirty = true; }
void ui_live_commit_frame() { live_dirty = true; }

// ---------- 屏幕对象 ----------
static lv_obj_t *scr_home, *scr_clips, *scr_player, *scr_live, *scr_offline;
static lv_style_t st_text, st_title;
static const lv_font_t *font_cjk = nullptr;

static lv_obj_t *home_grid;
static lv_obj_t *home_empty;
static lv_obj_t *lbl_clock, *lbl_status;
static lv_obj_t *banner, *banner_img, *banner_lbl, *banner_spinner, *banner_arc;
static lv_obj_t *clips_title, *clips_list;
static lv_obj_t *player_canvas, *player_lbl_status, *player_btn_pause, *player_lbl_pause;
static lv_obj_t *live_canvas, *live_lbl;
static lv_obj_t *offline_lbl_reason;
static lv_obj_t *cat_btns[4], *cat_imgs[4], *cat_badges[4], *cat_badge_lbls[4], *cat_names[4];

static String cur_cat_id;
static bool player_paused = false;
static lv_timer_t *sec_timer = nullptr;

static const lv_font_t *ui_font() { return font_cjk ? font_cjk : &lv_font_montserrat_20; }

// ---------- 小工具 ----------
static void style_init() {
  lv_style_init(&st_text);
  lv_style_set_text_font(&st_text, ui_font());
  lv_style_set_text_color(&st_text, lv_color_white());
  lv_style_init(&st_title);
  lv_style_set_text_font(&st_title, ui_font());
  lv_style_set_text_color(&st_title, lv_color_white());
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *txt, lv_style_t *st = &st_text) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_add_style(l, st, 0);
  lv_label_set_text(l, txt);
  return l;
}

// ---------- 横幅 ----------
static void banner_anim_y(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, v); }

static void banner_show(const char *text) {
  lv_label_set_text(banner_lbl, text);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, banner);
  lv_anim_set_exec_cb(&a, banner_anim_y);
  lv_anim_set_values(&a, -90, 10);
  lv_anim_set_time(&a, 300);
  lv_anim_start(&a);
}

static void banner_hide() {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, banner);
  lv_anim_set_exec_cb(&a, banner_anim_y);
  lv_anim_set_values(&a, lv_obj_get_y(banner), -90);
  lv_anim_set_time(&a, 250);
  lv_anim_start(&a);
}

static void banner_auto_hide(uint32_t ms) {
  lv_timer_t *t = lv_timer_create([](lv_timer_t *tm) {
    banner_hide();
    lv_timer_del(tm);
  }, ms, NULL);
  lv_timer_set_repeat_count(t, 1);
}

static void banner_set_pending() {
  lv_obj_clear_flag(banner_spinner, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(banner_arc, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(banner_img, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_border_color(banner, lv_color_hex(0x3fb27f), 0);
  banner_show("有猫在喝水…");
}

// cat_unknown：确认完成但未认出是哪只猫，几秒后自动消失，不加计数
static void banner_set_unknown() {
  lv_obj_add_flag(banner_spinner, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(banner_arc, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(banner_img, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_border_color(banner, lv_color_hex(0xd9a54f), 0);
  banner_show("有猫在喝水，但没认出是哪只");
  banner_auto_hide(5000);
}

static void banner_set_confirmed(const String &cat_id) {
  CatInfo *c = find_cat(cat_id);
  String name = c ? c->name : cat_id;
  char buf[64];
  snprintf(buf, sizeof(buf), "%s 喝水 +1", name.c_str());
  lv_label_set_text(banner_lbl, buf);
  lv_obj_add_flag(banner_spinner, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(banner_arc, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_border_color(banner, lv_color_hex(0x3fb27f), 0);
  void *dsc = avatar_img_dsc(cat_id);
  if (dsc) {
    lv_img_set_src(banner_img, dsc);
    lv_obj_clear_flag(banner_img, LV_OBJ_FLAG_HIDDEN);
  }
  // 头像弹跳
  if (dsc) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, banner_img);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&a, 20, 4);
    lv_anim_set_time(&a, 280);
    lv_anim_set_playback_time(&a, 180);
    lv_anim_start(&a);
  }
  banner_show(buf);
  // 8 秒后自动收起
  banner_auto_hide(8000);
}

// ---------- 主页 ----------
static void on_cat_clicked(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  String id = idx < (int)g_state.cats.size() ? g_state.cats[idx].id : "";
  xSemaphoreGive(state_mutex);
  if (id.length()) ui_show_clips(id);
}

static void refresh_home_grid() {
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  int n = (int)g_state.cats.size();
  String ids[4], names[4];
  int counts[4];
  for (int i = 0; i < n && i < 4; i++) {
    ids[i] = g_state.cats[i].id;
    names[i] = g_state.cats[i].name;
    counts[i] = g_state.cats[i].today_count;
  }
  String date = g_state.today_date;
  xSemaphoreGive(state_mutex);

  if (n == 0) {
    lv_obj_add_flag(home_grid, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(home_empty, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_clear_flag(home_grid, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(home_empty, LV_OBJ_FLAG_HIDDEN);

  for (int i = 0; i < 4; i++) {
    if (i < n) {
      lv_obj_clear_flag(cat_btns[i], LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(cat_names[i], names[i].c_str());
      char b[8];
      snprintf(b, sizeof(b), "%d", counts[i]);
      lv_label_set_text(cat_badge_lbls[i], b);
      lv_obj_clear_flag(cat_badges[i], counts[i] > 0 ? LV_OBJ_FLAG_HIDDEN : 0);
      if (counts[i] <= 0) lv_obj_add_flag(cat_badges[i], LV_OBJ_FLAG_HIDDEN);
      void *dsc = avatar_img_dsc(ids[i]);
      if (dsc) {
        lv_img_set_src(cat_imgs[i], dsc);
        lv_obj_clear_flag(cat_imgs[i], LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(cat_imgs[i], LV_OBJ_FLAG_HIDDEN);
      }
    } else {
      lv_obj_add_flag(cat_btns[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
}

static void home_gesture(lv_event_t *e) {
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
  if (dir == LV_DIR_RIGHT) ui_show_live();
}

static void build_home() {
  scr_home = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_home, lv_color_hex(0x101418), 0);
  lv_obj_clear_flag(scr_home, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(scr_home, home_gesture, LV_EVENT_GESTURE, NULL);

  lbl_clock = make_label(scr_home, "--:--");
  lv_obj_align(lbl_clock, LV_ALIGN_TOP_LEFT, 16, 12);
  lbl_status = make_label(scr_home, "");
  lv_obj_align(lbl_status, LV_ALIGN_TOP_RIGHT, -16, 12);

  lv_obj_t *hint = make_label(scr_home, LV_SYMBOL_RIGHT " 滑动看直播");
  lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -14);

  home_empty = make_label(scr_home, "等待猫咪档案…", &st_title);
  lv_obj_align(home_empty, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(home_empty, LV_OBJ_FLAG_HIDDEN);

  home_grid = lv_obj_create(scr_home);
  lv_obj_set_size(home_grid, 440, 400);
  lv_obj_align(home_grid, LV_ALIGN_TOP_MID, 0, 44);
  lv_obj_set_style_bg_opa(home_grid, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(home_grid, 0, 0);
  lv_obj_set_style_pad_all(home_grid, 0, 0);
  lv_obj_clear_flag(home_grid, LV_OBJ_FLAG_SCROLLABLE);

  for (int i = 0; i < 4; i++) {
    int gx = (i % 2) * 220 + 10;
    int gy = (i / 2) * 200 + 0;
    lv_obj_t *cell = lv_obj_create(home_grid);
    cat_btns[i] = cell;
    lv_obj_set_size(cell, 200, 195);
    lv_obj_set_pos(cell, gx, gy);
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cell, 0, 0);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_EVENT_BUBBLE);  // 滑动手势冒泡到屏幕
    lv_obj_add_event_cb(cell, on_cat_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    // 圆形头像容器
    lv_obj_t *circle = lv_obj_create(cell);
    lv_obj_set_size(circle, 150, 150);
    lv_obj_align(circle, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle, lv_color_hex(0x2a2f36), 0);
    lv_obj_set_style_border_width(circle, 3, 0);
    lv_obj_set_style_border_color(circle, lv_color_hex(0x3fb27f), 0);
    lv_obj_set_style_pad_all(circle, 0, 0);
    lv_obj_set_style_clip_corner(circle, true, 0);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    cat_imgs[i] = lv_img_create(circle);
    lv_obj_center(cat_imgs[i]);
    lv_obj_add_flag(cat_imgs[i], LV_OBJ_FLAG_HIDDEN);

    // 次数徽章
    cat_badges[i] = lv_obj_create(cell);
    lv_obj_set_size(cat_badges[i], 44, 44);
    lv_obj_align(cat_badges[i], LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_set_style_radius(cat_badges[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cat_badges[i], lv_color_hex(0xd94f4f), 0);
    lv_obj_set_style_border_width(cat_badges[i], 0, 0);
    lv_obj_clear_flag(cat_badges[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    cat_badge_lbls[i] = make_label(cat_badges[i], "0");
    lv_obj_center(cat_badge_lbls[i]);
    lv_obj_add_flag(cat_badges[i], LV_OBJ_FLAG_HIDDEN);

    cat_names[i] = make_label(cell, "");
    lv_obj_align(cat_names[i], LV_ALIGN_BOTTOM_MID, 0, -2);
  }

  // 喝水横幅（默认隐藏）
  banner = lv_obj_create(scr_home);
  lv_obj_set_size(banner, 420, 76);
  lv_obj_set_pos(banner, 30, -90);
  lv_obj_set_style_radius(banner, 18, 0);
  lv_obj_set_style_bg_color(banner, lv_color_hex(0x22303c), 0);
  lv_obj_set_style_border_color(banner, lv_color_hex(0x3fb27f), 0);
  lv_obj_set_style_border_width(banner, 2, 0);
  lv_obj_clear_flag(banner, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  banner_spinner = lv_spinner_create(banner, 1000, 60);
  lv_obj_set_size(banner_spinner, 40, 40);
  lv_obj_align(banner_spinner, LV_ALIGN_LEFT_MID, 12, 0);

  banner_arc = lv_arc_create(banner);
  lv_obj_set_size(banner_arc, 48, 48);
  lv_obj_align(banner_arc, LV_ALIGN_LEFT_MID, 8, 0);
  lv_arc_set_rotation(banner_arc, 270);
  lv_arc_set_bg_angles(banner_arc, 0, 360);
  lv_obj_remove_style(banner_arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(banner_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(banner_arc, LV_OBJ_FLAG_HIDDEN);

  banner_img = lv_img_create(banner);
  lv_img_set_zoom(banner_img, 64);  // 192 -> 48
  lv_obj_align(banner_img, LV_ALIGN_LEFT_MID, 8, 0);
  lv_obj_add_flag(banner_img, LV_OBJ_FLAG_HIDDEN);

  banner_lbl = make_label(banner, "");
  lv_obj_align(banner_lbl, LV_ALIGN_LEFT_MID, 68, 0);
}

// ---------- 片段列表页 ----------
static void on_back_home(lv_event_t *) { ui_show_home(); }

static void on_clip_clicked(lv_event_t *e) {
  long id = (long)(intptr_t)lv_event_get_user_data(e);
  ui_show_player(id);
}

static void build_clips() {
  scr_clips = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_clips, lv_color_hex(0x101418), 0);

  lv_obj_t *back = lv_btn_create(scr_clips);
  lv_obj_set_size(back, 90, 44);
  lv_obj_align(back, LV_ALIGN_TOP_LEFT, 12, 8);
  lv_obj_add_event_cb(back, on_back_home, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bl = make_label(back, LV_SYMBOL_LEFT " 返回");
  lv_obj_center(bl);

  clips_title = make_label(scr_clips, "", &st_title);
  lv_obj_align(clips_title, LV_ALIGN_TOP_MID, 0, 16);

  clips_list = lv_obj_create(scr_clips);
  lv_obj_set_size(clips_list, 440, 390);
  lv_obj_align(clips_list, LV_ALIGN_TOP_MID, 0, 70);
  lv_obj_set_style_bg_opa(clips_list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(clips_list, 0, 0);
  lv_obj_set_flex_flow(clips_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(clips_list, 10, 0);
}

static void fill_clips_list(const std::vector<ClipInfo> &clips) {
  lv_obj_clean(clips_list);
  if (clips.empty()) {
    lv_obj_t *l = make_label(clips_list, "今天还没有片段");
    lv_obj_set_style_text_color(l, lv_color_hex(0x888888), 0);
    return;
  }
  for (auto &c : clips) {
    lv_obj_t *btn = lv_btn_create(clips_list);
    lv_obj_set_size(btn, 420, 64);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x22303c), 0);
    lv_obj_add_event_cb(btn, on_clip_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)c.id);
    time_t t = (time_t)c.start_ts;
    struct tm tmv;
    localtime_r(&t, &tmv);
    char txt[64];
    snprintf(txt, sizeof(txt), "%02d:%02d:%02d  ·  %.0f 秒", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, c.duration_s);
    lv_obj_t *l = make_label(btn, txt);
    lv_obj_center(l);
  }
}

// 片段加载在工作线程完成后回填
static std::vector<ClipInfo> pending_clips;
static volatile bool clips_ready = false;

static void clips_loader(void *param) {
  String cat = cur_cat_id;
  String date;
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  date = g_state.today_date;
  xSemaphoreGive(state_mutex);
  std::vector<ClipInfo> v;
  rest_fetch_clips(cat, date, v);
  pending_clips = v;
  clips_ready = true;
  vTaskDelete(NULL);
}

// ---------- 播放页 ----------
static void on_pause_clicked(lv_event_t *) {
  player_paused = !player_paused;
  audio_stream_set_paused(player_paused);
  lv_label_set_text(player_lbl_pause, player_paused ? LV_SYMBOL_PLAY " 播放" : LV_SYMBOL_PAUSE " 暂停");
}

static void on_player_back(lv_event_t *) {
  player_stop();
  ui_show_home();
}

static void build_player() {
  scr_player = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_player, lv_color_black(), 0);
  lv_obj_clear_flag(scr_player, LV_OBJ_FLAG_SCROLLABLE);

  player_canvas = lv_canvas_create(scr_player);
  lv_obj_align(player_canvas, LV_ALIGN_TOP_MID, 0, 10);

  lv_obj_t *row = lv_obj_create(scr_player);
  lv_obj_set_size(row, 440, 90);
  lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -16);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *back = lv_btn_create(row);
  lv_obj_set_size(back, 110, 60);
  lv_obj_align(back, LV_ALIGN_LEFT_MID, 10, 0);
  lv_obj_add_event_cb(back, on_player_back, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bl = make_label(back, LV_SYMBOL_LEFT " 返回");
  lv_obj_center(bl);

  player_btn_pause = lv_btn_create(row);
  lv_obj_set_size(player_btn_pause, 130, 60);
  lv_obj_align(player_btn_pause, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_event_cb(player_btn_pause, on_pause_clicked, LV_EVENT_CLICKED, NULL);
  player_lbl_pause = make_label(player_btn_pause, LV_SYMBOL_PAUSE " 暂停");
  lv_obj_center(player_lbl_pause);

  player_lbl_status = make_label(row, "缓冲中…");
  lv_obj_set_style_text_color(player_lbl_status, lv_color_hex(0x999999), 0);
  lv_obj_align(player_lbl_status, LV_ALIGN_RIGHT_MID, -10, 0);
}

// ---------- 直播页 ----------
static void live_gesture(lv_event_t *e) {
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
  if (dir == LV_DIR_LEFT) {
    live_stop();
    ui_show_home();
  }
}

static void build_live() {
  scr_live = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_live, lv_color_black(), 0);
  lv_obj_clear_flag(scr_live, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(scr_live, live_gesture, LV_EVENT_GESTURE, NULL);

  live_canvas = lv_canvas_create(scr_live);
  lv_obj_align(live_canvas, LV_ALIGN_CENTER, 0, 0);

  live_lbl = make_label(scr_live, LV_SYMBOL_LEFT " 返回    LIVE");
  lv_obj_set_style_text_color(live_lbl, lv_color_hex(0xd94f4f), 0);
  lv_obj_align(live_lbl, LV_ALIGN_TOP_MID, 0, 10);
  lv_obj_set_style_bg_color(live_lbl, lv_color_hex(0x101418), 0);
  lv_obj_set_style_bg_opa(live_lbl, LV_OPA_70, 0);
  lv_obj_set_style_pad_all(live_lbl, 6, 0);
}

// ---------- 离线页 ----------
static void on_retry(lv_event_t *) { esp_restart(); }

static void build_offline() {
  scr_offline = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_offline, lv_color_hex(0x101418), 0);
  lv_obj_t *t = make_label(scr_offline, LV_SYMBOL_WIFI " 离线", &st_title);
  lv_obj_align(t, LV_ALIGN_CENTER, 0, -60);
  offline_lbl_reason = make_label(scr_offline, "");
  lv_obj_set_style_text_color(offline_lbl_reason, lv_color_hex(0x999999), 0);
  lv_obj_align(offline_lbl_reason, LV_ALIGN_CENTER, 0, -16);
  lv_obj_t *btn = lv_btn_create(scr_offline);
  lv_obj_set_size(btn, 140, 56);
  lv_obj_align(btn, LV_ALIGN_CENTER, 0, 60);
  lv_obj_add_event_cb(btn, on_retry, LV_EVENT_CLICKED, NULL);
  lv_obj_t *l = make_label(btn, "重试");
  lv_obj_center(l);
}

// ---------- 秒级状态刷新 ----------
static void sec_tick(lv_timer_t *) {
  time_t now = time(NULL);
  struct tm tmv;
  localtime_r(&now, &tmv);
  char clk[16];
  if (now > 1000000000) snprintf(clk, sizeof(clk), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
  else snprintf(clk, sizeof(clk), "--:--");
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  bool wifi = g_state.wifi_ok, srv = g_state.server_ok, ws = g_state.ws_ok;
  xSemaphoreGive(state_mutex);
  lv_label_set_text(lbl_clock, clk);
  lv_label_set_text(lbl_status, ws ? LV_SYMBOL_WIFI : (wifi ? LV_SYMBOL_WIFI "·" : LV_SYMBOL_CLOSE));
  lv_obj_set_style_text_color(lbl_status, srv ? lv_color_hex(0x3fb27f) : lv_color_hex(0xd94f4f), 0);
  (void)srv;
}

// ---------- 导航 ----------
void ui_show_home() {
  lv_lock();
  refresh_home_grid();
  lv_scr_load_anim(scr_home, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
  lv_unlock();
}

void ui_show_clips(const String &cat_id) {
  cur_cat_id = cat_id;
  CatInfo *c = find_cat(cat_id);
  char title[64];
  snprintf(title, sizeof(title), "%s 今日片段", c ? c->name.c_str() : cat_id.c_str());
  lv_lock();
  lv_label_set_text(clips_title, title);
  lv_obj_clean(clips_list);
  make_label(clips_list, "加载中…");
  clips_ready = false;
  lv_scr_load_anim(scr_clips, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
  lv_unlock();
  xTaskCreatePinnedToCore(clips_loader, "clips", 8192, NULL, 4, NULL, 0);
}

void ui_show_player(long event_id) {
  player_paused = false;
  lv_lock();
  lv_label_set_text(player_lbl_pause, LV_SYMBOL_PAUSE " 暂停");
  lv_label_set_text(player_lbl_status, "连接中…");
  lv_canvas_set_buffer(player_canvas, player_bufs[player_show_idx], PLAYER_W, PLAYER_H, LV_IMG_CF_TRUE_COLOR);
  lv_canvas_fill_bg(player_canvas, lv_color_black(), LV_OPA_COVER);
  lv_scr_load_anim(scr_player, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
  lv_unlock();
  player_start(event_id);
}

void ui_show_live() {
  lv_lock();
  lv_canvas_set_buffer(live_canvas, live_bufs[live_show_idx], LIVE_W, LIVE_H, LV_IMG_CF_TRUE_COLOR);
  lv_canvas_fill_bg(live_canvas, lv_color_black(), LV_OPA_COVER);
  lv_scr_load_anim(scr_live, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
  lv_unlock();
  live_start();
}

static void show_offline(const char *reason) {
  lv_label_set_text(offline_lbl_reason, reason && reason[0] ? reason : "无法连接网络或服务器");
  lv_scr_load(scr_offline);
}

// ---------- 初始化 ----------
void ui_init() {
  cmd_q = xQueueCreate(24, sizeof(Cmd));

  player_bufs[0] = (uint16_t *)heap_caps_malloc(PLAYER_W * PLAYER_H * 2, MALLOC_CAP_SPIRAM);
  player_bufs[1] = (uint16_t *)heap_caps_malloc(PLAYER_W * PLAYER_H * 2, MALLOC_CAP_SPIRAM);
  live_bufs[0] = (uint16_t *)heap_caps_malloc(LIVE_W * LIVE_H * 2, MALLOC_CAP_SPIRAM);
  live_bufs[1] = (uint16_t *)heap_caps_malloc(LIVE_W * LIVE_H * 2, MALLOC_CAP_SPIRAM);

  lv_lock();
  // 尝试加载中文字体（需先 uploadfs，见 tools/gen_font.sh）
  if (LittleFS.exists("/font/cjk20.bin")) {
    font_cjk = lv_font_load("L:/littlefs/font/cjk20.bin");
  }
  style_init();
  build_home();
  build_clips();
  build_player();
  build_live();
  build_offline();
  refresh_home_grid();
  lv_scr_load(scr_home);
  sec_timer = lv_timer_create(sec_tick, 1000, NULL);
  lv_unlock();
}

// ---------- 主循环处理 ----------
void ui_process() {
  // 帧提交
  if (player_dirty && lv_scr_act() == scr_player) {
    player_dirty = false;
    lv_lock();
    player_show_idx = 1 - player_show_idx;
    lv_canvas_set_buffer(player_canvas, player_bufs[player_show_idx], PLAYER_W, PLAYER_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_invalidate(player_canvas);
    lv_unlock();
  } else {
    player_dirty = false;
  }
  if (live_dirty && lv_scr_act() == scr_live) {
    live_dirty = false;
    lv_lock();
    live_show_idx = 1 - live_show_idx;
    lv_canvas_set_buffer(live_canvas, live_bufs[live_show_idx], LIVE_W, LIVE_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_invalidate(live_canvas);
    lv_unlock();
  } else {
    live_dirty = false;
  }

  if (clips_ready && lv_scr_act() == scr_clips) {
    clips_ready = false;
    lv_lock();
    fill_clips_list(pending_clips);
    lv_unlock();
  }

  Cmd c;
  while (xQueueReceive(cmd_q, &c, 0) == pdTRUE) {
    switch (c.type) {
      case CMD_REFRESH_HOME:
        lv_lock();
        if (lv_scr_act() == scr_home) refresh_home_grid();
        lv_unlock();
        break;
      case CMD_DRINKING_STARTED:
        lv_lock();
        banner_set_pending();
        lv_unlock();
        audio_play_alert();
        break;
      case CMD_CAT_CONFIRMED:
        state_bump_count(c.cat_id);
        lv_lock();
        banner_set_confirmed(c.cat_id);
        if (lv_scr_act() == scr_home) refresh_home_grid();
        lv_unlock();
        break;
      case CMD_CAT_UNKNOWN:
        lv_lock();
        banner_set_unknown();
        lv_unlock();
        break;
      case CMD_EVENT_DISCARDED:
        lv_lock();
        banner_hide();
        lv_unlock();
        break;
      case CMD_OFFLINE:
        lv_lock();
        if (c.flag) show_offline(c.text);
        else lv_scr_load(scr_home);
        g_state.offline_shown = c.flag;
        lv_unlock();
        break;
      case CMD_PLAYER_END:
        lv_lock();
        lv_label_set_text(player_lbl_status, "播放结束");
        lv_unlock();
        break;
      case CMD_PLAYER_ERROR:
        lv_lock();
        lv_label_set_text(player_lbl_status, "连接失败");
        lv_unlock();
        break;
    }
  }
}
