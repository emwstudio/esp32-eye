"""api：api-contract.md 的全部 REST 端点与两个 WebSocket。"""
from __future__ import annotations

import asyncio
import io
import logging
import struct
from pathlib import Path
from typing import TYPE_CHECKING, AsyncIterator

from fastapi import APIRouter, HTTPException, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import Response, StreamingResponse
from PIL import Image, ImageOps
from pydantic import BaseModel

from .mock_cam import AUDIO_RATE
from .config import resolve_binary

if TYPE_CHECKING:
    from .main import Services

log = logging.getLogger(__name__)

router = APIRouter()

LIVE_SIZE = 480
LIVE_FPS = 10
CLIP_SIZE = 320
CLIP_FPS = 10
CLIP_AUDIO_CHUNK_S = 0.2


def _svc(request: Request) -> "Services":
    return request.app.state.services


# ---- REST ----

@router.post("/api/debug/trigger")
async def debug_trigger(request: Request, cat_id: str | None = None) -> dict:
    """调试端点：模拟一次喝水事件消息流（不录像不调模型）。"""
    event_id = await _svc(request).debug_trigger_event(cat_id)
    return {"event_id": event_id, "cat_id": cat_id}


@router.post("/api/debug/log")
async def debug_log(request: Request) -> dict:
    """桌面屏远程日志接收（text/plain，每行一条）。"""
    body = await request.body()
    for line in body.decode("utf-8", "replace").splitlines():
        if line.strip():
            log.info("[display] %s", line)
    return {"ok": True}


@router.get("/api/cats")
async def list_cats(request: Request) -> list[dict]:
    cats = _svc(request).storage.list_cats()
    return [{"id": c["id"], "name": c["name"], "avatar_url": c["avatar_url"]} for c in cats]


@router.get("/api/cats/{cat_id}/avatar")
async def cat_avatar(request: Request, cat_id: str) -> Response:
    path = _svc(request).storage.cat_avatar_path(cat_id)
    if not path or not Path(path).exists():
        raise HTTPException(status_code=404, detail="avatar not found")
    img = ImageOps.exif_transpose(Image.open(path)).convert("RGB")  # iPhone 照片带 EXIF 方向，必须转正再裁
    w, h = img.size
    side = min(w, h)
    img = img.crop(((w - side) // 2, (h - side) // 2, (w + side) // 2, (h + side) // 2)).resize((192, 192))
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=85)
    return Response(content=buf.getvalue(), media_type="image/jpeg")


@router.get("/api/today")
async def today(request: Request) -> dict:
    return _svc(request).storage.today_summary()


@router.get("/api/clips")
async def clips(request: Request, cat_id: str | None = None, date: str | None = None) -> list[dict]:
    return _svc(request).storage.list_clips(cat_id=cat_id, date=date)


class CorrectBody(BaseModel):
    cat_id: str


@router.post("/api/events/{event_id}/correct")
async def correct_event(request: Request, event_id: int, body: CorrectBody) -> dict:
    services = _svc(request)
    event = services.storage.correct_event(event_id, body.cat_id)
    if event is None:
        raise HTTPException(status_code=404, detail="event or cat not found")
    log.info("event %d corrected to cat=%s", event_id, body.cat_id)
    # 人工纠正与自动确认同等语义：广播 cat_confirmed（cat_id 必非空）+ count_updated
    await services.bus.broadcast(
        {
            "type": "cat_confirmed",
            "event_id": event_id,
            "cat_id": event["cat_id"],
            "confidence": event["confidence"],
        }
    )
    await services.broadcast_count_updated()
    return event


@router.get("/api/health")
async def health(request: Request) -> dict:
    services = _svc(request)
    s = services.settings
    return {
        "ok": True,
        "cam_connected": services.ingest.cam_connected,
        "audio_connected": services.ingest.audio_connected,
        "gate_model": s.gate_model_name,
        "id_model": s.id_model_name,
    }


def _resize_jpeg(jpeg: bytes, size: int, quality: int = 70) -> bytes | None:
    try:
        img = Image.open(io.BytesIO(jpeg)).convert("RGB").resize((size, size))
    except OSError:
        return None  # 半截帧容错
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=quality)
    return buf.getvalue()


