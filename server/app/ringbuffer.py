"""带时间戳的环形缓冲：保存最近 N 秒的视频帧与音频块。"""
from __future__ import annotations

import asyncio
import time
from collections import deque
from dataclasses import dataclass


@dataclass
class VideoFrame:
    ts: float            # 接收时刻（Unix 秒）
    device_ts: int | None  # 设备时间戳（微秒），mock 源可为 None
    jpeg: bytes


@dataclass
class AudioChunk:
    ts: float
    pcm: bytes  # 16kHz 16bit 单声道小端


class RingBuffer:
    def __init__(self, seconds: float) -> None:
        self.seconds = seconds
        self._frames: deque[VideoFrame] = deque()
        self._audio: deque[AudioChunk] = deque()
        self._audio_subs: set[asyncio.Queue[bytes]] = set()

    def _prune(self, now: float) -> None:
        cutoff = now - self.seconds
        while self._frames and self._frames[0].ts < cutoff:
            self._frames.popleft()
        while self._audio and self._audio[0].ts < cutoff:
            self._audio.popleft()

    def add_frame(self, frame: VideoFrame) -> None:
        self._frames.append(frame)
        self._prune(frame.ts)

    def add_audio(self, chunk: AudioChunk) -> None:
        self._audio.append(chunk)
        self._prune(chunk.ts)
        for q in list(self._audio_subs):
            try:
                q.put_nowait(chunk.pcm)
            except asyncio.QueueFull:
                pass

    def latest_frame(self) -> VideoFrame | None:
        return self._frames[-1] if self._frames else None

    def frames_between(self, start: float, end: float) -> list[VideoFrame]:
        return [f for f in self._frames if start <= f.ts <= end]

    def frames_since(self, ts: float) -> list[VideoFrame]:
        return [f for f in self._frames if f.ts > ts]

    def audio_between(self, start: float, end: float) -> list[AudioChunk]:
        return [c for c in self._audio if start <= c.ts <= end]

    def subscribe_audio(self, maxsize: int = 200) -> asyncio.Queue[bytes]:
        q: asyncio.Queue[bytes] = asyncio.Queue(maxsize=maxsize)
        self._audio_subs.add(q)
        return q

    def unsubscribe_audio(self, q: asyncio.Queue[bytes]) -> None:
        self._audio_subs.discard(q)

    def __len__(self) -> int:
        return len(self._frames)


def now() -> float:
    return time.time()
