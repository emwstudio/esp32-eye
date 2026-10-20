# esp32-eye API 契约（Mac 服务 ↔ AMOLED 桌面屏）

本文档是 server（FastAPI）与 firmware/display（ESP32 AMOLED）之间的唯一接口权威来源。
所有端点默认挂在 Mac 服务上，桌面屏通过 `http://<mac-ip>:8000` 访问。

## 数据模型

### Cat（猫档案）
```json
{ "id": "buding", "name": "布丁", "avatar_url": "/api/cats/buding/avatar" }
```

### Event（喝水事件）
```json
{
  "id": 123,
  "cat_id": "buding",            // 确认前为 null
  "confidence": 0.86,            // 确认前为 null
  "status": "confirming",        // confirming | confirmed | unknown | discarded
  "start_ts": 1729500000.0,      // Unix 秒（浮点）
  "end_ts": 1729500035.0,
  "duration_s": 35.0,
  "clip_path": "data/clips/2026-10-21/123.mp4"
}
```

## REST API

- `GET /api/cats` → `Cat[]`
- `GET /api/cats/{cat_id}/avatar` → JPEG（约 192×192，供桌面屏缓存）
- `GET /api/today` → `{ "date": "2026-10-21", "counts": { "buding": 3, "naigai": 1 }, "events_today": 4 }`
- `GET /api/clips?cat_id=&date=YYYY-MM-DD` → `Event[]`（仅 status=confirmed/unknown，按 start_ts 倒序）
- `POST /api/events/{id}/correct` body `{ "cat_id": "naigai" }` → 更新归属（人工纠正）
- `GET /api/live` → MJPEG 流（`multipart/x-mixed-replace`），480×480，~10fps（由 CAM 源缩放）
- `GET /api/live_audio` → 裸 PCM 流（`application/octet-stream`，16kHz 16bit 单声道小端，连续推送）
- `GET /api/health` → `{ "ok": true, "cam_connected": true, "gate_model": "...", "id_model": "..." }`

## WebSocket：事件推送 `/ws/events`

桌面屏连接后，服务端推送 JSON 文本帧：

```json
{ "type": "drinking_started", "event_id": 123, "ts": 1729500000.0 }
{ "type": "cat_confirmed", "event_id": 123, "cat_id": "buding", "confidence": 0.86 }
{ "type": "event_discarded", "event_id": 123, "reason": "false_positive" }
{ "type": "count_updated", "date": "2026-10-21", "counts": { "buding": 3 } }
{ "type": "hello", "counts": { "buding": 2, "naigai": 1 }, "date": "2026-10-21" }
```

- `hello`：连接建立时立即下发（当前计数快照）。
- `drinking_started`：门控判定瞬间即推，不等待身份确认。
- `cat_confirmed`：仅当身份确认为具体某只猫时下发（`cat_id` 必为非空字符串），可能在 `drinking_started` 后几秒到十几秒到达。
- `cat_unknown`：`{ "type": "cat_unknown", "event_id": 123 }` —— 身份确认完成但置信度不足，事件保留但不归属任何猫；桌面屏把"确认中"横幅切换为"未识别是哪家喵"样式，不加计数。
- `event_discarded`：事件被否决（如身份确认模型判定"并非在喝水"的误报）；桌面屏撤销"确认中"横幅，不加计数。
- 计数只在收到 `cat_confirmed`（或人工纠正后的 `count_updated`）时才 +1，避免误报虚增。

## WebSocket：片段回放 `/ws/clip/{event_id}`

桌面屏连接此端点后，服务端交错推送二进制帧（边下边播）：

- 每个二进制帧格式：`[1B type][4B uint32 LE 时间戳 ms][4B uint32 LE 长度][payload]`
  - `type=0x01`：视频帧，payload 为 320×320 JPEG；
  - `type=0x02`：音频块，payload 为 PCM（16kHz 16bit 单声道小端）；
  - 时间戳为该帧/块相对片段起点的播放时刻（ms）。
- 结束时发送文本帧 `{"type":"clip_end"}`。
- 桌面屏以音频为时钟，按时间戳显示视频帧；音频抖动缓冲 200-500ms。

## 约定

- 桌面屏断线后自动重连 `/ws/events`（指数退避，1s→30s 封顶）。
- 日期分组一律以 Mac 本地时区为准（服务端下发 `date` 字段）。