@router.get("/api/live")
async def live_video(request: Request) -> StreamingResponse:
    """MJPEG 流：环形缓冲最新帧缩放到 480×480，~10fps。"""
    services = _svc(request)

    async def gen() -> AsyncIterator[bytes]:
        last_ts = 0.0
        while True:
            frame = services.ring.latest_frame()
            if frame and frame.ts > last_ts:
                last_ts = frame.ts
                jpeg = await asyncio.to_thread(_resize_jpeg, frame.jpeg, LIVE_SIZE)
                if jpeg is None:
                    await asyncio.sleep(0.02)
                    continue
                yield b"--frame\r\nContent-Type: image/jpeg\r\n\r\n" + jpeg + b"\r\n"
            await asyncio.sleep(1.0 / LIVE_FPS)

    return StreamingResponse(gen(), media_type="multipart/x-mixed-replace; boundary=frame")


# 直播带编码与缓存在 udp_live 模块（MJPEG 流与 UDP 推送共用）
from .udp_live import get_band_jpeg as _get_band_jpeg
from .udp_live import band_jpeg as _band_jpeg  # noqa: F401  # 兼容既有引用


@router.get("/api/live_band")
async def live_video_band(request: Request, h: int = 240) -> StreamingResponse:
    """MJPEG 流（宽屏带）：缩放后中心裁 480×h（h 默认 240，桌面屏用 200）。"""
    services = _svc(request)

    async def gen() -> AsyncIterator[bytes]:
        last_sent_ts = 0.0
        while True:
            # 发送所有新帧（latest-only 会在 TCP 大读到齐时合并丢帧，实测腰斩帧率）
            for frame in services.ring.frames_since(last_sent_ts):
                jpeg = await _get_band_jpeg(frame.jpeg, frame.ts, h)
                if jpeg is None:
                    continue
                last_sent_ts = frame.ts
                yield b"--frame\r\nContent-Type: image/jpeg\r\n\r\n" + jpeg + b"\r\n"
            await asyncio.sleep(0.005)

    return StreamingResponse(gen(), media_type="multipart/x-mixed-replace; boundary=frame")


@router.get("/api/demo_band")
async def demo_video_band(request: Request, frames: int | None = None) -> StreamingResponse:
    video = _svc(request).settings.data_dir / "demo_loop.mp4"
    if not video.is_file():
        raise HTTPException(status_code=404, detail="demo video not found")

    async def gen() -> AsyncIterator[bytes]:
        proc = await asyncio.create_subprocess_exec(
            resolve_binary("ffmpeg"), "-loglevel", "error",
            "-stream_loop", "-1", "-re", "-i", str(video),
            "-vf", "fps=12,scale=480:360,crop=480:200",
            "-an", "-f", "image2pipe", "-vcodec", "mjpeg", "-q:v", "6", "pipe:1",
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
        )
        try:
            sent = 0
            while frames is None or sent < frames:
                frame = await proc.stdout.readuntil(b"\xff\xd9")
                yield b"--frame\r\nContent-Type: image/jpeg\r\n\r\n" + frame + b"\r\n"
                sent += 1
        except (asyncio.IncompleteReadError, asyncio.LimitOverrunError):
            pass
        finally:
            if proc.returncode is None:
                proc.terminate()
                try:
                    await asyncio.wait_for(proc.wait(), timeout=2)
                except TimeoutError:
                    proc.kill()
                    await proc.wait()

    return StreamingResponse(gen(), media_type="multipart/x-mixed-replace; boundary=frame")


