#include "bsp_display.h"
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

void lv_lock() { xSemaphoreTake(lv_mutex, portMAX_DELAY); }
void lv_unlock() { xSemaphoreGive(lv_mutex); }

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

static void lvgl_flush_cb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
  lv_disp_flush_ready(disp);
}

static void lvgl_tick_cb(void *arg) { lv_tick_inc(LVGL_TICK_PERIOD_MS); }

static void touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
  if (!take_touch_interrupt()) {
    return;
  }
  uint8_t touched = touch.getPoint(touch_x, touch_y, touch.getSupportTouchPoint());
  if (touched) {
    data->state = LV_INDEV_STATE_PR;
    data->point.x = touch_x[0];
    data->point.y = touch_y[0];
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

bool bsp_display_init() {
  gfx_bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
  gfx = new Arduino_CO5300(gfx_bus, LCD_RESET, 0 /* rotation */, LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);
  if (!gfx->begin()) {
    Serial.println("[BSP] gfx begin failed");
    return false;
  }
  gfx->setBrightness(200);
  gfx->fillScreen(0x0000);
  return true;
}

void bsp_touch_init() {
  touch.setPins(TP_RST, TP_INT);
  if (!touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
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

  // 内部 DMA RAM 双缓冲（SPI DMA 不支持 PSRAM），480*48 行/块
  const uint32_t buf_pixels = LCD_WIDTH * LCD_HEIGHT / 10;
  lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_DMA);
  lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_DMA);
  if (!buf1 || !buf2) {
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
