#!/usr/bin/env python3
"""用真实摄像头音视频验证录像链路：拉 6 秒真实数据 → make_clip → ffprobe 验证 MP4 音画双轨。

用法：cd server && .venv/bin/python tools/record_smoke.py
"""
from __future__ import annotations

import asyncio
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import httpx  # noqa: E402
import websockets  # noqa: E402

from app.config import load_settings  # noqa: E402
from app.ingest import resolve_local_host  # noqa: E402
from app.recorder import Recorder  # noqa: E402
from app.ringbuffer import AudioChunk, RingBuffer, VideoFrame  # noqa: E402
from app.storage import Storage  # noqa: E402

DURATION_S = 6.0


async def main() -> int:
    settings = load_settings()
    ring = RingBuffer(30)
    base = settings.cam_base_url.rstrip("/")
    stream_url = resolve_local_host(f"{base}/stream")
    audio_url = resolve_local_host(settings.audio_ws_url())
    print(f"视频: {stream_url}\n音频: {audio_url}")

    async def pull_video():
        timeout = httpx.Timeout(None, read=10.0, connect=5.0)
        async with httpx.AsyncClient(timeout=timeout, trust_env=False) as c:
            async with c.stream("GET", stream_url) as r:
                buf = b""
                async for chunk in r.aiter_bytes(65536):
                    buf += chunk
                    while True:
                        soi = buf.find(b"\xff\xd8")
                        if soi < 0:
                            buf = buf[-2:]
                            break
                        eoi = buf.find(b"\xff\xd9", soi + 2)
                        if eoi < 0:
                            buf = buf[soi:]
                            break
                        now = time.time()
                        ring.add_frame(VideoFrame(ts=now, device_ts=int(now * 1e6), jpeg=buf[soi : eoi + 2]))
                        buf = buf[eoi + 2 :]

    async def pull_audio():
        async with websockets.connect(audio_url, max_size=None, compression=None, proxy=None) as ws:
            async for msg in ws:
                if isinstance(msg, bytes) and len(msg) >= 8:
                    (dts,) = struct.unpack("<Q", msg[:8])
                    ring.add_audio(AudioChunk(ts=dts / 1e6, pcm=msg[8:]))

    print(f"采集 {DURATION_S} 秒真实音视频…（请对着摄像头说两句话/敲敲碗）")
    t0 = time.time()
    video_task = asyncio.create_task(pull_video())
    audio_task = asyncio.create_task(pull_audio())
    await asyncio.sleep(DURATION_S)
    video_task.cancel()
    audio_task.cancel()
    for t in (video_task, audio_task):
        try:
            await t
        except asyncio.CancelledError:
            pass

    n_v = len(ring.frames_between(t0, time.time()))
    n_a = len(ring.audio_between(t0, time.time()))
    print(f"采集到: 视频 {n_v} 帧, 音频 {n_a} 块")
    if n_v < 10 or n_a < 10:
        print("FAIL: 采集不足")
        return 1

    storage = Storage(settings.db_path)
    event_id = storage.create_event(t0)
    storage.finish_event(event_id, time.time())
    rec = Recorder(settings, ring, storage)
    clip = await rec.make_clip(event_id, t0, time.time())
    print("clip:", clip)
    if not clip or not Path(clip).exists():
        print("FAIL: 没有生成片段")
        return 1

    # ffprobe 验证双轨
    import subprocess

    out = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "stream=codec_type,codec_name", "-of", "csv=p=0", clip],
        capture_output=True, text=True,
    )
    print("ffprobe:", out.stdout.strip().replace("\n", " | "))
    has_video = "video" in out.stdout
    has_audio = "audio" in out.stdout
    print(f"视频轨 {'✓' if has_video else '✗'}  音频轨 {'✓' if has_audio else '✗'}")
    return 0 if (has_video and has_audio) else 2


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
