# esp32-eye 桌面显示屏固件（Waveshare ESP32-S3-Touch-AMOLED-2.16）

猫喝水监控系统的桌面显示节点。**单页仪表盘，无触摸交互**：
上半屏 480×240 实时直播（`/api/live_band`，常开，自启动），左上角时钟、右上角连接状态点；
下半屏 2×2 猫卡片（圆形头像 + 名字 + 今日次数）。喝水事件通过 WS 驱动横幅与卡片动画反馈。

## 接口契约

严格遵守 `../docs/api-contract.md`（REST + WS 事件）。

## 快速开始

```bash
cp include/config.h.example include/config.h   # 填写 WiFi 与 SERVER_BASE
pio run                                        # 编译
pio run -t upload --upload-port /dev/cu.usbmodemXXXX   # 烧录固件
```

中文字体已编译进固件（`src/font_cjk20.c`）。新增 UI 文字引入新汉字/字符时：

```bash
# 把新字符追加到 tools/cjk_chars.txt，然后：
npx -y lv_font_conv@1.5.2 --font /tmp/NotoSansSC.ttf --size 20 --bpp 4 --format lvgl \
  --no-compress --symbols "$(cat tools/cjk_chars.txt | tr -d '\n')" -o src/font_cjk20.c
sed -i '' 's|#include "lvgl/lvgl.h"|#include "lvgl.h"|' src/font_cjk20.c
```

调试工具：`tools/serial_reset_read.py`（RTS 复位后从头抓启动日志）、`tools/read_serial.py`（被动抓串口）。
远程日志 rlog 会上报到服务端 `/api/debug/log`（服务端日志里 `[display]` 前缀）。

## BSP 来源与适配

以微雪官方仓库 [waveshareteam/ESP32-S3-Touch-AMOLED-2.16](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16)
的 Arduino 示例为底座：

- **显示**：`05_LVGL_Widgets` 示例的移植层 —— `Arduino_ESP32QSPI` + `Arduino_CO5300`、
  LVGL flush/rounder 回调。官方捆绑的 Arduino_GFX 1.6.4 与上游逐字节一致（仅换行符差异）。
  注意：CO5300 不支持 90° 硬件旋转，flush 回调里做手工 90° 转置（rot_buf，勿删）。
- **触摸**：`TouchDrvCST92xx`（CST9220）。当前 UI 无触摸交互，touchpad_read 仅保留诊断打点。
- **音频**：`07_ES8311` 示例的 ES8311 初始化序列原样保留（`src/es8311/`），I2S 用 legacy
  `driver/i2s.h` + PSRAM 环形缓冲。codec 初始化失败时音频整体降级（不崩溃，UI 正常）。
- **PMU**：AXP2101 初始化序列来自官方 `03_LVGL_AXP2101_ADC_Data` 示例。
- **LVGL**：8.4.0，与官方示例一致。

## 真机调试教训（勿回退）

1. `lv_lock/lv_unlock` 是**递归**互斥锁（普通 take 会在 LVGL 事件处理器里自锁）。
2. LVGL 单缓冲（buf2=NULL；双缓冲多占 23KB 内部 RAM 会挤爆网络任务栈）。
3. MJPEG 直播**必须块读**（逐字节 WiFiClient::read 打满 CPU0 → IDLE0 饿死 →
   任务看门狗 22s 一重启）。且抽取器必须处理整块数据的所有帧——块尾在 EOI 后的
   字节（含下一帧 SOI）不能丢，否则帧率腰斩。
4. rlogf 里绝不能 Serial 输出（USB CDC 阻塞会冻 UI）。
5. I2C 外设（触摸/codec/PMU）上电 boot 需要时间：setup 里 delay(1000) + 触摸 begin 重试。
6. 头像缓存路径 `/avatars_v2/`（EXIF 修复后的缓存版本）。
7. 直播帧率优化（实测数据见下）：视频带**绕过 LVGL canvas**——JPEGDEC 行优先解码到
   PSRAM 逻辑帧缓冲 ×3，reader（core0）/解码（core0）/转置直推（core1）三段流水，
   直推按面板 48 行分块、逻辑行内连续读 + 内部 staging 跨步写（PSRAM 跨步写会让
   解码耗时翻倍，勿用"解码时直接转置写"），staging 与 LVGL flush 共享 rot_buf
   （双方都在 lv_lock 内使用，互斥）。QSPI 已提速 80MHz（`gfx->begin(80000000)`）。
