"""UDP 直播推送测试。"""
import asyncio
import socket
import struct
import time

import pytest

from app.ringbuffer import RingBuffer, VideoFrame
from app.udp_live import HDR_FMT, HDR_LEN, UdpLivePush
from PIL import Image
import io


def _make_jpeg(color: int) -> bytes:
    img = Image.new("RGB", (64, 48), (color, 20, 200))
    buf = io.BytesIO()
    img.save(buf, format="JPEG")
    return buf.getvalue()


@pytest.mark.asyncio
async def test_udp_push_reassembles_frame():
    ring = RingBuffer(10)
    push = UdpLivePush(ring)

    # 本地 UDP 接收器
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind(("127.0.0.1", 0))
    port = rx.getsockname()[1]
    rx.setblocking(False)

    await push.start()
    push.register("127.0.0.1", port, ttl_s=30)
    try:
        ring.add_frame(VideoFrame(ts=time.time(), device_ts=0, jpeg=_make_jpeg(255)))
        chunks: dict[int, bytes] = {}
        count = -1
        deadline = time.time() + 5
        loop = asyncio.get_running_loop()
        while time.time() < deadline and (count < 0 or len(chunks) < count):
            await asyncio.sleep(0.05)
            while True:
                try:
                    data, _ = rx.recvfrom(65535)
                except BlockingIOError:
                    break
                seq, idx, total = struct.unpack(HDR_FMT, data[:HDR_LEN])
                count = total
                chunks[idx] = data[HDR_LEN:]
        assert count > 0, "没收到任何 UDP 分片"
        assert len(chunks) == count, f"分片不齐: {len(chunks)}/{count}"
        payload = b"".join(chunks[i] for i in range(count))
        assert payload[:2] == b"\xff\xd8" and payload[-2:] == b"\xff\xd9"
        img = Image.open(io.BytesIO(payload))
        assert img.size == (480, 240)  # 直播带尺寸
    finally:
        await push.stop()
        rx.close()


@pytest.mark.asyncio
async def test_udp_push_no_targets_silence():
    """无注册目标时不应发送也不应报错。"""
    ring = RingBuffer(10)
    push = UdpLivePush(ring)
    await push.start()
    ring.add_frame(VideoFrame(ts=time.time(), device_ts=0, jpeg=_make_jpeg(1)))
    await asyncio.sleep(0.2)
    await push.stop()
