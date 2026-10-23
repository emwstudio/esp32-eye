import asyncio
import time

from app.detector import DrinkingDetector
from app.ringbuffer import RingBuffer

from conftest import flat_jpeg, blob_jpeg


def make_detector(settings, starts, ends):
    det = DrinkingDetector(
        settings,
        RingBuffer(40),
        on_event_start=lambda ts: starts.append(ts) or len(starts),
        on_event_end=lambda eid, ts: ends.append((eid, ts)),
    )
    return det


def feed(det, frames):
    """测试 settings 里 gate_interval_s=0：每帧一票。blob=猫在画面（肯定），flat=空场景（否定）。"""

    async def run():
        ts = time.time()
        for i, jpeg in enumerate(frames):
            await det.process_frame(jpeg, ts + i * 0.1)

    asyncio.run(run())


def test_trigger_after_consecutive_positives_and_release(settings):
    starts, ends = [], []
    det = make_detector(settings, starts, ends)
    # 基线（空场景）→ 3 帧有猫触发 → 4 帧空场景释放
    feed(det, [flat_jpeg()] + [blob_jpeg(i) for i in range(3)] + [flat_jpeg()] * 4)
    assert starts and ends
    assert ends[0][0] == 1  # event_id 由 on_event_start 返回值决定
    # 触发时刻 = 首次肯定帧的时间戳
    assert abs(ends[0][1] - starts[0]) > 0.3


def test_no_false_positive_on_alternating_votes(settings):
    starts, ends = [], []
    det = make_detector(settings, starts, ends)
    frames = [flat_jpeg()]
    for i in range(6):
        frames += [blob_jpeg(i), flat_jpeg()]
    feed(det, frames)
    assert not starts and not ends
    assert not det.in_event


def test_empty_scene_no_event(settings):
    starts, ends = [], []
    det = make_detector(settings, starts, ends)
    same = flat_jpeg()
    feed(det, [same] * 10)  # 场景恒为空，占比≈0
    assert not starts
    assert not det.in_event


def test_in_event_positive_resets_release_counter(settings):
    starts, ends = [], []
    # 3 帧有猫触发；随后 3 帧空（不足 4）+ 1 帧有猫 + 再 4 帧空才结束
    det = make_detector(settings, starts, ends)
    frames = (
        [flat_jpeg()]
        + [blob_jpeg(i) for i in range(3)]
        + [flat_jpeg()] * 3
        + [blob_jpeg(9)]
        + [flat_jpeg()] * 4
    )
    feed(det, frames)
    assert len(starts) == 1 and len(ends) == 1