8. 直播实测帧率瓶颈在**服务端**：`api.py` 的 live_band 每连接循环 = 轮询源帧 →
   Pillow 重编码 → yield（阻塞于 TCP 排空）→ sleep(100ms)。Mac 上编码实测仅 ~1ms，
   但每连接到 ESP32 交付一帧约 150ms（cwnd 空闲重置 + 延迟 ACK），叠加 sleep 后
   每连接 ~3.3fps；curl 旁路实测源产帧 ~6.5fps。固件管线容量 ~13fps，不是瓶颈。
   要提速需服务端：按源帧缓存编码结果跨连接共享、降低 sleep、或提高 CAM 产帧率。

## 结构

```
src/
  main.cpp            启动：BSP → UI → WiFi → bootstrap/health 任务（直播自启动/离线恢复重启）
  state.{h,cpp}       全局状态（猫/计数/连接态）
  alert_wav.h         内置提示音（tools/gen_alert.py 生成）
  font_cjk20.c        内置中文字体（lv_font_conv 产物，见上）
  bsp/bsp_display.*   CO5300 QSPI + CST9220 + LVGL 移植层（含 90° 手工转置）
  bsp/bsp_audio.*     ES8311 + I2S DMA + 环形缓冲（codec 失败可降级）
  bsp/bsp_power.*     AXP2101 PMU + I2C 总线扫描诊断
  es8311/             官方 ES8311 驱动（原样拷贝）
  net/net_rest.*      REST：cats/today/clips/health + 头像 LittleFS 缓存（/avatars_v2/）
  net/net_events.*    /ws/events 客户端（指数退避 1s→30s）
  net/live.*          /api/live_band MJPEG（reader/解码/转置直推三段流水，绕过 LVGL canvas）
                      + /api/live_audio PCM
  net/rlog.*          远程日志（HTTP POST 到服务端）
  ui/ui.*             单页仪表盘 + 命令队列（跨线程安全）
```

## 内存布局

- LVGL draw buffer：内部 DMA RAM 单缓冲 480×24 行
- 头像解码缓冲：PSRAM 192×192×2 ×4
- 直播逻辑帧缓冲：PSRAM 480×240×2 ×3（流水线信箱）
- 直播 JPEG 信箱：PSRAM 400KB ×2
- 直推 staging：与 LVGL flush 共享 rot_buf（内部 DMA RAM 23KB）
- 音频环形缓冲：PSRAM 48KB（≈1.5s）

## 工具链

- `platform = espressif32@8.0.0`（Arduino core 3.3.12）。官方示例捆绑的 Arduino_GFX 1.6.4
  依赖 Arduino 3.x 的 periman 头，故不可用旧的 2.0.x 内核。
- Arduino_GFX 锁定 **1.6.9**（上游修好了 Arduino 3.3.6+ 的 SPI 兼容；其 CO5300/QSPI 驱动
  与官方示例捆绑的 1.6.4 逐字节一致）。
- JPEGDEC 的 S3 SIMD 汇编需要 esp-dsp 平台头；`include/dsps_fft2r_platform.h` 是其最小
  shim（S3 下仅需 `dsps_fft2r_sc16_aes3_enabled=1`），勿删。

## 已知限制

- 直播实测帧率受服务端每连接 pacing 限制（~3-6fps，视 CAM 产帧率与 Mac 负载），
  固件管线容量 ~13fps 已非瓶颈；提速需改服务端（见"真机调试教训"第 8 条）。
- 直播音视频为两条独立 HTTP 流，未做唇同步。
- 回放（/ws/clip）功能已按产品决策移除（player.* 已删除）。
- 触摸无 UI 交互，仅诊断打点。
- 视频直推覆盖区（时钟/状态点）靠每帧后 invalidate 两个小标签重绘，理论上有
  亚帧级覆盖闪烁，肉眼不可见。
