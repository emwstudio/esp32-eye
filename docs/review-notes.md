
# deferred minors（审查遗留，暂不修）
- server/app/api.py: PIL 解码/缩放在事件循环内同步执行，可改 asyncio.to_thread
- firmware/cam: MJPEG 发送期间持有摄像头互斥锁，慢客户端会卡住 /capture
- display: 提示音与回放共用音频时钟，同时发生时时钟瞬移
- display: ui 状态读竞态（低风险）
- server: ingest httpx 无读超时，CAM 半死不触发重连
- 根 .gitignore 未忽略 server/data/app.db
