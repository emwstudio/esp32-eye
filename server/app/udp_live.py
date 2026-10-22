"""UDP 直播推送：把直播带帧分片推送到注册的桌面屏。

TCP 到 ESP32 的投递受 lwIP 窗口/延迟 ACK 限制在 ~5fps；UDP 无 ACK/拥塞语义，
能打满链路。丢包只丢帧，直播画面可接受。

分片格式（每片一个 UDP 报文）：[frame_seq u32 LE][chunk_idx u16 LE][chunk_count u16 LE][payload]
"""
from __future__ import annotations

import asyncio
import io
import logging
import socket
import struct
import time

from PIL import Image

from .ringbuffer import RingBuffer

log = logging.getLogger(__name__)

UDP_CHUNK = 1400  # 小于常见 MTU，避免 IP 分片
HDR_FMT = "<IHH"
HDR_LEN = struct.calcsize(HDR_FMT)


def band_jpeg(jpeg: bytes, quality: int = 55, band_h: int = 240) -> bytes | None:
    """缩放后中心裁 480×band_h。半截帧返回 None。"""
    try:
        img = Image.open(io.BytesIO(jpeg)).convert("RGB")
    except OSError:
        return None
    w, h = img.size
    img = img.resize((480, int(h * 480 / w)))
    if img.size[1] < band_h:  # 源不够高就不裁
        band_h = img.size[1]
    top = max(0, (img.size[1] - band_h) // 2)
    img = img.crop((0, top, 480, top + band_h))
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=quality)
    return buf.getvalue()


# 全进程共享的直播带编码缓存（同一源帧同一高度只编码一次）
_band_cache: dict[tuple[float, int], bytes] = {}


async def get_band_jpeg(frame_jpeg: bytes, ts: float, band_h: int = 240) -> bytes | None:
    key = (ts, band_h)
    if key in _band_cache:
        return _band_cache[key]
    jpeg = await asyncio.to_thread(band_jpeg, frame_jpeg, 55, band_h)
    if jpeg is None:
        return None
    # 缓存只作优化：先裁剪再插入，返回本地变量，乱序到达的迟帧也不会 KeyError
    while len(_band_cache) >= 8:
        oldest = min(_band_cache)
        if oldest == key:
            break
        del _band_cache[oldest]
    _band_cache[key] = jpeg
    return jpeg


class UdpLivePush:
    """注册表 + 推送任务。register() 由 /api/live_udp 调用。"""

    def __init__(self, ring: RingBuffer) -> None:
        self.ring = ring
        self.targets: dict[tuple[str, int], float] = {}  # (host, port) -> 过期时刻
        self._task: asyncio.Task[None] | None = None
        self._sock: socket.socket | None = None

    def register(self, host: str, port: int, ttl_s: float = 60.0, band_h: int = 200) -> None:
        self.targets[(host, port)] = (time.time() + ttl_s, band_h)
        log.info("udp live target registered: %s:%d (ttl %.0fs)", host, port, ttl_s)

    async def start(self) -> None:
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setblocking(False)
        self._task = asyncio.create_task(self._run())

    async def stop(self) -> None:
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
        if self._sock:
            self._sock.close()

    async def _run(self) -> None:
        last_ts = 0.0
        seq = 0
        while True:
            now = time.time()
            expired = [k for k, (exp, _) in self.targets.items() if exp <= now]
            for k in expired:
                log.info("udp live target expired: %s:%d", *k)
                del self.targets[k]
            if self.targets:
                for frame in self.ring.frames_since(last_ts):  # 全量新帧，不合并
                    for (host, port), (_, band_h) in list(self.targets.items()):
                        jpeg = await get_band_jpeg(frame.jpeg, frame.ts, band_h)
                        if not jpeg:
                            continue
                        last_ts = frame.ts
                        seq += 1
                        chunks = [jpeg[i : i + UDP_CHUNK] for i in range(0, len(jpeg), UDP_CHUNK)]
                        count = len(chunks)
                        for ci, payload in enumerate(chunks):
                            self._sock.sendto(  # type: ignore[union-attr]
                                struct.pack(HDR_FMT, seq, ci, count) + payload, (host, port)
                            )
            else:
                if self.ring.latest_frame():
                    last_ts = self.ring.latest_frame().ts  # type: ignore[union-attr]
            await asyncio.sleep(0.002)
