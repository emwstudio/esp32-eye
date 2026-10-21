# catcam — 猫喝水监控摄像头节点

Waveshare **ESP32-S3-CAM-OV5640**（ESP32-S3R8：8MB Octal PSRAM / 16MB Flash）固件，
PlatformIO + Arduino core 3.2.0（pioarduino espressif32@54.03.20，ESP-IDF 5.4）。
注意：官方 platformio/espressif32 至今捆绑 core 2.0.17（无 `driver/i2s_std.h`），
要 core 3.x 必须用 pioarduino fork（platformio.ini 中已按发布包 URL 锁定）。

## 功能

- WiFi STA（凭证在 `include/config.h`，不入库）
- mDNS：`catcam.local`；NTP 对时（pool.ntp.org），音视频帧共用 epoch 微秒时间戳
- HTTP（80 端口，esp_http_server，多连接不互堵）：
  - `GET /stream` — MJPEG 流（`multipart/x-mixed-replace`），SVGA 800×600 JPEG，约 12fps
  - `GET /capture` — 单帧 JPEG
  - `GET /healthz` — JSON（uptime / RSSI / NTP 同步状态 / 堆内存 / 音频客户端数）
- 音频：ES7210 双麦 ADC → I2S 16kHz/16bit 立体声采集，L/R 混成单声道；
  `ws://catcam.local:81/audio` 向每个客户端推送二进制帧：
  `[8B uint64 LE 时间戳 µs][640B PCM = 20ms]`；无客户端时自动暂停 I2S 采集省电
- FreeRTOS 任务分离：摄像头抓取在 httpd worker、音频采集任务、WS 推送任务互不阻塞

## 用法

```bash
cp include/config.h.example include/config.h   # 填入 WIFI_SSID / WIFI_PASSWORD
pio run                                        # 编译
pio run -t upload && pio device monitor        # 烧录 + 串口日志
```

浏览器打开 `http://catcam.local/stream` 看画面。

## 引脚核对记录

已与微雪官方仓库 [waveshareteam/ESP32-S3-CAM-OVxxxx](https://github.com/waveshareteam/ESP32-S3-CAM-OVxxxx)
的 `examples/Arduino-v3.2.0/examples/02_CameraWebServer`（`CAMERA_MODEL_ESP_EYE`）交叉核对，
与板型开源固件 furby-connect-ai 的引脚表**完全一致**，无差异：

- 摄像头 DVP：XCLK=38, SIOD=8, SIOC=7, Y9..Y2=21/39/40/42/46/48/47/45, VSYNC=17, HREF=18, PCLK=41，XCLK 20MHz
- **OV5640 无软件控制的 PWDN/RESET（均为 -1）**；摄像头供电由 CH32 IO 扩展器（0x24）IO6 拉高使能。
  SCCB 与 I2C 共用 GPIO8/7。**总线共存方案（真机验证）**：esp32-camera 在 IDF≥5.4 下 SCCB 走新
  i2c_master 驱动（sccb-ng.c），与 Arduino Wire 的 legacy 驱动不能共占 I2C0（否则 ES7210 写报
  `ESP_ERR_INVALID_STATE`）。因此固件自建 i2c_master 总线（port 0，GPIO8/7，100kHz），摄像头以
  `pin_sccb_sda=-1, sccb_i2c_port=0` 挂到该总线，CH32/ES7210 也全部走同一新驱动，完全不使用 Wire。
  初始化顺序：`GPIO 总线恢复（9 个 SCL 脉冲）→ i2cBusInit() → CH32 IO6=1 → esp_camera_init() → ES7210`。
- I2S：MCLK=10, BCLK=11, LRCK=12, DIN=13（ES7210 → ESP32）, DOUT=14（→ ES8311，本节点未用）
- ES7210 配置取自官方 `06_esp_sr` 示例：16kHz / 16bit / 标准 I2S（非 TDM，MIC1→L、MIC2→R）、
  MCLK=256Fs=4.096MHz、bias 2.87V、增益 30dB、ADC 数字音量 +40dB

## 已知限制

- WS 音频在独立端口 81（`WebSockets` 库自建 server），HTTP 在 80；路径固定 `/audio`，
  其他路径的 WS 连接会被立即断开
- WiFi 连接失败仅串口诊断（启动时扫描全网 SSID/RSSI/信道、打印断开 reason 码）并后台重试，
  未实现 AP 配网模式（后续迭代）；国家码已设为 CN（1-13 信道）
- `/stream` 目标帧率 12fps，实际取决于链路；多客户端同时拉流时共享同一 12fps 抓取节奏
