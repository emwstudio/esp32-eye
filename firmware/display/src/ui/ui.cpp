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
LV_FONT_DECLARE(font_cjk24);  // 24px 中文字体（猫名，全字符集防改名豆腐块，src/font_cjk24.c）
LV_FONT_DECLARE(font_cjk16);  // 16px 辅助文字字体（卡片单位"/ 8次"，src/font_cjk16.c）
LV_FONT_DECLARE(font_num36);  // 36px 数字字体（猫卡次数大数字用，src/font_num36.c）
static const lv_font_t *font_cjk = &font_cjk20;

static lv_obj_t *lbl_clock, *lbl_status;
static lv_obj_t *offline_mask, *offline_lbl;
static lv_obj_t *banner, *banner_lbl;
static lv_obj_t *cat_cards[4], *cat_imgs[4], *cat_names[4], *cat_counts[4], *cat_units[4];
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
// 每猫主题色（按 cat_id 映射，未知猫用默认绿）
static lv_color_t theme_color(const String &id) {
  if (id == "baobao") return lv_color_hex(0x6cb5f5);     // 蓝
  if (id == "bobo") return lv_color_hex(0xf4a7b9);       // 粉
  if (id == "qiuqiu") return lv_color_hex(0xffd54f);     // 黄
  if (id == "shandian") return lv_color_hex(0xb39ddb);   // 紫
  return lv_color_hex(0x3fb27f);
}

static void card_pulse_cb(void *obj, int32_t v) {
  // v: 0..100 → 边框宽度 2..6 呼吸
  lv_obj_set_style_border_width((lv_obj_t *)obj, 2 + (v * 4) / 100, 0);
  lv_obj_set_style_border_opa((lv_obj_t *)obj, LV_OPA_40 + (v * (LV_OPA_COVER - LV_OPA_40)) / 100, 0);
}

static void card_pulse(lv_obj_t *card, lv_color_t color) {
  lv_obj_set_style_border_color(card, color, 0);
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

static void card_border_default(lv_obj_t *card) {
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(0x262d36), 0);
}

// 确认喝水的高亮：脉冲 3 下后残留淡主题色描边（10s 后恢复默认，避免残留成"永久边框"）
static lv_timer_t *hl_timers[4] = {NULL, NULL, NULL, NULL};

static void card_highlight(int idx, lv_color_t color) {
  if (idx < 0 || idx > 3) return;
  lv_anim_del(cat_cards[idx], card_pulse_cb);
  if (hl_timers[idx]) { lv_timer_del(hl_timers[idx]); hl_timers[idx] = NULL; }
  card_pulse(cat_cards[idx], color);
  hl_timers[idx] = lv_timer_create([](lv_timer_t *t) {
    for (int i = 0; i < 4; i++) {
      if (hl_timers[i] == t) {
        hl_timers[i] = NULL;
        lv_anim_del(cat_cards[i], card_pulse_cb);
        card_border_default(cat_cards[i]);
        break;
      }
    }
    lv_timer_del(t);
  }, 10000, NULL);
  lv_timer_set_repeat_count(hl_timers[idx], 1);
}

static void count_bounce_cb(void *obj, int32_t v) {
  lv_obj_set_style_transform_zoom((lv_obj_t *)obj, v, 0);
}

static void count_bounce(lv_obj_t *lbl) {
  // 大号数字常态 zoom=256（1x），弹跳冲到 320（1.25x）再回弹
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, lbl);
  lv_anim_set_exec_cb(&a, count_bounce_cb);
  lv_anim_set_values(&a, 256, 320);
  lv_anim_set_time(&a, 180);
  lv_anim_set_playback_time(&a, 220);
  lv_anim_set_repeat_count(&a, 1);
  lv_anim_start(&a);
}

// 进度条动画（计数变化时过渡）
static lv_obj_t *cat_bars[4];
static int bar_val[4] = {0, 0, 0, 0};

static void bar_anim_cb(void *obj, int32_t v) {
  for (int i = 0; i < 4; i++) {
    if (cat_bars[i] == (lv_obj_t *)obj) {
      bar_val[i] = v;
      break;
    }
  }
  lv_bar_set_value((lv_obj_t *)obj, v, LV_ANIM_OFF);
}

