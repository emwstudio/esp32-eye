"""identifier：事件结束后调云端 35B 模型复核喝水并确认是哪只猫，失败重试（最多 3 次，退避）。

三种终态：is_drinking=false → discarded（误报）；置信度不足或认不出 → unknown；
确认成功 → confirmed。结果经 on_identified 回调交给上层广播。
"""
from __future__ import annotations

import asyncio
import io
import logging
from typing import Awaitable, Callable
from pathlib import Path

import numpy as np
from PIL import Image

from .config import Settings
from .models_client import VisionModelClient, id_judge
from .ringbuffer import RingBuffer, VideoFrame
from .storage import Storage

log = logging.getLogger(__name__)

# id_fn(frames, refs) -> (cat_id, confidence, is_drinking)；cat_id 为空表示模型无法判断
IdFn = Callable[[list[bytes], dict[str, list[bytes]]], Awaitable[tuple[str, float, bool]]]

MAX_FRAMES = 3
MAX_ATTEMPTS = 3
BACKOFF_S = 5.0


def sharpness(jpeg: bytes) -> float:
    img = Image.open(io.BytesIO(jpeg)).convert("L").resize((64, 64))
    a = np.asarray(img, dtype=np.float32)
    gx, gy = np.gradient(a)
    return float(np.mean(np.hypot(gx, gy)))


def pick_clearest(frames: list[VideoFrame], n: int = MAX_FRAMES) -> list[bytes]:
    if not frames:
        return []
    scored = sorted(frames, key=lambda f: sharpness(f.jpeg), reverse=True)
    return [f.jpeg for f in scored[:n]]


class Identifier:
    def __init__(
        self,
        settings: Settings,
        storage: Storage,
        ring: RingBuffer,
        id_fn: IdFn | None = None,
    ) -> None:
        self.settings = settings
        self.storage = storage
        self.ring = ring
        if id_fn is not None:
            self._id_fn = id_fn
        else:
            client = VisionModelClient(
                base_url=settings.id_model_base_url,
                api_key=settings.id_model_api_key,
                model=settings.id_model_name,
            )
            self._id_fn = lambda frames, refs: id_judge(client, frames, refs)
        self.backoff_s = BACKOFF_S

    async def identify(self, event_id: int, start_ts: float, end_ts: float) -> None:
        event = self.storage.get_event(event_id)
        if event is None:
            return
        frames = pick_clearest(self.ring.frames_between(start_ts, end_ts))
        refs: dict[str, list[bytes]] = {}
        for cid, paths in self.storage.cat_reference_images(self.settings.cats_dir).items():
            refs[cid] = [p.read_bytes() for p in paths]

        for attempt in range(1, MAX_ATTEMPTS + 1):
            try:
                cat_id, confidence, is_drinking = await self._id_fn(frames, refs)
                break
            except Exception as e:
                log.warning(
                    "identify event %d attempt %d/%d failed: %s",
                    event_id, attempt, MAX_ATTEMPTS, e,
                )
                if attempt == MAX_ATTEMPTS:
                    log.error("event %d stays confirming: id model unreachable", event_id)
                    return
                await asyncio.sleep(self.backoff_s * attempt)
        else:
            return

        if not is_drinking:
            self.storage.update_identity(event_id, None, confidence, "discarded")
            log.info("event %d discarded: id model says not drinking (conf=%.2f)",
                     event_id, confidence)
        elif cat_id and confidence >= self.settings.identity_confidence_min:
            self.storage.update_identity(event_id, cat_id, confidence, "confirmed")
            log.info("event %d confirmed cat=%s conf=%.2f", event_id, cat_id, confidence)
        else:
            self.storage.update_identity(event_id, cat_id or None, confidence, "unknown")
            log.info("event %d identity unknown (cat_id=%r conf=%.2f)", event_id, cat_id, confidence)

        if self.on_identified:
            self.on_identified(event_id)

    # 由 main 注入：身份确认出终态后广播（cat_confirmed / cat_unknown / event_discarded）
    on_identified: Callable[[int], None] | None = None
