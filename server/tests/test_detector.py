import asyncio
import time

from app.detector import DrinkingDetector
from app.ringbuffer import RingBuffer

from conftest import flat_jpeg, random_jpeg


def make_detector(settings, gate_script, starts, ends):
    """gate_script: 依次返回的 (is_drinking, conf) 列表，用完后返回最后一个。"""
    calls = {"n": 0}
    script = list(gate_script)

    async def gate_fn(jpeg: bytes):
        calls["n"] += 1
        r = script.pop(0) if script else (False, 0.0)
        return r

    det = DrinkingDetector(
        settings,
        RingBuffer(40),
        gate_fn,
        on_event_start=lambda ts: starts.append(ts) or len(starts),
        on_event_end=lambda eid, ts: ends.append((eid, ts)),
    )
    return det, calls


def feed(det, frames):
    async def run():
        ts = time.time()
        for i, jpeg in enumerate(frames):
            await det.process_frame(jpeg, ts + i * 0.1)

    asyncio.run(run())


def test_trigger_after_consecutive_positives_and_release(settings):
    starts, ends = [], []
    det, calls = make_detector(
        settings, [(True, 0.9)] * 3 + [(False, 0.1)] * 4, starts, ends
    )
    # 首帧建立基线，不产生运动
    feed(det, [flat_jpeg()] + [random_jpeg(i) for i in range(7)])
    assert starts and ends
    assert ends[0][0] == 1  # event_id 由 on_event_start 返回值决定
    assert calls["n"] == 7
    # 触发时刻 = 首次肯定帧的时间戳
    assert abs(ends[0][1] - starts[0]) > 0.3


def test_no_false_positive_on_alternating_votes(settings):
    starts, ends = [], []
    det, calls = make_detector(
        settings, [(True, 0.9), (False, 0.1)] * 6, starts, ends
    )
    feed(det, [flat_jpeg()] + [random_jpeg(i) for i in range(12)])
    assert not starts and not ends
    assert not det.in_event


def test_no_motion_no_gate_call(settings):
    starts, ends = [], []
    det, calls = make_detector(settings, [(True, 0.9)] * 10, starts, ends)
    same = flat_jpeg()
    feed(det, [same] * 10)  # 帧差恒为 0，低于阈值
    assert calls["n"] == 0
    assert not starts


def test_in_event_positive_resets_release_counter(settings):
    starts, ends = [], []
    # 3 次肯定触发；随后 3 次否定（不足 4）+ 1 次肯定 + 再 4 次否定才结束
    script = [(True, 0.9)] * 3 + [(False, 0.1)] * 3 + [(True, 0.8)] + [(False, 0.1)] * 4
    det, _ = make_detector(settings, script, starts, ends)
    feed(det, [flat_jpeg()] + [random_jpeg(i) for i in range(11)])
    assert len(starts) == 1 and len(ends) == 1