static void bar_set_animated(int i, int value) {
  lv_anim_del(cat_bars[i], bar_anim_cb);  // 重启同对象动画，避免叠加
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, cat_bars[i]);
  lv_anim_set_exec_cb(&a, bar_anim_cb);
  lv_anim_set_values(&a, bar_val[i], value);
  lv_anim_set_time(&a, 350);
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
    card_highlight(idx, theme_color(cat_id));
    count_bounce(cat_counts[idx]);
  }
}

// ---------- 猫卡片 ----------
#define DAILY_GOAL 8  // 每日目标次数（进度条满格）

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
      lv_color_t theme = theme_color(ids[i]);
      lv_label_set_text(cat_names[i], names[i].c_str());
      lv_obj_set_style_text_color(cat_counts[i], theme, 0);  // 大数字跟猫的主题色走（呼应头像环）
      char b[8];
      snprintf(b, sizeof(b), "%d", counts[i]);
      lv_label_set_text(cat_counts[i], b);
      // 头像圆环主题色（卡片边框维持极淡底色，脉冲时才上主题色）
      lv_obj_set_style_border_color(lv_obj_get_parent(cat_imgs[i]), theme, 0);
      // 进度条：主题色填充，达标（满格）换金色
      int v = counts[i] > DAILY_GOAL ? DAILY_GOAL : counts[i];
      lv_obj_set_style_bg_color(cat_bars[i], counts[i] >= DAILY_GOAL ? lv_color_hex(0xffd700) : theme, LV_PART_INDICATOR);
      if (v != bar_val[i]) bar_set_animated(i, v);
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
//   y=0..34    标题栏（三元素同基线垂直居中，内收避圆角）
//   y=34..234  视频带 480×200（/api/live_band?h=200，无 LVGL 对象，直推独占）
//   y=234..480 2×2 猫卡片（每张 240×112）
#define TITLE_H 34
#define VIDEO_Y 34
static void build_main() {
  scr_main = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_main, lv_color_hex(0x101418), 0);
  lv_obj_clear_flag(scr_main, LV_OBJ_FLAG_SCROLLABLE);

  // ---- 标题栏（容器裁剪：子元素越界也不会画进视频带）----
  lv_obj_t *titlebar = lv_obj_create(scr_main);
  lv_obj_set_size(titlebar, 480, TITLE_H);
  lv_obj_set_pos(titlebar, 0, 0);
  lv_obj_set_style_bg_opa(titlebar, LV_OPA_TRANSP, 0);  // 透明底，屏幕底色即标题栏底色
  lv_obj_set_style_border_width(titlebar, 0, 0);
  lv_obj_set_style_pad_all(titlebar, 0, 0);
  lv_obj_clear_flag(titlebar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  // 左侧时间（往右收 38px，避开圆角）
  lbl_clock = make_label(titlebar, "--:--");
  lv_obj_align(lbl_clock, LV_ALIGN_LEFT_MID, 38, 0);
  lv_obj_set_style_text_color(lbl_clock, lv_color_hex(0x9fb3c8), 0);

  // 中间大标题
  lv_obj_t *lbl_title = make_label(titlebar, "猫咪喝水计数器");
  lv_obj_align(lbl_title, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_text_color(lbl_title, lv_color_hex(0x9fd3b8), 0);

  // 右侧连接状态点（往左收 40px，避开圆角）
  lbl_status = make_label(titlebar, "●");
  lv_obj_align(lbl_status, LV_ALIGN_RIGHT_MID, -40, 0);
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

  // 猫卡片区：2x2（每格 240x112，卡片 236x108）
  for (int i = 0; i < 4; i++) {
    lv_obj_t *card = lv_obj_create(scr_main);
    cat_cards[i] = card;
    lv_obj_set_size(card, 236, 108);
    lv_obj_set_pos(card, (i % 2) * 240 + 2, 234 + (i / 2) * 112 + 2);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1a1f26), 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x262d36), 0);  // 极淡边框
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // 左：88px 圆形头像（圆环主题色在 refresh_cards 按猫设置）
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

    // 右上：名字（24px 白）
    cat_names[i] = make_label(card, "");
    lv_obj_set_style_text_font(cat_names[i], &font_cjk24, 0);
    lv_obj_set_pos(cat_names[i], 108, 10);

    // 右下：大号次数数字（36px 数字字体，主题色在 refresh_cards 里跟猫走）
    cat_counts[i] = make_label(card, "0");
    lv_obj_set_style_text_font(cat_counts[i], &font_num36, 0);
    lv_obj_set_pos(cat_counts[i], 108, 48);
    // "/ 8次"（16px 灰）自动贴住数字右缘并底对齐（跟随数字宽度变化）
    char ub[16];
    snprintf(ub, sizeof(ub), "/ %d次", DAILY_GOAL);
    lv_obj_t *unit = make_label(card, ub);
    lv_obj_set_style_text_font(unit, &font_cjk16, 0);
    lv_obj_set_style_text_color(unit, lv_color_hex(0x8a93a0), 0);
    lv_obj_align_to(unit, cat_counts[i], LV_ALIGN_OUT_RIGHT_BOTTOM, 4, 0);
    cat_units[i] = unit;

    // 底部：细进度条（今日次数 / DAILY_GOAL，圆角轨道，主题色填充，满格金色）
    lv_obj_t *bar = lv_bar_create(card);
    cat_bars[i] = bar;
    lv_obj_set_size(bar, 120, 8);
    lv_obj_set_pos(bar, 108, 90);
    lv_bar_set_range(bar, 0, DAILY_GOAL);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, 4, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x262d36), 0);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x3fb27f), LV_PART_INDICATOR);
  }

  home_empty = make_label(scr_main, "等待猫咪档案…");
  lv_obj_align(home_empty, LV_ALIGN_BOTTOM_MID, 0, -100);
  lv_obj_add_flag(home_empty, LV_OBJ_FLAG_HIDDEN);
}

