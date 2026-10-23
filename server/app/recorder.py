"""recorder：事件结束后从环形缓冲截取窗口，ffmpeg 合成 H.264+AAC MP4。"""
from __future__ import annotations

import asyncio
import logging
import statistics
import tempfile
from pathlib import Path

from .config import Settings, resolve_binary
from .mock_cam import AUDIO_RATE
from .ringbuffer import RingBuffer
from .storage import Storage, local_date_str

log = logging.getLogger(__name__)

# launchd 常驻环境 PATH 极简，找不到 Homebrew 的 ffmpeg——启动时解析一次
FFMPEG = resolve_binary("ffmpeg")


async def _run_ffmpeg(args: list[str]) -> None:
    proc = await asyncio.create_subprocess_exec(
        FFMPEG, "-nostdin", *args,
        stdout=asyncio.subprocess.DEVNULL,
        stderr=asyncio.subprocess.PIPE,
    )
    try:
        _, stderr = await proc.communicate()
    except asyncio.CancelledError:
        proc.kill()
        await proc.wait()
        raise
    if proc.returncode != 0:
        raise RuntimeError(f"ffmpeg failed ({proc.returncode}): {stderr.decode()[-500:]}")


class Recorder:
    def __init__(self, settings: Settings, ring: RingBuffer, storage: Storage) -> None:
        self.settings = settings
        self.ring = ring
        self.storage = storage

    async def make_clip(self, event_id: int, start_ts: float, end_ts: float) -> str | None:
        """截取 [start-CLIP_PRE_S, end+CLIP_POST_S]，返回相对 data_dir 的 clip_path。"""
        s = self.settings
        win_start = start_ts - s.clip_pre_s
        win_end = end_ts + s.clip_post_s
        frames = self.ring.frames_between(win_start, win_end)
        if not frames:
            log.warning("event %d: no frames in ring buffer, clip skipped", event_id)
            return None
        audio = self.ring.audio_between(win_start, win_end)
        if not audio:
            log.warning("event %d: NO audio chunks in clip window — 片段将无声（查音频 WS 链路）", event_id)

        day = local_date_str(start_ts)
        out_dir = s.clips_dir / day
        out_dir.mkdir(parents=True, exist_ok=True)
        out_path = out_dir / f"{event_id}.mp4"

        # 帧率按实际帧间隔中位数（同时用 concat duration 做精确对齐）
        intervals = [b.ts - a.ts for a, b in zip(frames, frames[1:]) if b.ts > a.ts]
        median_interval = statistics.median(intervals) if intervals else 1.0 / 12.0

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            concat_lines = ["ffconcat version 1.0"]
            for i, f in enumerate(frames):
                img = tmp_path / f"f{i:06d}.jpg"
                img.write_bytes(f.jpeg)
                if i + 1 < len(frames):
                    dur = max(frames[i + 1].ts - f.ts, 1e-3)
                else:
                    dur = median_interval
                concat_lines.append(f"file '{img}'")
                concat_lines.append(f"duration {dur:.6f}")
            concat_file = tmp_path / "frames.txt"
            concat_file.write_text("\n".join(concat_lines))

            args = ["-y", "-f", "concat", "-safe", "0", "-i", str(concat_file)]

            pcm_path: Path | None = None
            if audio:
                # 按时间戳对齐：块间空隙补静音
                total_s = sum(len(c.pcm) for c in audio) / 2 / AUDIO_RATE
                win_s = win_end - win_start
                # 音频留痕：无声片段排查用（48 号片段全静音事件的教训）
                log.info("event %d clip audio: %d chunks %.1fs in %.1fs window (%.0f%%)",
                         event_id, len(audio), total_s, win_s, total_s / win_s * 100)
                pcm = bytearray()
                # 音频时间轴对齐视频的实际起点 frames[0].ts，不是事件窗口起点：
                # 事件长于环形缓冲（40s）时视频只覆盖末尾一段；若从 win_start 起摆，
                # 前面补的静音会把真实音频推出 -shortest 裁剪点 → 整段无声（48/50 的教训）
                cursor = frames[0].ts
                for chunk in sorted(audio, key=lambda c: c.ts):
                    chunk_end = chunk.ts + len(chunk.pcm) / 2 / AUDIO_RATE
                    if chunk_end <= cursor:
                        continue  # 整段在视频起点之前，用不上
                    start = chunk.ts
                    data = chunk.pcm
                    if start < cursor:
                        # 部分重叠：裁掉视频起点之前的部分
                        cut = int((cursor - start) * AUDIO_RATE) * 2
                        data = data[cut:]
                        start = cursor
                    if start > cursor:
                        gap_samples = int((start - cursor) * AUDIO_RATE)
                        pcm.extend(b"\x00\x00" * gap_samples)
                        cursor = start
                    pcm.extend(data)
                    cursor += len(data) / 2 / AUDIO_RATE
                pcm_path = tmp_path / "audio.pcm"
                pcm_path.write_bytes(bytes(pcm))
                args += ["-f", "s16le", "-ar", str(AUDIO_RATE), "-ac", "1", "-i", str(pcm_path)]

            # CRF 18 + medium：接近无损地保留源 JPEG 帧细节（默认 CRF23 会再糊一档）
            args += ["-c:v", "libx264", "-crf", "18", "-preset", "medium", "-pix_fmt", "yuv420p", "-movflags", "+faststart"]
            if pcm_path is not None:
                # 高通 150Hz：碗沿麦克风拾取的 50Hz 工频哼声（市电/泵）很响，
                # 喝水舔水声在 500Hz+，滤掉低频底噪实测 RMS -27→-33dB
                args += ["-af", "highpass=f=150", "-c:a", "aac", "-shortest"]
            args.append(str(out_path))
            await _run_ffmpeg(args)

        rel = str(out_path.relative_to(s.data_dir.parent))
        self.storage.set_clip_path(event_id, rel)
        log.info("event %d clip saved: %s (%.1fs, %d frames)", event_id, rel,
                 frames[-1].ts - frames[0].ts, len(frames))
        return rel
