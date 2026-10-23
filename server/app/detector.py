"""detector：第 0 层帧差运动门控 + 第 1 层 1B 模型抽帧判定 + 状态机防抖。"""
from __future__ import annotations

import asyncio
import io
import logging
import time
from typing import Awaitable, Callable

import numpy as np
from PIL import Image

from .config import Settings
from .ringbuffer import RingBuffer

log = logging.getLogger(__name__)

DIFF_SIZE = 32  # 降采样灰度图边长

# gate_fn(jpeg_bytes) -> (is_drinking, confidence)
GateFn = Callable[[bytes], Awaitable[tuple[bool, float]]]
EventHook = Callable[[int, float], None]


def gray_small(jpeg: bytes) -> np.ndarray:
    img = Image.open(io.BytesIO(jpeg)).convert("L").resize((DIFF_SIZE, DIFF_SIZE))
    return np.asarray(img, dtype=np.float32)


class DrinkingDetector:
    """连续 TRIGGER_CONSECUTIVE 次肯定触发事件，连续 RELEASE_CONSECUTIVE 次否定结束。"""

    def __init__(
        self,
        settings: Settings,
        ring: RingBuffer,
        gate_fn: GateFn,
        on_event_start: Callable[[float], int],
        on_event_end: EventHook,
    ) -> None:
        self.settings = settings
        self.ring = ring
        self.gate_fn = gate_fn
        self.on_event_start = on_event_start
        self.on_event_end = on_event_end

        self._last_gray: np.ndarray | None = None
        self._last_gate_ts = 0.0
        self._positive = 0
        self._negative = 0
        self._first_positive_ts: float | None = None
        self.event_id: int | None = None  # 进行中事件 id
        self._last_frame_ts = 0.0
        self._task: asyncio.Task[None] | None = None
        self._lock = asyncio.Lock()
        # 实测期观测计数（stat_loop 每分钟汇总一次）
        self._stat_frames = 0
        self._stat_motion = 0
        self._stat_gate = 0
        self._stat_pos = 0

    @property
    def in_event(self) -> bool:
        return self.event_id is not None

    def motion_detected(self, jpeg: bytes) -> bool:
        """第 0 层：相邻帧降采样灰度帧差均值 > 阈值。"""
        g = gray_small(jpeg)
        prev, self._last_gray = self._last_gray, g
        if prev is None:
            return False
        diff = float(np.mean(np.abs(g - prev)))
        return diff > self.settings.motion_diff_threshold

    async def process_frame(self, jpeg: bytes, ts: float) -> None:
        async with self._lock:
            self._stat_frames += 1
            if not self.motion_detected(jpeg):
                return
            self._stat_motion += 1
            if ts - self._last_gate_ts < self.settings.gate_interval_s:
                return
            self._last_gate_ts = ts
            self._stat_gate += 1
            try:
                is_drinking, conf = await self.gate_fn(jpeg)
            except Exception as e:
                log.warning("gate model error (counted as negative): %s", e)
                is_drinking, conf = False, 0.0
            # 实测期全量留痕：每次门控判定都记（只有运动通过才有，最多 1/1.5s）
            log.info("gate verdict: drinking=%d conf=%.2f (pos_streak=%d)", is_drinking, conf, self._positive)
            if is_drinking:
                self._stat_pos += 1
            self._vote(is_drinking, ts, conf)

    def _vote(self, positive: bool, ts: float, conf: float) -> None:
        if positive:
            if self._positive == 0:
                self._first_positive_ts = ts
            self._positive += 1
            self._negative = 0
        else:
            self._negative += 1
            self._positive = 0
            self._first_positive_ts = None

        if not self.in_event and self._positive >= self.settings.trigger_consecutive:
            start_ts = self._first_positive_ts if self._first_positive_ts is not None else ts
            self.event_id = self.on_event_start(start_ts)
            log.info("drinking_started event_id=%d start_ts=%.3f", self.event_id, start_ts)
        elif self.in_event and self._negative >= self.settings.release_consecutive:
            event_id, self.event_id = self.event_id, None
            self._positive = 0
            self._negative = 0
            log.info("drinking event %d released at %.3f", event_id, ts)
            self.on_event_end(event_id, ts)

    async def _poll_loop(self) -> None:
        """后台循环：从环形缓冲消费新帧。"""
        while True:
            for f in self.ring.frames_since(self._last_frame_ts):
                self._last_frame_ts = f.ts
                await self.process_frame(f.jpeg, f.ts)
            await asyncio.sleep(0.1)

    async def _stat_loop(self) -> None:
        """实测期观测：每分钟汇总一次各级漏斗（帧→运动→门控→肯定）。"""
        while True:
            await asyncio.sleep(60)
            log.info(
                "detector stat: frames=%d motion=%d gate=%d pos=%d (in_event=%s)",
                self._stat_frames, self._stat_motion, self._stat_gate,
                self._stat_pos, self.event_id,
            )
            self._stat_frames = self._stat_motion = self._stat_gate = self._stat_pos = 0

    def start(self) -> None:
        self._last_frame_ts = time.time()
        self._task = asyncio.create_task(self._poll_loop())
        self._stat_task = asyncio.create_task(self._stat_loop())

    async def stop(self) -> None:
        for t in (self._task, getattr(self, "_stat_task", None)):
            if t:
                t.cancel()
        for t in (self._task, getattr(self, "_stat_task", None)):
            if t:
                try:
                    await t
                except asyncio.CancelledError:
                    pass
