"""ingest：后台拉取 CAM 的 MJPEG 视频流与 WS 音频流，断线指数退避重连。

时间戳策略：音频块在 CAM 端 NTP 已同步时使用设备时间戳（µs → 秒）；
未同步时回退接收时刻并警告一次。视频帧始终用接收时刻——MJPEG 流不含
逐帧设备时间戳，且 HTTP 管线缓冲带来的延迟差异是已知余项。
"""
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
DEVICE_TS_TOLERANCE_S = 60.0  # 设备时间戳与本地时钟偏差超过此值视为 NTP 未同步


def resolve_local_host(url: str) -> str:
    """把 .local/mDNS 主机名解析成 IP。

    TUN 模式代理会在网络层劫持所有 TCP 连接并按域名远程解析——mDNS 名字
    在代理端不可解析会导致连接被静默关闭，所以连 CAM 前先在本地解出 IP。
    解析失败时原样返回（让上层重试逻辑处理）。
    """
    import socket
    from urllib.parse import urlparse, urlunparse

    parts = urlparse(url)
    host = parts.hostname or ""
    if not (host.endswith(".local") or host.endswith(".lan")):
        return url
    try:
        ip = socket.getaddrinfo(host, None, socket.AF_INET)[0][4][0]
    except socket.gaierror:
        log.warning("mDNS resolve failed for %s", host)
        return url
    netloc = f"{ip}:{parts.port}" if parts.port else ip
    resolved = urlunparse(parts._replace(netloc=netloc))
    log.info("resolved %s -> %s", host, ip)
    return resolved


class CamIngest:
    """真实 CAM 数据源。use_mock 时由 MockCamSource 替代。"""

    def __init__(self, settings: Settings, ring: RingBuffer) -> None:
        self.settings = settings
        self.ring = ring
        self.video_connected = False
        self.audio_connected = False
        self._tasks: list[asyncio.Task[None]] = []
        self._device_ts_warned = False

    @property
    def connected(self) -> bool:
        return self.video_connected

    def audio_chunk_ts(self, device_ts_us: int, now: float) -> float:
        """NTP 已同步（与本地时钟偏差 ≤60s）时用设备时间戳，否则回退接收时刻。"""
        device_s = device_ts_us / 1e6
        if abs(now - device_s) <= DEVICE_TS_TOLERANCE_S:
            return device_s
        if not self._device_ts_warned:
            log.warning(
                "cam device timestamp unsynced (device=%.3f local=%.3f); "
                "falling back to receive time",
                device_s, now,
            )
            self._device_ts_warned = True
        return now

    def start(self) -> None:
        base = self.settings.cam_base_url.rstrip("/")
        self._tasks = [
            asyncio.create_task(self._video_loop(resolve_local_host(f"{base}/stream"))),
            asyncio.create_task(self._audio_loop(resolve_local_host(self.settings.audio_ws_url()))),
        ]

    async def stop(self) -> None:
        for t in self._tasks:
            t.cancel()
        for t in self._tasks:
            try:
                await t
            except asyncio.CancelledError:
                pass

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
        async with httpx.AsyncClient(timeout=None, trust_env=False) as client:
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
                async with websockets.connect(url, max_size=None, compression=None) as ws:
                    self.audio_connected = True
                    self._device_ts_warned = False  # 每次重连后重新判定 NTP 同步
                    log.info("cam audio stream connected: %s", url)
                    backoff = BACKOFF_MIN
                    async for msg in ws:
                        if isinstance(msg, str) or len(msg) < 8:
                            continue
                        (device_ts,) = struct.unpack("<Q", msg[:8])
                        pcm = msg[8:]
                        if pcm:
                            ts = self.audio_chunk_ts(device_ts, time.time())
                            self.ring.add_audio(AudioChunk(ts=ts, pcm=pcm))
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

    @property
    def audio_connected(self) -> bool:
        if self.mock is not None:
            return self.mock.connected
        if self.real is not None:
            return self.real.audio_connected
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
