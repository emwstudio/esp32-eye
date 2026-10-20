"""ingest：后台拉取 CAM 的 MJPEG 视频流与 WS 音频流，断线指数退避重连。"""
from __future__ import annotations

import asyncio
import logging
import struct
import time

import httpx
import websockets

from .config import Settings
from .mock_cam import MockCamSource
from .ringbuffer import AudioChunk, RingBuffer, VideoFrame

log = logging.getLogger(__name__)

BACKOFF_MIN = 1.0
BACKOFF_MAX = 30.0


class CamIngest:
    """真实 CAM 数据源。use_mock 时由 MockCamSource 替代。"""

    def __init__(self, settings: Settings, ring: RingBuffer) -> None:
        self.settings = settings
        self.ring = ring
        self.video_connected = False
        self.audio_connected = False
        self._tasks: list[asyncio.Task[None]] = []

    @property
    def connected(self) -> bool:
        return self.video_connected

    def start(self) -> None:
        base = self.settings.cam_base_url.rstrip("/")
        self._tasks = [
            asyncio.create_task(self._video_loop(f"{base}/stream")),
            asyncio.create_task(self._audio_loop(self._ws_url(base) + "/audio")),
        ]

    async def stop(self) -> None:
        for t in self._tasks:
            t.cancel()
        for t in self._tasks:
            try:
                await t
            except asyncio.CancelledError:
                pass

    @staticmethod
    def _ws_url(base: str) -> str:
        if base.startswith("https://"):
            return "wss://" + base[len("https://") :]
        if base.startswith("http://"):
            return "ws://" + base[len("http://") :]
        return base

    async def _video_loop(self, url: str) -> None:
        backoff = BACKOFF_MIN
        while True:
            try:
                await self._pull_mjpeg(url)
            except asyncio.CancelledError:
                raise
            except Exception as e:
                log.warning("cam video disconnected: %s; retry in %.0fs", e, backoff)
            self.video_connected = False
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, BACKOFF_MAX)

    async def _pull_mjpeg(self, url: str) -> None:
        buf = b""
        async with httpx.AsyncClient(timeout=None) as client:
            async with client.stream("GET", url) as resp:
                resp.raise_for_status()
                self.video_connected = True
                log.info("cam video stream connected: %s", url)
                async for data in resp.aiter_bytes(65536):
                    buf += data
                    while True:
                        soi = buf.find(b"\xff\xd8")
                        if soi < 0:
                            buf = buf[-2:]  # 保留可能的半个 SOI
                            break
                        eoi = buf.find(b"\xff\xd9", soi + 2)
                        if eoi < 0:
                            buf = buf[soi:]
                            break
                        jpeg = buf[soi : eoi + 2]
                        buf = buf[eoi + 2 :]
                        now = time.time()
                        self.ring.add_frame(
                            VideoFrame(ts=now, device_ts=int(now * 1e6), jpeg=jpeg)
                        )

    async def _audio_loop(self, url: str) -> None:
        backoff = BACKOFF_MIN
        while True:
            try:
                async with websockets.connect(url, max_size=None) as ws:
                    self.audio_connected = True
                    log.info("cam audio stream connected: %s", url)
                    backoff = BACKOFF_MIN
                    async for msg in ws:
                        if isinstance(msg, str) or len(msg) < 8:
                            continue
                        (device_ts,) = struct.unpack("<Q", msg[:8])
                        pcm = msg[8:]
                        if pcm:
                            self.ring.add_audio(
                                AudioChunk(ts=time.time(), pcm=pcm)
                            )
            except asyncio.CancelledError:
                raise
            except Exception as e:
                log.warning("cam audio disconnected: %s; retry in %.0fs", e, backoff)
            self.audio_connected = False
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, BACKOFF_MAX)


class IngestManager:
    """按配置选择真实 CAM 或 mock 源，统一管理生命周期。"""

    def __init__(self, settings: Settings, ring: RingBuffer) -> None:
        self.settings = settings
        self.ring = ring
        self.mock: MockCamSource | None = None
        self.real: CamIngest | None = None

    @property
    def cam_connected(self) -> bool:
        if self.mock is not None:
            return self.mock.connected
        if self.real is not None:
            return self.real.connected
        return False

    def start(self) -> None:
        if self.settings.use_mock_cam:
            log.info("CAM mock mode enabled")
            self.mock = MockCamSource(self.ring)
            self.mock.start()
        else:
            self.real = CamIngest(self.settings, self.ring)
            self.real.start()

    async def stop(self) -> None:
        if self.mock is not None:
            await self.mock.stop()
        if self.real is not None:
            await self.real.stop()
