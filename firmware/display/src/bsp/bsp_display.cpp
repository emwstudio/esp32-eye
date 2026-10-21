#include "bsp_display.h"
#include "net/rlog.h"
#include "board_pins.h"
#include "esp_timer.h"

Arduino_DataBus *gfx_bus = nullptr;
Arduino_CO5300 *gfx = nullptr;

static TouchDrvCST92xx touch;
static int16_t touch_x[2], touch_y[2];
static volatile bool touch_pending = false;

static SemaphoreHandle_t lv_mutex = NULL;
static esp_timer_handle_t lvgl_tick_timer = NULL;

#define LVGL_TICK_PERIOD_MS 2

// 注意：lv_mutex 是递归互斥锁，必须用 Take/GiveRecursive——
// 否则 LVGL 事件处理器（已在锁内）里再 lv_lock 会永久自锁，UI 假死
void lv_lock() { xSemaphoreTakeRecursive(lv_mutex, portMAX_DELAY); }
void lv_unlock() { xSemaphoreGiveRecursive(lv_mutex); }

static void IRAM_ATTR on_touch_interrupt() { touch_pending = true; }

static bool take_touch_interrupt() {
  noInterrupts();
  bool pending = touch_pending;
  touch_pending = false;
  interrupts();
  return pending;
}

// 官方示例: 刷新区域对齐偶数坐标（CO5300 QSPI 要求）
static void lvgl_rounder_cb(lv_disp_drv_t *disp_drv, lv_area_t *area) {
  if (area->x1 % 2 != 0) area->x1--;
  if (area->y1 % 2 != 0) area->y1--;
  if (area->x2 % 2 == 0) area->x2++;
  if (area->y2 % 2 == 0) area->y2++;
}

static uint16_t *rot_buf = nullptr;  // 旋转转置缓冲（内部 DMA RAM，PSRAM 过不了 SPI DMA）

// flush 耗时统计（帧率优化打点）
static volatile uint32_t flush_total_us = 0, flush_count = 0;
void bsp_flush_stats(uint32_t *total_us, uint32_t *count) {
  *total_us = flush_total_us;
  *count = flush_count;
  flush_total_us = 0;
  flush_count = 0;
}

// 直播直推与 LVGL flush 共享 rot_buf（双方都在 lv_lock 内使用，互斥）
uint16_t *bsp_video_stage_buf() {
  if (!rot_buf) rot_buf = (uint16_t *)heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT / 20 * 2, MALLOC_CAP_DMA);
  return rot_buf;
}

static void lvgl_flush_cb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  int64_t t0 = esp_timer_get_time();
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  // 面板原生方向与桌面摆放差 90°，CO5300 又不支持 90° 硬件旋转：
  // 手工把每个块转置——逻辑像素 (x,y) → 面板 (x'=y, y'=479-x)，块 (w×h) → (h×w)
  if (!rot_buf) rot_buf = (uint16_t *)heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT / 20 * 2, MALLOC_CAP_DMA);
  const uint16_t *src = (const uint16_t *)color_p;
  uint32_t w1 = w - 1;
  for (uint32_t i = 0; i < w; i++) {          // 目标行 = area->x2 - x_l
    uint32_t src_col = w1 - i;                // 源列（从右往左）
    for (uint32_t j = 0; j < h; j++) {        // 目标列 = y_l - area->y1
      rot_buf[i * h + j] = src[j * w + src_col];
    }
  }
  gfx->draw16bitRGBBitmap(area->y1, LCD_HEIGHT - 1 - area->x2, rot_buf, h, w);
  lv_disp_flush_ready(disp);
  flush_total_us += (uint32_t)(esp_timer_get_time() - t0);
  flush_count++;
}

static void lvgl_tick_cb(void *arg) { lv_tick_inc(LVGL_TICK_PERIOD_MS); }

