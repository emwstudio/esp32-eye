# esp32-eye：多猫每日喝水计数器

用两块 ESP32-S3、一台 MacBook 和 MiniCPM-V 视觉模型，记录每只猫什么时候喝水、今天喝了几次，并保存带声音的喝水片段。

项目包含摄像头固件、Mac 服务端和 AMOLED 桌面屏固件。当前机位是**碗沿朝斜上方拍猫脸，画面里看不到水**，所以检测逻辑不是寻找水面或舔水动作，而是“猫进入画面并占据较大比例”。

## 功能

- OV5640 实时视频与麦克风音频采集
- 背景差分占比 + 本地 1B 模型门控，减少大模型调用
- 云端 35B 模型复核喝水画面并对照参考照片认猫
- 保存事件时间、猫咪身份、今日次数和带声音的 MP4 片段
- 桌面屏实时视频带、四猫头像、每日计数、进度条、提醒音和高亮动画
- 断流重连、WebSocket 心跳、视频分级看门狗和 UI 卡死自动复位
- 每天零点按服务端日期重新计数
- 可独立切换的拍摄演示模式：固定计数 + 循环视频，不影响真实数据库

## 系统结构

```text
┌──────────────────────┐
│ ESP32-S3-CAM-OV5640  │
│ 摄像头 + 麦克风       │
└──────────┬───────────┘
           │ MJPEG / PCM over Wi-Fi
           ▼
┌──────────────────────┐
│ MacBook FastAPI      │
│                      │
│ 背景差分占比          │
│   ↓                  │
│ 本地 MiniCPM-V 1B    │  判断画面里有没有猫
│   ↓ 连续有效帧        │
│ 云端 MiniCPM-V 35B   │  复核 + 四选一认猫
│   ↓                  │
│ SQLite + MP4 片段    │
└──────────┬───────────┘
           │ REST / WebSocket / MJPEG
           ▼
┌──────────────────────┐
│ ESP32-S3-Touch-      │
│ AMOLED-2.16          │
│ 视频带 + 四猫计数卡片 │
└──────────────────────┘
```

## 硬件

| 模块 | 型号 | 用途 |
|---|---|---|
| 摄像头板 | Waveshare ESP32-S3-CAM-OV5640 | 碗沿视频与音频采集 |
| 摄像头 | OV5640 | 1024×768 JPEG 画面 |
| 桌面屏 | Waveshare ESP32-S3-Touch-AMOLED-2.16 | 480×480 AMOLED 仪表盘 |
| 电脑 | Apple Silicon MacBook | 视频接入、检测、识别、存储和推流 |

## 检测与识别逻辑

### 1. 轻量门控

- 与“空碗背景”做灰度差分，估计猫在画面中的占比。
- 每 **1.5 秒**检查一次，但不是每帧都调用 1B。
- 前景占比 **> 15%** 才调用本地 1B；占比较低时直接记否定票。
- 本地 **MiniCPM-V-4.7-1B（llama.cpp，Q4_K_M）**只负责判断“画面里有没有猫”。
- 1B 看到猫，且前景占比 **> 35%**，才形成一次肯定票。
- 连续 **3 次肯定**开始事件，连续 **4 次否定**结束事件。

1B 不负责最终认猫。实测三只浅色猫外观相近，1B 的四选一区分不可靠，因此不使用它做离线身份确认。详细结果见 `docs/cat-id-spike-report.md`。

### 2. 身份确认

事件结束后，服务端抽取事件帧，调用 **MiniCPM-V-4.7-35B-A3B**：

- 复核画面是否符合碗边喝水场景。
- 对照每只猫的参考照片，返回 `cat_id`、置信度和 `is_drinking`。
- 非喝水画面会被丢弃，不计数。
- 身份不明确时保留 `unknown`，不强行指定一只猫。
- 云端暂时不可用时进入重试流程，不用 1B 结果冒充可靠的身份确认。

### 3. 存储

- SQLite：事件时间、身份、状态、置信度、时长和片段路径。
- MP4：保留事件前 5 秒、事件后 3 秒及事件过程，音轨按原始采集导出。
- 环形视频缓冲：40 秒。
- 桌面屏通过 REST 和 WebSocket 获取计数与事件通知。

## 桌面屏

当前是单页仪表盘：

- 标题栏：时间、标题、在线状态。
- 视频带：480×200，位于标题栏下方。
- 四张猫卡：头像、名字、今日次数和 `/8次` 进度条。
- `drinking_started`：立即播放本地提醒音。
- `cat_confirmed`：对应猫卡高亮、数字动画和次数加一。
- 不播放摄像头环境声。
- 跨天自动重新获取今日计数。

看门狗策略：

| 故障 | 处理 |
|---|---|
| 视频 5 秒无上屏 | 重新注册 UDP，并确保 TCP 回退 |
| 视频 20 秒无上屏 | 重启完整直播管线 |
| 视频 60 秒无上屏 | 重启设备 |
| UI 20 秒无循环心跳 | 最高优先级看门狗直接重启设备 |

## 仓库结构

```text
assets/cats/             四只猫的参考照片与头像
docs/                   API 契约、模型实测和调试记录
firmware/cam/           OV5640 摄像头固件
firmware/display/       AMOLED 桌面屏固件与字体工具
server/app/             FastAPI、检测、识别、录像和存储
server/tests/           服务端自动化测试
server/tools/           模型与录像实测脚本
```

## 环境准备

### 软件依赖

- Python 3.12+
- PlatformIO
- ffmpeg / ffprobe
- llama.cpp（提供 OpenAI 兼容的 `llama-server`）
- ModelBest API Key（仅云端 35B 身份确认需要）