// ---------- 秒级状态刷新 ----------
static void sec_tick(lv_timer_t *) {
  static bool coords_dumped = false;
  if (!coords_dumped) {  // 一次性诊断：标题栏元素实际坐标（核对是否压到视频带 y>=34）
    coords_dumped = true;
    lv_area_t a;
    lv_obj_update_layout(scr_main);
    lv_obj_get_coords(lbl_clock, &a);
    rlogf("[UI] clock coords x=%d..%d y=%d..%d", a.x1, a.x2, a.y1, a.y2);
    Serial.printf("[UI] clock coords x=%d..%d y=%d..%d\n", a.x1, a.x2, a.y1, a.y2);
    lv_obj_get_coords(lbl_status, &a);
    rlogf("[UI] status coords x=%d..%d y=%d..%d", a.x1, a.x2, a.y1, a.y2);
    Serial.printf("[UI] status coords x=%d..%d y=%d..%d\n", a.x1, a.x2, a.y1, a.y2);
    if (cat_counts[0]) {
      lv_obj_get_coords(cat_counts[0], &a);
      rlogf("[UI] count0 coords x=%d..%d y=%d..%d", a.x1, a.x2, a.y1, a.y2);
      Serial.printf("[UI] count0 coords x=%d..%d y=%d..%d\n", a.x1, a.x2, a.y1, a.y2);
    }
  }
  time_t now = time(NULL);
  struct tm tmv;
  localtime_r(&now, &tmv);
  char clk[16];
  if (now > 1000000000) snprintf(clk, sizeof(clk), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
  else snprintf(clk, sizeof(clk), "--:--");
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  bool srv = g_state.server_ok, ws = g_state.ws_ok;
  xSemaphoreGive(state_mutex);
  // 只在内容变化时重绘（配合 flush 裁剪，避免无谓刷新区撞视频带）
  static char last_clk[16] = {0};
  if (strcmp(clk, last_clk) != 0) {
    strncpy(last_clk, clk, sizeof(last_clk) - 1);
    lv_label_set_text(lbl_clock, clk);
  }
  static bool last_ok = true;  // 初值 true ≠ 初始红，强制首刷
  bool ok = srv && ws;
  if (ok != last_ok) {
    last_ok = ok;
    lv_obj_set_style_text_color(lbl_status, ok ? lv_color_hex(0x3fb27f) : lv_color_hex(0xd94f4f), 0);
  }
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
