"""detector：背景差分占比门控（纯 CV，无 VLM）+ 状态机防抖。

机位语义：摄像头固定在水碗碗沿直拍猫脸，画面里看不到水。
猫凑到碗边喝水 → 与"空碗背景"产生大面积差异（占比高），低头静止的舔水间隙占比也在；
猫离开 → 画面回到背景 → 占比掉回低位。背景只在场景近乎空时缓慢自适应
（猫在场期间不会被学进背景）。

实测依据：1B VLM 对"猫是否占满画面"的判定不稳定（同构图多次结果不一致），
占比是确定性可算的指标，阈值按日志实测校准。
"""
from __future__ import annotations

import asyncio
import io
import logging
import time
from typing import Callable

import numpy as np
from PIL import Image

from .config import Settings
from .ringbuffer import RingBuffer

log = logging.getLogger(__name__)

DIFF_SIZE = 32          # 降采样灰度图边长
OCC_DIFF_TH = 25        # 像素灰度差超过该值计入"被占据"
BG_EMPTY_RATIO = 0.10   # 占比低于该值认为场景为空，允许背景自适应
BG_ADAPT_ALPHA = 0.02   # 空场景下背景每帧向当前画面靠拢 2%（抗光线漂移）

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
        on_event_start: Callable[[float], int],
        on_event_end: EventHook,
    ) -> None:
        self.settings = settings
        self.ring = ring
        self.on_event_start = on_event_start
        self.on_event_end = on_event_end

        self._bg_gray: np.ndarray | None = None
        self._high_since: float | None = None  # 占比钉死在高位（>0.9）的起始时刻
        self._last_vote_ts = 0.0
        self._positive = 0
        self._negative = 0
        self._first_positive_ts: float | None = None
        self.event_id: int | None = None  # 进行中事件 id
        self._last_frame_ts = 0.0
        self._task: asyncio.Task[None] | None = None
        self._stat_task: asyncio.Task[None] | None = None
        self._lock = asyncio.Lock()
        # 实测期观测计数（stat_loop 每分钟汇总一次）
        self._stat_frames = 0
        self._stat_gate = 0
        self._stat_pos = 0

    @property
    def in_event(self) -> bool:
        return self.event_id is not None

    def reset_baseline(self) -> None:
        """清空背景基线（下一帧重建）。机位挪动/测试手动喂帧前用。"""
        self._bg_gray = None

    def occupancy_ratio(self, jpeg: bytes) -> float:
        """与空碗背景差分，返回被前景（猫）占据的像素比例。"""
        g = gray_small(jpeg)
        if self._bg_gray is None:
            self._bg_gray = g
            return 0.0
        ratio = float(np.mean(np.abs(g - self._bg_gray) > OCC_DIFF_TH))
        if ratio < BG_EMPTY_RATIO:
            self._bg_gray = self._bg_gray * (1 - BG_ADAPT_ALPHA) + g * BG_ADAPT_ALPHA
        return ratio

    async def process_frame(self, jpeg: bytes, ts: float) -> None:
        async with self._lock:
            self._stat_frames += 1
            ratio = self.occupancy_ratio(jpeg)
            # 占比钉死在 ~1.0 超过 120s：镜头被盖住或机位挪动了，强制重建背景基线
            if ratio > 0.9:
                now = time.time()
                if self._high_since is None:
                    self._high_since = now
                elif now - self._high_since > 120:
                    log.warning("occupancy pinned high 120s, re-baselining (camera moved or lens covered?)")
                    self.reset_baseline()
                    self._high_since = None
                    ratio = 0.0
            else:
                self._high_since = None
            if ts - self._last_vote_ts < self.settings.gate_interval_s:
                return
            self._last_vote_ts = ts
            self._stat_gate += 1
            positive = ratio > self.settings.occupancy_ratio_threshold
            if positive:
                self._stat_pos += 1
            # 有活动（占比非零）或事件进行中才记票，安静期不刷屏
            if positive or self.in_event or ratio > BG_EMPTY_RATIO:
                log.info(
                    "occupancy vote: ratio=%.2f positive=%d (pos_streak=%d)",
                    ratio, positive, self._positive,
                )
            self._vote(positive, ts, ratio)

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
        """实测期观测：每分钟汇总一次漏斗（帧→投票→肯定）。"""
        while True:
            await asyncio.sleep(60)
            log.info(
                "detector stat: frames=%d votes=%d pos=%d (in_event=%s)",
                self._stat_frames, self._stat_gate, self._stat_pos, self.event_id,
            )
            self._stat_frames = self._stat_gate = self._stat_pos = 0

    def start(self) -> None:
        self._last_frame_ts = time.time()
        self._task = asyncio.create_task(self._poll_loop())
        self._stat_task = asyncio.create_task(self._stat_loop())

    async def stop(self) -> None:
        for t in (self._task, self._stat_task):
            if t:
                t.cancel()
        for t in (self._task, self._stat_task):
            if t:
                try:
                    await t
                except asyncio.CancelledError:
                    pass