Python 包：

```bash
cd server
python3 -m venv .venv
.venv/bin/pip install \
  fastapi uvicorn httpx pillow numpy python-dotenv \
  pytest pytest-asyncio
```

### 配置服务端

```bash
cp server/.env.example server/.env
```

编辑 `server/.env`：

- `CAM_BASE_URL`：默认 `http://catcam.local`，也可写摄像头固定 IP。
- `GATE_MODEL_BASE_URL`：默认 `http://127.0.0.1:8080/v1`。
- `ID_MODEL_API_KEY`：ModelBest 云端模型 Key。

真实 `.env` 已被 `.gitignore` 排除，不要提交。

### 启动本地 1B

用 llama.cpp 启动 MiniCPM-V-4.7-1B 和对应 mmproj，监听 8080：

```bash
llama-server \
  -m /path/to/MiniCPM-V-4_7-1B-Q4_K_M.gguf \
  --mmproj /path/to/mmproj.gguf \
  --port 8080
```

模型文件名以本地下载为准。服务端通过 OpenAI 兼容接口调用，不需要把 35B 部署在 Mac 本地。

### 启动服务端

```bash
cd server
./run.sh
```

或者：

```bash
cd server
.venv/bin/python -m uvicorn app.main:app --host 0.0.0.0 --port 8000
```

健康检查：

```bash
curl http://127.0.0.1:8000/api/health
curl http://127.0.0.1:8000/api/today
```

## 编译和烧录

先分别复制配置模板并填写 Wi-Fi 与服务端地址：

```bash
cp firmware/cam/include/config.h.example firmware/cam/include/config.h
cp firmware/display/include/config.h.example firmware/display/include/config.h
```

`firmware/display/include/config.h` 中的 `SERVER_BASE` 应指向 Mac 的局域网地址，例如 `http://192.168.1.100:8000`。两个 `config.h` 都被 `.gitignore` 排除。

摄像头板：

```bash
cd firmware/cam
pio run -e esp32-s3-cam-ov5640 -t upload --upload-port /dev/cu.usbmodemXXXX
```

桌面屏：

```bash
cd firmware/display
pio run -e amoled216 -t upload --upload-port /dev/cu.usbmodemXXXX
```

macOS 换 USB 口后设备名可能变化。烧录前建议先核对芯片 MAC，避免把桌面固件烧到另一块板：

```bash
python3 ~/.platformio/packages/tool-esptoolpy/esptool.py \
  --chip esp32s3 --port /dev/cu.usbmodemXXXX read_mac
```

## 拍摄演示模式

演示模式用于视频拍摄，和真实监控数据隔离：

- 视频带改为循环播放 `server/data/demo_loop.mp4`，当前为 12fps、无黑边、静音。
- 桌面屏固定显示演示计数，真实 REST/WS 计数不会覆盖它。
- 真实摄像头识别、录像和 SQLite 不写入演示数字。
- 演示视频和真实数据库修改不纳入 Git 提交。

当前演示计数：宝宝 5、啵啵 4、球球 6、驴驴 3。

在 `firmware/display/include/config.h` 中：

```cpp
#define DEMO_MODE 1
#define DEMO_VIDEO_PATH "/api/demo_band"
```

恢复真实画面和真实计数：

```cpp
#define DEMO_MODE 0
```

修改后重新编译、烧录桌面屏。`server/data/demo_loop.mp4` 需要自行放置；文件不存在时 `/api/demo_band` 返回 404。

## API 速查

| 端点 | 用途 |
|---|---|
| `GET /api/health` | 摄像头、音频和模型配置状态 |
| `GET /api/cats` | 猫咪档案 |
| `GET /api/today` | 今日真实计数 |
| `GET /api/clips` | 已保存片段列表 |
| `GET /api/live` | 480×480 MJPEG 实时流 |
| `GET /api/live_band?h=200` | 480×200 MJPEG 视频带 |
| `GET /api/live_audio` | 16kHz / 16bit / 单声道 PCM |
| `GET /api/demo_band` | 循环演示视频，可用 `?frames=N` 限制帧数 |
| `WS /ws/events` | 计数、开始喝水、身份确认和撤销事件 |

调试事件接口 `POST /api/debug/trigger` 会创建测试事件，不要在正式数据中随意调用。

## 测试与实测记录

运行服务端测试：

```bash
cd server
.venv/bin/pytest -q
```

当前为 **43 项测试通过**。

已有模型与真机记录：

- 35B 先导四选一测试：8 组答对 8 组。
- 人工核对的 14 次真实喝水：均触发记录，14 次身份均判断正确。
- 另有 1 次非喝水画面在 35B 复核阶段被过滤。
- 上述数字只代表对应测试样本，不等同于长期识别率承诺。

## 已知限制

- 摄像头没有红外夜视，夜间关灯后无法保证检测。
- 身份确认依赖云端 35B；网络或 API 不可用时，事件会延迟确认。
- 外观相近的猫仍可能在复杂角度、遮挡或低光下误判，需要保留人工纠正入口。
- 系统记录的是靠近碗边的事件，不是精确饮水量，也不用于疾病诊断。
- 桌面屏 UDP 接收异常时会使用 TCP 回退，实际帧率随 Wi-Fi 和解码负载波动。
- 演示模式只是拍摄辅助，不代表真实识别结果。

更多协议和模型评测见：

- `docs/api-contract.md`
- `docs/cat-id-spike-report.md`
- `firmware/cam/README.md`
- `firmware/display/README.md`

## License

本项目采用 [MIT License](LICENSE)。复用模型和硬件示例前，请同时遵守对应上游项目的许可证。
