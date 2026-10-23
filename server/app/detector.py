"""detector：背景差分占比 + 1B VLM 猫识别 → 投票防抖。

机位语义：摄像头固定在水碗碗沿朝斜上方直拍猫脸，拍不到水。
"猫在喝水"的判定 = 猫进了摄像头且占据大部分画面：
- 第 0 层（纯 CV 占比）：与"空碗背景"差分算前景占比。
  占比 ≤ 前置阈值 → 场景没东西，直接否定票（猫离开 → 事件结束），不惊动模型；
  占比 > 触发阈值 → 才算"占满画面"。
- 第 1 层（1B VLM）：占比过前置阈值时把帧交给 1B 回答"画面里有没有猫"
  （实测它能稳定认出猫，但不具备空间占比判断能力——占比交给 CV）。
- 肯定票 = 1B 看到猫 且 占比过触发阈值。每票两者都留日志（1B 是本项目的
  评测对象，日志即实测素材）。
- 背景只在场景近乎空时缓慢自适应（猫在场不会被学进背景）；
  占比钉死高位 120s 强制重建基线（镜头被盖/挪机位）。
"""
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

DIFF_SIZE = 32          # 降采样灰度图边长
OCC_DIFF_TH = 25        # 像素灰度差超过该值计入"被占据"
BG_EMPTY_RATIO = 0.10   # 占比低于该值认为场景为空，允许背景自适应
BG_ADAPT_ALPHA = 0.02   # 空场景下背景每帧向当前画面靠拢 2%（抗光线漂移）

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

            # 前置过滤：场景近乎空 → 直接否定票（猫走了事件才能结束），不调模型
            if ratio <= self.settings.occupancy_prefilter_ratio:
                if self.in_event:
                    log.info("occupancy empty: ratio=%.2f → 否定票 (neg_streak=%d)", ratio, self._negative)
                self._vote(False, ts, 0.0)
                return

            self._stat_gate += 1
            try:
                cat_present, conf = await self.gate_fn(jpeg)
            except Exception as e:
                log.warning("gate model error (counted as negative): %s", e)
                cat_present, conf = False, 0.0
            # 肯定票 = 1B 看到猫 且 占比够高（"猫进画面且占大部分"=在碗边喝水，碗沿机位语义）
            is_drinking = cat_present and ratio > self.settings.occupancy_ratio_threshold
            # 实测期全量留痕：每次 1B 判定 + 占比都记（评测素材）
            log.info(
                "gate verdict: cat=%d ratio=%.2f → drinking=%d conf=%.2f (pos_streak=%d)",
                cat_present, ratio, is_drinking, conf, self._positive,
            )
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
        """实测期观测：每分钟汇总一次漏斗（帧→模型调用→肯定）。"""
        while True:
            await asyncio.sleep(60)
            log.info(
                "detector stat: frames=%d gate_calls=%d pos=%d (in_event=%s)",
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