@router.get("/api/demo_audio")
async def demo_audio(request: Request, chunks: int | None = None) -> StreamingResponse:
    video = _svc(request).settings.data_dir / "demo_loop.mp4"
    if not video.is_file():
        raise HTTPException(status_code=404, detail="demo video not found")

    async def gen() -> AsyncIterator[bytes]:
        proc = await asyncio.create_subprocess_exec(
            resolve_binary("ffmpeg"), "-loglevel", "error",
            "-stream_loop", "-1", "-re", "-i", str(video),
            "-vn", "-ac", "1", "-ar", "16000", "-f", "s16le", "pipe:1",
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
        )
        try:
            sent = 0
            while chunks is None or sent < chunks:
                try:
                    pcm = await proc.stdout.readexactly(640)
                except asyncio.IncompleteReadError:
                    break
                yield pcm
                sent += 1
        finally:
            if proc.returncode is None:
                proc.terminate()
                try:
                    await asyncio.wait_for(proc.wait(), timeout=2)
                except TimeoutError:
                    proc.kill()
                    await proc.wait()

    return StreamingResponse(gen(), media_type="application/octet-stream")


@router.get("/api/live_udp")
async def live_udp_register(request: Request, port: int = 5060, h: int = 200) -> dict:
    """UDP 直播注册：桌面屏调用后，服务端把直播带帧以 UDP 推送到 <调用者IP>:port。

    TCP 到 ESP32 的投递受 lwIP 窗口/延迟 ACK 限制在 ~5fps；UDP 无此语义。
    注册有效期 60 秒，客户端需周期续期。帧分片格式见 UdpLivePush。
    h 为直播带高度（桌面屏标题栏布局用 200）。
    """
    svc = _svc(request)
    host = request.client.host if request.client else None
    if not host:
        raise HTTPException(status_code=400, detail="no client host")
    svc.udp_live.register(host, port, band_h=h)
    return {"ok": True, "target": f"{host}:{port}", "ttl_s": 60}


@router.get("/api/live_audio")
async def live_audio(request: Request) -> StreamingResponse:
    """裸 PCM 流：16kHz 16bit 单声道小端，原样转发。"""
    services = _svc(request)

    async def gen() -> AsyncIterator[bytes]:
        q = services.ring.subscribe_audio()
        try:
            while True:
                yield await q.get()
        finally:
            services.ring.unsubscribe_audio(q)

    return StreamingResponse(gen(), media_type="application/octet-stream")


# ---- /ws/events ----

@router.websocket("/ws/events")
async def ws_events(ws: WebSocket) -> None:
    services: "Services" = ws.app.state.services
    await ws.accept()
    services.bus.connect(ws)

    async def _heartbeat() -> None:
        # 30s 应用层心跳：桌面屏靠它识别"连接假死"（TCP 活着但消息不通）
        # 并重连；服务端这边发送失败即关连接，促使 receive 退出。
        while True:
            await asyncio.sleep(30)
            try:
                await ws.send_json({"type": "hb"})
            except Exception:
                try:
                    await ws.close()
                except Exception:
                    pass
                return

    hb = asyncio.create_task(_heartbeat())
    try:
        summary = services.storage.today_summary()
        await ws.send_json(
            {"type": "hello", "counts": summary["counts"], "date": summary["date"]}
        )
        while True:
            await ws.receive_text()  # 保持连接；客户端无需上行
    except WebSocketDisconnect:
        pass
    finally:
        hb.cancel()
        services.bus.disconnect(ws)


# ---- /ws/clip/{event_id} ----

# 二进制帧格式：[1B type][4B uint32 LE 时间戳 ms][4B uint32 LE 长度][payload]
def pack_frame(frame_type: int, ts_ms: int, payload: bytes) -> bytes:
    return struct.pack("<BII", frame_type, ts_ms, len(payload)) + payload


async def _iter_mjpeg(pipe: asyncio.StreamReader) -> AsyncIterator[bytes]:
    """从 ffmpeg image2pipe(mjpeg) 输出按 SOI/EOI 切帧。"""
    buf = b""
    while chunk := await pipe.read(65536):
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
            yield buf[soi : eoi + 2]
            buf = buf[eoi + 2 :]


async def _iter_pcm(pipe: asyncio.StreamReader, chunk_bytes: int) -> AsyncIterator[bytes]:
    while chunk := await pipe.read(chunk_bytes):
        yield chunk


