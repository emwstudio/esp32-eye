# esp32-eye 桌面显示屏固件（Waveshare ESP32-S3-Touch-AMOLED-2.16）

猫喝水监控系统的桌面显示节点。480×480 AMOLED 主页显示四只猫头像与今日喝水次数，
喝水事件实时横幅提醒，支持片段回放（WS 音视频同步）与实时直播（MJPEG + PCM）。

## 接口契约

严格遵守 `../docs/api-contract.md`（REST + WS 事件 + WS 片段回放二进制帧）。

## 快速开始

```bash
cp include/config.h.example include/config.h   # 填写 WiFi 与 SERVER_BASE
pio run                                        # 编译
pio run --target upload                        # 烧录固件
```

可选：中文字体（猫名为中文时显示更美观，缺省时回退 Montserrat，汉字显示为方框）：

```bash
tools/gen_font.sh                              # 生成 data/font/cjk20.bin（需 node/npx）
pio run --target uploadfs                      # 烧录 LittleFS
```

## BSP 来源与适配

以微雪官方仓库 [waveshareteam/ESP32-S3-Touch-AMOLED-2.16](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16)
的 Arduino 示例为底座：

- **显示**：`05_LVGL_Widgets` 示例的移植层 —— `Arduino_ESP32QSPI` + `Arduino_CO5300`、
  LVGL flush/rounder 回调、双 DMA 缓冲。官方捆绑的 Arduino_GFX 1.6.4 与上游
  [moononournation/Arduino_GFX v1.6.4](https://github.com/moononournation/Arduino_GFX/tree/v1.6.4)
  逐字节一致（仅换行符差异），故直接锁定上游依赖，未拷贝 BSP 库。
- **触摸**：`TouchDrvCST92xx`（CST9220），沿用官方 `setSwapXY(true)` + `setMirrorXY(true,false)` 映射。
- **音频**：`07_ES8311` 示例的 ES8311 初始化序列原样保留（`src/es8311/`，Espressif 官方驱动），
  I2S 层从 ESP_I2S 改为 legacy `driver/i2s.h`，以获得 DMA 配置与环形缓冲控制；
  源为 16kHz/16bit/mono PCM（契约），复制成立体声送 codec，PA_CTRL=GPIO46 常开。
- **LVGL**：8.4.0，与官方示例一致；`include/lv_conf.h` 来自官方捆绑版本，关闭了 demo、
  精简字体并开启 POSIX FS（`lv_font_load` 加载 LittleFS 中文字体）。

## 结构

```
src/
  main.cpp            启动：BSP → UI → WiFi → bootstrap/health 任务
  state.{h,cpp}       全局状态（猫/计数/连接态）
  alert_wav.h         内置提示音（tools/gen_alert.py 生成）
  bsp/bsp_display.*   CO5300 QSPI + CST9220 + LVGL 移植层
  bsp/bsp_audio.*     ES8311 + I2S DMA + 环形缓冲 + 播放时钟（A/V 同步基准）
  es8311/             官方 ES8311 驱动（原样拷贝）
  net/net_rest.*      REST：cats/today/clips/health + 头像 LittleFS 缓存
  net/net_events.*    /ws/events 客户端（指数退避 1s→30s）
  net/player.*        /ws/clip/{id} 二进制帧解析，音频时钟对齐视频
  net/live.*          /api/live MJPEG + /api/live_audio PCM 双流
  ui/ui.*             五屏 UI + 命令队列（跨线程安全）
```

## 内存布局

- LVGL draw buffer：内部 DMA RAM 2×480×48 行
- 头像解码缓冲：PSRAM 192×192×2 ×4
- 播放/直播帧双缓冲：PSRAM（320² 与 480²）
- 音频环形缓冲：PSRAM 48KB（≈1.5s）

## 工具链

- `platform = espressif32@8.0.0`（Arduino core 3.3.12）。官方示例捆绑的 Arduino_GFX 1.6.4
  依赖 Arduino 3.x 的 periman 头，故不可用旧的 2.0.x 内核。
- Arduino_GFX 锁定 **1.6.9**（上游修好了 Arduino 3.3.6+ 的 SPI 兼容；其 CO5300/QSPI 驱动
  与官方示例捆绑的 1.6.4 逐字节一致）。
- JPEGDEC 的 S3 SIMD 汇编需要 esp-dsp 平台头；`include/dsps_fft2r_platform.h` 是其最小
  shim（S3 下仅需 `dsps_fft2r_sc16_aes3_enabled=1`），勿删。

## 已知限制

- JPEG 解码帧率、WS 音视频同步手感无法在编译期验证，需上机实测。
- 中文渲染依赖 `tools/gen_font.sh` 生成字体 + `uploadfs`；未烧录时猫名中文显示为方框。
- 播放页无拖动 seek（契约未提供）。
- 直播音视频为两条独立 HTTP 流，未做唇同步（音频自带抖动缓冲）。
