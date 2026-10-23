import asyncio
import time

from app.detector import DrinkingDetector
from app.ringbuffer import RingBuffer

from conftest import flat_jpeg, blob_jpeg


def make_detector(settings, gate_script, starts, ends):
    """gate_script: 依次返回的 (is_drinking, conf) 列表，用完后返回最后一个。
    注意：前景占比超过前置阈值（blob 帧）才会调用 gate_fn；flat 帧（空场景）直接否定票。"""
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
    """测试 settings 里 gate_interval_s=0：每帧一票。"""

    async def run():
        ts = time.time()
        for i, jpeg in enumerate(frames):
            await det.process_frame(jpeg, ts + i * 0.1)

    asyncio.run(run())


def test_trigger_after_consecutive_positives_and_release(settings):
    starts, ends = [], []
    det, calls = make_detector(settings, [(True, 0.9)] * 3, starts, ends)
    # 基线（空场景）→ 3 帧有猫且 1B 判喝水 → 触发；4 帧空场景 → 释放
    feed(det, [flat_jpeg()] + [blob_jpeg(i) for i in range(3)] + [flat_jpeg()] * 4)
    assert starts and ends
    assert ends[0][0] == 1  # event_id 由 on_event_start 返回值决定
    assert calls["n"] == 3  # 只有 blob 帧（前景占比高）才调用模型
    # 触发时刻 = 首次肯定帧的时间戳
    assert abs(ends[0][1] - starts[0]) > 0.3


def test_no_false_positive_on_alternating_votes(settings):
    starts, ends = [], []
    det, calls = make_detector(settings, [(True, 0.9), (False, 0.1)] * 3, starts, ends)
    feed(det, [flat_jpeg()] + [blob_jpeg(i) for i in range(6)])
    assert not starts and not ends
    assert not det.in_event
    assert calls["n"] == 6


def test_empty_scene_no_gate_call(settings):
    starts, ends = [], []
    det, calls = make_detector(settings, [(True, 0.9)] * 10, starts, ends)
    same = flat_jpeg()
    feed(det, [same] * 10)  # 场景恒为空：前置过滤拦下，一票否定，不调用模型
    assert calls["n"] == 0
    assert not starts


def test_in_event_positive_resets_release_counter(settings):
    starts, ends = [], []
    # 3 次肯定触发；随后 3 次否定（不足 4）+ 1 次肯定 + 再 4 帧空场景才结束
    script = [(True, 0.9)] * 3 + [(False, 0.1)] * 3 + [(True, 0.8)]
    det, calls = make_detector(settings, script, starts, ends)
    feed(
        det,
        [flat_jpeg()]
        + [blob_jpeg(i) for i in range(7)]  # 7 次模型调用（上面的脚本）
        + [flat_jpeg()] * 4,                # 空场景否定票收尾
    )
    assert len(starts) == 1 and len(ends) == 1
    assert calls["n"] == 7