static void touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
  // CST9220 最终版读法（实测校准）：
  // - 按下/抬起用 INT 引脚电平（isPressed）——抬起检测可靠
  // - 坐标在芯片有新报告时（getPoint 返回 n>0）才更新，无新数据时保持——
  //   芯片缓冲间歇性就绪，把"无新数据"当作"抬起"会造成状态抖动
  // 注：单页仪表盘无触摸交互，本函数仅保留诊断打点
  static int lx = -1, ly = -1;
  bool pressed = touch.isPressed();
  uint8_t touched = touch.getPoint(touch_x, touch_y, touch.getSupportTouchPoint());
  if (touched) {
    // 实测标定：驱动坐标 → 屏幕逻辑坐标只需 X 镜像（物理左=tx 大值）
    lx = LCD_WIDTH - 1 - touch_x[0];
    ly = touch_y[0];
  }
  data->state = pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
  if (lx >= 0) {
    data->point.x = lx;
    data->point.y = ly;
  }
  // 诊断：按下/抬手/拖动（变化 >20px）记录
  static lv_indev_state_t last = LV_INDEV_STATE_REL;
  static int last_x = -1, last_y = -1;
  bool trans = data->state != last;
  bool moved = data->state == LV_INDEV_STATE_PR &&
               (abs((int)data->point.x - last_x) > 20 || abs((int)data->point.y - last_y) > 20);
  if (trans || moved) {
    rlogf("[TOUCH] %s log=%d,%d raw=%d,%d",
          data->state == LV_INDEV_STATE_PR ? (trans ? "DOWN" : "MOVE") : "UP",
          (int)data->point.x, (int)data->point.y,
          touched ? (int)touch_y[0] : -1, touched ? (int)touch_x[0] : -1);
    last = data->state;
    last_x = data->point.x;
    last_y = data->point.y;
  }
}

bool bsp_display_init() {
  gfx_bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
  // CO5300 不支持 90° 硬件旋转（仅翻转），90° 旋转在 flush 回调里手工转置
  gfx = new Arduino_CO5300(gfx_bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);
  if (!gfx->begin(80000000)) {  // QSPI 提速 40→80MHz（默认 40MHz 是 flush/DMA 瓶颈之一）
    Serial.println("[BSP] gfx begin failed");
    return false;
  }
  gfx->setBrightness(200);
  gfx->fillScreen(0x0000);
  return true;
}

void bsp_touch_init() {
  touch.setPins(TP_RST, TP_INT);
  // CST9220 上电 boot 需要时间，失败时重试（官方示例前置 delay(1000)，此处再加保险）
  bool online = false;
  for (int attempt = 0; attempt < 3 && !online; attempt++) {
    online = touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
    if (!online) {
      Serial.printf("[BSP] touch begin attempt %d failed\n", attempt + 1);
      delay(500);
    }
  }
  if (!online) {
    Serial.println("[BSP] touch not online, UI 将无触摸输入");
    return;
  }
  Serial.printf("[BSP] touch model: %s\n", touch.getModelName());
  touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
  touch.setSwapXY(true);
  touch.setMirrorXY(true, false);
  pinMode(TP_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TP_INT), on_touch_interrupt, FALLING);
}

static void lvgl_port_init() {
  lv_init();

  // 内部 DMA RAM 双缓冲（SPI DMA 不支持 PSRAM），480*24 行/块
  // 注：1/10 屏双缓冲要 92KB 内部 RAM，会挤爆任务栈分配；1/20 双缓冲 46KB 是内存与性能的折中
  const uint32_t buf_pixels = LCD_WIDTH * LCD_HEIGHT / 20;
  lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_DMA);
  lv_color_t *buf2 = NULL;  // 单缓冲：双缓冲多占 23KB 内部 RAM，挤爆了网络任务栈
  if (!buf1) {
    Serial.println("[BSP] LVGL draw buffer alloc failed");
  }

  static lv_disp_draw_buf_t draw_buf;
  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_pixels);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = LCD_WIDTH;
  disp_drv.ver_res = LCD_HEIGHT;
  disp_drv.flush_cb = lvgl_flush_cb;
  disp_drv.rounder_cb = lvgl_rounder_cb;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touchpad_read;
  indev_drv.gesture_limit = 60;        // 位移 >60px 才算手势（默认 50）
  indev_drv.gesture_min_velocity = 0;  // 不收速度门票（默认 3px/周期，慢滑会被拒）
  lv_indev_drv_register(&indev_drv);

  const esp_timer_create_args_t tick_args = {.callback = &lvgl_tick_cb, .name = "lvgl_tick"};
  esp_timer_create(&tick_args, &lvgl_tick_timer);
  esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000);
}

void bsp_lvgl_task() {
  lv_lock();
  lv_timer_handler();
  lv_unlock();
}

void bsp_lvgl_init() {
  lv_mutex = xSemaphoreCreateRecursiveMutex();
  lvgl_port_init();
}
