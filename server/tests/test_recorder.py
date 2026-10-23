import json
import subprocess
import time
from pathlib import Path

import pytest

from app.recorder import Recorder
from app.ringbuffer import AudioChunk, RingBuffer, VideoFrame
from app.storage import Storage, local_date_str

from conftest import random_jpeg


@pytest.mark.parametrize("with_audio", [True, False])
def test_recorder_make_clip(settings, with_audio):
    ring = RingBuffer(seconds=60)
    storage = Storage(settings.db_path)
    storage.scan_cats(settings.cats_dir)
    recorder = Recorder(settings, ring, storage)

    base = time.time()
    for i in range(12):  # ~1.2s @ 10fps
        ring.add_frame(VideoFrame(ts=base + i * 0.1, device_ts=None, jpeg=random_jpeg(i)))
    if with_audio:
        for i in range(10):  # 1s PCM
            ring.add_audio(AudioChunk(ts=base + i * 0.1, pcm=b"\x01\x00" * 1600))

    event_id = storage.create_event(base + 0.2)
    storage.finish_event(event_id, base + 0.9)

    import asyncio

    rel = asyncio.run(recorder.make_clip(event_id, base + 0.2, base + 0.9))
    assert rel is not None
    clip = Path(settings.data_dir.parent) / rel
    assert clip.exists() and rel.startswith(f"data/clips/{local_date_str()}/")
    assert storage.get_event(event_id)["clip_path"] == rel

    # ffprobe 验证流与时长
    out = subprocess.run(
        ["ffprobe", "-v", "quiet", "-print_format", "json", "-show_streams", str(clip)],
        check=True, capture_output=True, text=True,
    ).stdout
    streams = {s["codec_type"]: s for s in json.loads(out)["streams"]}
    assert streams["video"]["codec_name"] == "h264"
    if with_audio:
        assert streams["audio"]["codec_name"] == "aac"
    else:
        assert "audio" not in streams


def test_recorder_no_frames_returns_none(settings):
    ring = RingBuffer(seconds=60)
    storage = Storage(settings.db_path)
    storage.scan_cats(settings.cats_dir)
    recorder = Recorder(settings, ring, storage)
    event_id = storage.create_event(time.time())

    import asyncio

    assert asyncio.run(recorder.make_clip(event_id, time.time(), time.time())) is None
    assert storage.get_event(event_id)["clip_path"] is None


def test_recorder_long_event_audio_aligned_to_video_start(settings):
    """回归：事件长于环形缓冲时，音频必须对齐视频实际起点而非事件窗口起点。

    曾踩过：80s 事件的录像视频只覆盖末尾 40s，音频从事件起点补静音，
    -shortest 裁剪后整段无声（48/50 号片段）。
    """
    import asyncio

    ring = RingBuffer(seconds=40)
    storage = Storage(settings.db_path)
    storage.scan_cats(settings.cats_dir)
    recorder = Recorder(settings, ring, storage)

    base = time.time() - 60  # 60s 前开始的数据，模拟长事件（ring 只留末尾 40s）
    for i in range(600):  # 60s @10fps，ring 只留末尾 40s
        ts = base + i * 0.1
        ring.add_frame(VideoFrame(ts=ts, device_ts=None, jpeg=random_jpeg(i)))
        ring.add_audio(AudioChunk(ts=ts, pcm=b"\x10\x00" * 1600))  # 非零 PCM（100ms/块）

    event_id = storage.create_event(base)
    storage.finish_event(event_id, base + 55)
    rel = asyncio.run(recorder.make_clip(event_id, base, base + 55))
    assert rel is not None
    clip = Path(settings.data_dir.parent) / rel

    # 解出 PCM 验证：有音轨且不全是静音
    pcm_path = clip.parent / f"{event_id}.pcm"
    subprocess.run(
        ["ffmpeg", "-y", "-v", "quiet", "-i", str(clip), "-f", "s16le", "-ac", "1",
         "-ar", "16000", str(pcm_path)],
        check=True,
    )
    pcm = pcm_path.read_bytes()
    assert pcm, "no audio decoded"
    assert any(b != 0 for b in pcm[:16000]), "clip 开头一秒全静音——音频没对齐到视频起点"
    pcm_path.unlink()
