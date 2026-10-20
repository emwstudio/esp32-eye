"""内置合成数据源：无硬件时提供渐变/噪点 JPEG 帧（~12fps）与静音/正弦 PCM。"""
from __future__ import annotations

import asyncio
import io
import logging
import math
import struct
import time

import numpy as np
from PIL import Image

from .ringbuffer import AudioChunk, RingBuffer, VideoFrame

log = logging.getLogger(__name__)

MOCK_FPS = 12.0
MOCK_AUDIO_CHUNK_S = 0.1
AUDIO_RATE = 16000


def make_frame(t: float, motion: bool, size: int = 480) -> bytes:
    """合成一帧：平滑渐变 + 轻微噪点；motion=True 时叠加剧烈运动块。"""
    x = np.linspace(0, 1, size, dtype=np.float32)
    base = (np.outer(np.sin(2 * math.pi * (x + t * 0.05)), np.ones(size)) + 1) * 60
    grad = base + (x * 80)[None, :]
    noise = np.random.default_rng(int(t * 1000)).integers(-2, 3, (size, size))
    img = np.clip(grad + noise, 0, 255).astype(np.uint8)
    if motion:
        rng = np.random.default_rng(int(t * 7))
        img[100:300, 100:300] = rng.integers(0, 255, (200, 200))
    rgb = np.stack([img, img, img], axis=-1)
    buf = io.BytesIO()
    Image.fromarray(rgb).save(buf, format="JPEG", quality=70)
    return buf.getvalue()


def make_audio_chunk(t: float, seconds: float = MOCK_AUDIO_CHUNK_S) -> bytes:
    """静音 + 微弱 440Hz 正弦。"""
    n = int(AUDIO_RATE * seconds)
    samples = (np.sin(2 * math.pi * 440 * (t + np.arange(n) / AUDIO_RATE)) * 300).astype(
        np.int16
    )
    return samples.tobytes()


class MockCamSource:
    """把合成帧/音频直接写入环形缓冲。inject_motion() 供测试门控逻辑。"""

    def __init__(self, ring: RingBuffer) -> None:
        self.ring = ring
        self._motion_until = 0.0
        self._task: asyncio.Task[None] | None = None
        self._stopped = asyncio.Event()

    def inject_motion(self, seconds: float = 5.0) -> None:
        self._motion_until = time.time() + seconds

    @property
    def connected(self) -> bool:
        return self._task is not None and not self._task.done()

    async def _run(self) -> None:
        log.info("mock cam started (%.0f fps video, 16kHz audio)", MOCK_FPS)
        frame_interval = 1.0 / MOCK_FPS
        next_frame = time.time()
        next_audio = time.time()
        while not self._stopped.is_set():
            now = time.time()
            if now >= next_frame:
                motion = now < self._motion_until
                self.ring.add_frame(
                    VideoFrame(ts=now, device_ts=int(now * 1e6), jpeg=make_frame(now, motion))
                )
                next_frame += frame_interval
            if now >= next_audio:
                self.ring.add_audio(AudioChunk(ts=now, pcm=make_audio_chunk(now)))
                next_audio += MOCK_AUDIO_CHUNK_S
            await asyncio.sleep(0.01)
        log.info("mock cam stopped")

    def start(self) -> None:
        self._task = asyncio.create_task(self._run())

    async def stop(self) -> None:
        self._stopped.set()
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass


def pcm_duration_s(pcm: bytes) -> float:
    return len(pcm) / 2 / AUDIO_RATE


def pack_audio_frame(device_ts_us: int, pcm: bytes) -> bytes:
    return struct.pack("<Q", device_ts_us) + pcm
