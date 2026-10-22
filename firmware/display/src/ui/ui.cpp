#include "ui.h"
#include "state.h"
#include "bsp/bsp_display.h"
#include "bsp/bsp_audio.h"
#include "net/net_rest.h"
#include "net/rlog.h"
#include <time.h>

// 单页仪表盘：上 480x240 直播（常开），下 480x240 二乘二猫卡片。
// 无任何导航/点按/手势；事件反馈全部来自 /ws/events。

// ---------- 命令队列 ----------
enum CmdType {
  CMD_REFRESH_HOME,
  CMD_DRINKING_STARTED,
  CMD_CAT_CONFIRMED,
  CMD_CAT_UNKNOWN,
  CMD_EVENT_DISCARDED,
  CMD_OFFLINE,
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

// ---------- 直播覆盖层 ----------
// 直播画面由 live 任务直接 gfx 直推（绕过 LVGL canvas，帧率优化）；
// 覆盖层标签重绘函数在对象声明之后定义（见下）。

// ---------- 页面对象 ----------
static lv_obj_t *scr_main;
static lv_style_t st_text;
LV_FONT_DECLARE(font_cjk20);  // 编译进固件的中文字体（tools/gen_font.sh 产物 src/font_cjk20.c）
static const lv_font_t *font_cjk = &font_cjk20;

static lv_obj_t *lbl_clock, *lbl_status;
static lv_obj_t *offline_mask, *offline_lbl;
static lv_obj_t *banner, *banner_lbl;
static lv_obj_t *cat_cards[4], *cat_imgs[4], *cat_names[4], *cat_counts[4];
static lv_obj_t *home_empty;

// 标题栏独立后视频带内无任何 LVGL 对象，直推整块写屏（无需挖洞/逐帧 invalidate）

static const lv_font_t *ui_font() { return font_cjk ? font_cjk : &lv_font_montserrat_20; }

static lv_obj_t *make_label(lv_obj_t *parent, const char *txt) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_add_style(l, &st_text, 0);
  lv_label_set_text(l, txt);
  return l;
}

// ---------- 横幅（视频区顶部细条）----------
static void banner_anim_y(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, v); }

static void banner_show(const char *text) {
  lv_label_set_text(banner_lbl, text);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, banner);
  lv_anim_set_exec_cb(&a, banner_anim_y);
  lv_anim_set_values(&a, 480, 260);  // 从屏外下方滑到视频带下缘（浮在猫卡区顶部）
  lv_anim_set_time(&a, 250);
  lv_anim_start(&a);
}

static void banner_hide() {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, banner);
  lv_anim_set_exec_cb(&a, banner_anim_y);
  lv_anim_set_values(&a, lv_obj_get_y(banner), 480);
  lv_anim_set_time(&a, 220);
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
  rlogf("[UI] banner pending");
  lv_obj_set_style_border_color(banner, lv_color_hex(0x3fb27f), 0);
  banner_show("有猫在喝水…");
}

static void banner_set_unknown() {
  rlogf("[UI] banner unknown");
  lv_obj_set_style_border_color(banner, lv_color_hex(0xd9a54f), 0);
  banner_show("有猫在喝水（没认出是哪只）");
  banner_auto_hide(5000);
}

// ---------- 猫卡片动画 ----------
static void card_pulse_cb(void *obj, int32_t v) {
  // v: 0..100 → 边框宽度 2..6 呼吸
  lv_obj_set_style_border_width((lv_obj_t *)obj, 2 + (v * 4) / 100, 0);
  lv_obj_set_style_border_opa((lv_obj_t *)obj, LV_OPA_40 + (v * (LV_OPA_COVER - LV_OPA_40)) / 100, 0);
}

static void card_pulse(lv_obj_t *card) {
  lv_obj_set_style_border_color(card, lv_color_hex(0x3fb27f), 0);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, card);
  lv_anim_set_exec_cb(&a, card_pulse_cb);
  lv_anim_set_values(&a, 0, 100);
  lv_anim_set_time(&a, 350);
  lv_anim_set_playback_time(&a, 350);
  lv_anim_set_repeat_count(&a, 3);
  lv_anim_start(&a);
}

static void count_bounce_cb(void *obj, int32_t v) {
  lv_obj_set_style_transform_zoom((lv_obj_t *)obj, v, 0);
}

static void count_bounce(lv_obj_t *lbl) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, lbl);
  lv_anim_set_exec_cb(&a, count_bounce_cb);
  lv_anim_set_values(&a, 256, 384);  // 1.0x → 1.5x → 回弹
  lv_anim_set_time(&a, 180);
  lv_anim_set_playback_time(&a, 220);
  lv_anim_set_repeat_count(&a, 1);
  lv_anim_start(&a);
}