class _IndexedFrames:
    """给流式 payload 配上按序号递增的相对时间戳（ms）。"""

    def __init__(self, it: AsyncIterator[bytes], step_ms: int) -> None:
        self._it = it
        self._step_ms = step_ms
        self._idx: int | None = 0

    async def next(self) -> tuple[int, bytes] | None:
        if self._idx is None:
            return None
        payload = await anext(self._it, None)
        if payload is None:
            self._idx = None
            return None
        ts = self._idx * self._step_ms
        self._idx += 1
        return ts, payload


FFMPEG = resolve_binary("ffmpeg")
FFPROBE = resolve_binary("ffprobe")


async def _ffprobe(args: list[str]) -> str:
    proc = await asyncio.create_subprocess_exec(
        FFPROBE, "-v", "quiet", *args,
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL,
    )
    out, _ = await proc.communicate()
    return out.decode().strip()


async def _clip_has_audio(path: Path) -> bool:
    out = await _ffprobe(["-select_streams", "a", "-show_entries", "stream=index",
                          "-of", "csv=p=0", str(path)])
    return bool(out)


async def _clip_duration_s(path: Path) -> float:
    out = await _ffprobe(["-show_entries", "format=duration", "-of", "csv=p=0", str(path)])
    try:
        return float(out)
    except ValueError:
        return 0.0


@router.websocket("/ws/clip/{event_id}")
async def ws_clip(ws: WebSocket, event_id: int) -> None:
    services: "Services" = ws.app.state.services
    await ws.accept()
    event = services.storage.get_event(event_id)
    clip = event["clip_path"] if event else None
    clip_path = (services.settings.data_dir.parent / clip) if clip else None
    if clip_path is None or not Path(clip_path).exists():
        await ws.send_json({"type": "clip_end"})
        await ws.close()
        return

    video_proc = await asyncio.create_subprocess_exec(
        FFMPEG, "-nostdin", "-i", str(clip_path),
        "-vf", f"scale={CLIP_SIZE}:{CLIP_SIZE},fps={CLIP_FPS}",
        "-f", "image2pipe", "-vcodec", "mjpeg", "-",
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL,
    )
    if await _clip_has_audio(clip_path):
        audio_cmd = [
            FFMPEG, "-nostdin", "-i", str(clip_path), "-vn",
            "-f", "s16le", "-acodec", "pcm_s16le", "-ar", str(AUDIO_RATE), "-ac", "1", "-",
        ]
    else:
        # 无声片段兜底：anullsrc 生成与视频等长的静音 PCM，
        # 保证 display 端"音频为时钟"假设恒成立
        duration = await _clip_duration_s(clip_path)
        audio_cmd = [
            FFMPEG, "-nostdin", "-f", "lavfi", "-t", f"{duration:.3f}",
            "-i", f"anullsrc=r={AUDIO_RATE}:cl=mono",
            "-f", "s16le", "-acodec", "pcm_s16le", "-",
        ]
    audio_proc = await asyncio.create_subprocess_exec(
        *audio_cmd,
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL,
    )
    assert video_proc.stdout is not None and audio_proc.stdout is not None

    chunk_bytes = int(AUDIO_RATE * CLIP_AUDIO_CHUNK_S) * 2
    video_src = _IndexedFrames(_iter_mjpeg(video_proc.stdout), 1000 // CLIP_FPS)
    audio_src = _IndexedFrames(
        _iter_pcm(audio_proc.stdout, chunk_bytes), int(CLIP_AUDIO_CHUNK_S * 1000)
    )

    try:
        pending_v = await video_src.next()
        pending_a = await audio_src.next()
        while pending_v is not None or pending_a is not None:
            if pending_a is None or (pending_v is not None and pending_v[0] <= pending_a[0]):
                await ws.send_bytes(pack_frame(0x01, pending_v[0], pending_v[1]))
                pending_v = await video_src.next()
            else:
                await ws.send_bytes(pack_frame(0x02, pending_a[0], pending_a[1]))
                pending_a = await audio_src.next()
    finally:
        for p in (video_proc, audio_proc):
            if p.returncode is None:
                p.kill()
            await p.wait()
        await asyncio.sleep(0)  # 让管道 EOF 回调在本事件循环内完成，避免 GC 警告
    await ws.send_json({"type": "clip_end"})