static void banner_set_confirmed(const String &cat_id) {
  CatInfo *c = find_cat(cat_id);
  String name = c ? c->name : cat_id;
  char buf[64];
  snprintf(buf, sizeof(buf), "%s 在喝水", name.c_str());
  lv_obj_set_style_border_color(banner, lv_color_hex(0x3fb27f), 0);
  banner_show(buf);
  banner_auto_hide(8000);
  // 对应猫卡片绿色边框脉冲
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  int idx = -1;
  for (int i = 0; i < (int)g_state.cats.size() && i < 4; i++) {
    if (g_state.cats[i].id == cat_id) idx = i;
  }
  xSemaphoreGive(state_mutex);
  if (idx >= 0) {
    card_pulse(cat_cards[idx]);
    count_bounce(cat_counts[idx]);
  }
}

// ---------- 猫卡片 ----------
static void refresh_cards() {
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  int n = (int)g_state.cats.size();
  String ids[4], names[4];
  int counts[4];
  for (int i = 0; i < n && i < 4; i++) {
    ids[i] = g_state.cats[i].id;
    names[i] = g_state.cats[i].name;
    counts[i] = g_state.cats[i].today_count;
  }
  xSemaphoreGive(state_mutex);

  if (n == 0) {
    lv_obj_clear_flag(home_empty, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_add_flag(home_empty, LV_OBJ_FLAG_HIDDEN);

  for (int i = 0; i < 4; i++) {
    if (i < n) {
      lv_obj_clear_flag(cat_cards[i], LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(cat_names[i], names[i].c_str());
      char b[16];
      snprintf(b, sizeof(b), "%d 次", counts[i]);
      lv_label_set_text(cat_counts[i], b);
      void *dsc = avatar_img_dsc(ids[i]);
      if (dsc) {
        lv_img_set_src(cat_imgs[i], dsc);
        lv_obj_clear_flag(cat_imgs[i], LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(cat_imgs[i], LV_OBJ_FLAG_HIDDEN);
      }
    } else {
      lv_obj_add_flag(cat_cards[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
}

// ---------- 构建页面 ----------
// 布局（面板原生方向 480×480）：
//   y=0..56    标题栏（左时钟右状态点，各向内收 30px 避开圆角；中间大标题）
//   y=56..256  视频带 480×200（/api/live_band?h=200，无 LVGL 对象，直推独占）
//   y=256..480 2×2 猫卡片（每张 240×112）
#define TITLE_H 56
#define VIDEO_Y 56
static void build_main() {
  scr_main = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_main, lv_color_hex(0x101418), 0);
  lv_obj_clear_flag(scr_main, LV_OBJ_FLAG_SCROLLABLE);

  // ---- 标题栏 ----
  // 左侧时间（往右收 30px，避开圆角）
  lbl_clock = make_label(scr_main, "--:--");
  lv_obj_align(lbl_clock, LV_ALIGN_TOP_LEFT, 38, 16);
  lv_obj_set_style_bg_color(lbl_clock, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(lbl_clock, LV_OPA_50, 0);
  lv_obj_set_style_pad_all(lbl_clock, 4, 0);
  lv_obj_set_style_radius(lbl_clock, 6, 0);

  // 中间大标题
  lv_obj_t *lbl_title = make_label(scr_main, "猫猫喝水计数器");
  lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 14);
  lv_obj_set_style_text_color(lbl_title, lv_color_hex(0x9fd3b8), 0);

  // 右侧连接状态点（往左收 30px，避开圆角）
  lbl_status = make_label(scr_main, "●");
  lv_obj_align(lbl_status, LV_ALIGN_TOP_RIGHT, -40, 16);
  lv_obj_set_style_text_color(lbl_status, lv_color_hex(0xd94f4f), 0);

  // 离线遮罩（视频带）
  offline_mask = lv_obj_create(scr_main);
  lv_obj_set_size(offline_mask, LIVE_W, LIVE_H);
  lv_obj_set_pos(offline_mask, 0, VIDEO_Y);
  lv_obj_set_style_bg_color(offline_mask, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(offline_mask, LV_OPA_70, 0);
  lv_obj_set_style_border_width(offline_mask, 0, 0);
  lv_obj_clear_flag(offline_mask, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  offline_lbl = make_label(offline_mask, "离线 / 重连中…");
  lv_obj_center(offline_lbl);
  lv_obj_add_flag(offline_mask, LV_OBJ_FLAG_HIDDEN);

  // 喝水横幅（浮在猫卡区顶部一行，默认藏在屏外下方）
  banner = lv_obj_create(scr_main);
  lv_obj_set_size(banner, 440, 40);
  lv_obj_set_pos(banner, 20, 480);  // 初始藏在屏外下方
  lv_obj_set_style_radius(banner, 12, 0);
  lv_obj_set_style_bg_color(banner, lv_color_hex(0x22303c), 0);
  lv_obj_set_style_bg_opa(banner, LV_OPA_90, 0);
  lv_obj_set_style_border_color(banner, lv_color_hex(0x3fb27f), 0);
  lv_obj_set_style_border_width(banner, 2, 0);
  lv_obj_set_style_clip_corner(banner, true, 0);
  lv_obj_clear_flag(banner, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  banner_lbl = make_label(banner, "");
  lv_obj_center(banner_lbl);

  // 猫卡片区：2x2（每格 240x112）
  for (int i = 0; i < 4; i++) {
    lv_obj_t *card = lv_obj_create(scr_main);
    cat_cards[i] = card;
    lv_obj_set_size(card, 236, 108);
    lv_obj_set_pos(card, (i % 2) * 240 + 2, 256 + (i / 2) * 112 + 2);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x181d23), 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2a2f36), 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // 左：88px 圆形头像
    lv_obj_t *circle = lv_obj_create(card);
    lv_obj_set_size(circle, 88, 88);
    lv_obj_align(circle, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle, lv_color_hex(0x2a2f36), 0);
    lv_obj_set_style_border_width(circle, 2, 0);
    lv_obj_set_style_border_color(circle, lv_color_hex(0x3fb27f), 0);
    lv_obj_set_style_pad_all(circle, 0, 0);
    lv_obj_set_style_clip_corner(circle, true, 0);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    cat_imgs[i] = lv_img_create(circle);
    lv_img_set_zoom(cat_imgs[i], 117);  // 192 -> ~88
    lv_obj_center(cat_imgs[i]);
    lv_obj_add_flag(cat_imgs[i], LV_OBJ_FLAG_HIDDEN);

    // 右上：名字；右下：次数
    cat_names[i] = make_label(card, "");
    lv_obj_set_pos(cat_names[i], 104, 18);
    cat_counts[i] = make_label(card, "0 次");
    lv_obj_set_pos(cat_counts[i], 104, 54);
    lv_obj_set_style_text_color(cat_counts[i], lv_color_hex(0x9fd3b8), 0);
  }

  home_empty = make_label(scr_main, "等待猫咪档案…");
  lv_obj_align(home_empty, LV_ALIGN_BOTTOM_MID, 0, -100);
  lv_obj_add_flag(home_empty, LV_OBJ_FLAG_HIDDEN);
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
  bool srv = g_state.server_ok, ws = g_state.ws_ok;
  xSemaphoreGive(state_mutex);
  lv_label_set_text(lbl_clock, clk);
  lv_obj_set_style_text_color(lbl_status, (srv && ws) ? lv_color_hex(0x3fb27f) : lv_color_hex(0xd94f4f), 0);
}

// ---------- 初始化 ----------
void ui_init() {
  cmd_q = xQueueCreate(24, sizeof(Cmd));

  lv_lock();
  lv_style_init(&st_text);
  lv_style_set_text_font(&st_text, ui_font());
  lv_style_set_text_color(&st_text, lv_color_white());

  build_main();
  refresh_cards();
  lv_scr_load(scr_main);
  lv_timer_create(sec_tick, 1000, NULL);
  lv_unlock();
}

// ---------- 主循环处理 ----------
void ui_process() {
  Cmd c;
  while (xQueueReceive(cmd_q, &c, 0) == pdTRUE) {
    switch (c.type) {
      case CMD_REFRESH_HOME:
        lv_lock();
        refresh_cards();
        lv_unlock();
        break;
      case CMD_DRINKING_STARTED:
        lv_lock();
        banner_set_pending();
        lv_unlock();
        break;
      case CMD_CAT_CONFIRMED:
        state_bump_count(c.cat_id);
        lv_lock();
        banner_set_confirmed(c.cat_id);
        refresh_cards();
        lv_unlock();
        rlogf("[UI] cat_confirmed %s count+1", c.cat_id);
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
        if (c.flag) {
          if (c.text[0]) lv_label_set_text(offline_lbl, c.text);
          else lv_label_set_text(offline_lbl, "离线 / 重连中…");
          lv_obj_clear_flag(offline_mask, LV_OBJ_FLAG_HIDDEN);
        } else {
          lv_obj_add_flag(offline_mask, LV_OBJ_FLAG_HIDDEN);
        }
        g_state.offline_shown = c.flag;
        lv_unlock();
        break;
    }
  }
}
