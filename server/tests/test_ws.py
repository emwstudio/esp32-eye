import struct
import subprocess
import time

from fastapi.testclient import TestClient

from app.main import create_app
from app.storage import local_date_str

from conftest import flat_jpeg, blob_jpeg


def parse_frames(messages):
    """解析 /ws/clip 二进制帧：[1B type][4B ts ms LE][4B len LE][payload]"""
    parsed = []
    for m in messages:
        assert len(m) >= 9
        ftype, ts_ms, length = struct.unpack("<BII", m[:9])
        payload = m[9:]
        assert len(payload) == length
        parsed.append((ftype, ts_ms, payload))
    return parsed


def test_ws_events_hello_and_event_flow(settings):
    async def id_fn(frames, refs):
        return "buding", 0.86, True

    app = create_app(settings, id_fn=id_fn)
    with TestClient(app) as client:
        services = app.state.services
        # 停掉 detector 后台轮询，手动喂帧以保证确定性
        client.portal.call(services.detector.stop)
        services.detector.reset_baseline()  # 清掉 mock cam 帧建立的背景基线，手动喂帧从干净基线开始

        with client.websocket_connect("/ws/events") as ws:
            hello = ws.receive_json()
            assert hello["type"] == "hello"
            assert hello["counts"] == {} and "date" in hello

            ts = time.time()
            frames = [flat_jpeg()] + [blob_jpeg(i) for i in range(3)] + [flat_jpeg()] * 4
            # 前 4 帧（基线 + 3 次肯定）→ drinking_started
            for i, jpeg in enumerate(frames[:4]):
                client.portal.call(services.detector.process_frame, jpeg, ts + i * 0.1)
            started = ws.receive_json()
            assert started["type"] == "drinking_started"
            event_id = started["event_id"]
            assert isinstance(event_id, int) and started["ts"] > 0

            # 后 4 帧否定 → 事件结束 → identifier(mock) → cat_confirmed
            for i, jpeg in enumerate(frames[4:]):
                client.portal.call(
                    services.detector.process_frame, jpeg, ts + (i + 4) * 0.1
                )
            confirmed = ws.receive_json()
            assert confirmed["type"] == "cat_confirmed"
            assert confirmed["event_id"] == event_id
            assert confirmed["cat_id"] == "buding"
            assert confirmed["confidence"] == 0.86

            count_msg = ws.receive_json()
            assert count_msg["type"] == "count_updated"
            # cats 目录为空，buding 不在 cats 表中但 confirmed 事件仍按 cat_id 计数
            assert count_msg["counts"] == {"buding": 1}

        event = services.storage.get_event(event_id)
        assert event["status"] == "confirmed"
        assert event["duration_s"] > 0


def test_ws_clip_binary_frames(settings, tmp_path):
    # 用 ffmpeg 合成一个 2s 的测试 MP4（testsrc 视频 + sine 音频）
    day = local_date_str()
    clip_dir = settings.data_dir / "clips" / day
    clip_dir.mkdir(parents=True, exist_ok=True)
    clip_file = clip_dir / "1.mp4"
    subprocess.run(
        [
            "ffmpeg", "-y",
            "-f", "lavfi", "-i", "testsrc=size=320x240:rate=10:duration=2",
            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=16000:duration=2",
            "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac",
            str(clip_file),
        ],
        check=True, capture_output=True,
    )

    app = create_app(settings)
    with TestClient(app) as client:
        storage = app.state.services.storage
        # 停后台任务，避免无关模型调用
        client.portal.call(app.state.services.detector.stop)
        client.portal.call(app.state.services.ingest.stop)

        event_id = storage.create_event(time.time() - 2)
        storage.finish_event(event_id, time.time())
        storage.set_clip_path(event_id, f"data/clips/{day}/1.mp4")

        messages = []
        with client.websocket_connect(f"/ws/clip/{event_id}") as ws:
            while True:
                msg = ws.receive()
                if "text" in msg and msg["text"] is not None:
                    import json

                    assert json.loads(msg["text"])["type"] == "clip_end"
                    break
                messages.append(msg["bytes"])

        assert messages, "no binary frames received"
        parsed = parse_frames(messages)
        videos = [p for p in parsed if p[0] == 0x01]
        audios = [p for p in parsed if p[0] == 0x02]
        assert len(videos) >= 15  # 2s @10fps ≈ 20 帧
        assert len(audios) >= 8   # 2s / 0.2s ≈ 10 块

        # 时间戳：各自单调递增，视频首帧 ts=0，步长 100ms
        v_ts = [v[1] for v in videos]
        assert v_ts[0] == 0 and v_ts == sorted(v_ts)
        assert v_ts[1] - v_ts[0] == 100
        a_ts = [a[1] for a in audios]
        assert a_ts[0] == 0 and a_ts == sorted(a_ts)

        # 视频 payload 是 JPEG，音频 payload 是偶数字节 PCM
        for _, _, payload in videos:
            assert payload[:2] == b"\xff\xd8" and payload[-2:] == b"\xff\xd9"
        for _, _, payload in audios:
            assert len(payload) % 2 == 0

        # 交错性：整体上按时间戳归并（视频 ts 与音频 ts 交替出现）
        types = [p[0] for p in parsed]
        assert 0x01 in types and 0x02 in types


def test_ws_clip_missing_event(settings):
    app = create_app(settings)
    with TestClient(app) as client:
        client.portal.call(app.state.services.detector.stop)
        client.portal.call(app.state.services.ingest.stop)
        with client.websocket_connect("/ws/clip/9999") as ws:
            msg = ws.receive_json()
            assert msg == {"type": "clip_end"}


def _drive_event(client, services, n_pos=3, n_neg=4):
    """手动喂帧走完一次事件（触发 + 释放）。blob=猫在画面（肯定），flat=空场景（否定）。"""
    services.detector.reset_baseline()  # 同上：清掉 mock cam 建立的背景基线
    ts = time.time()
    frames = [flat_jpeg()] + [blob_jpeg(i) for i in range(n_pos)] + [flat_jpeg()] * n_neg
    for i, jpeg in enumerate(frames):
        client.portal.call(services.detector.process_frame, jpeg, ts + i * 0.1)


def test_ws_event_discarded_flow(settings):
    async def id_fn(frames, refs):
        return "buding", 0.95, False  # 35B 复核：并非在喝水

    app = create_app(settings, id_fn=id_fn)
    with TestClient(app) as client:
        services = app.state.services
        client.portal.call(services.detector.stop)
        with client.websocket_connect("/ws/events") as ws:
            assert ws.receive_json()["type"] == "hello"
            _drive_event(client, services)
            started = ws.receive_json()
            assert started["type"] == "drinking_started"
            discarded = ws.receive_json()
            assert discarded["type"] == "event_discarded"
            assert discarded["event_id"] == started["event_id"]
            assert discarded["reason"] == "false_positive"
        event = services.storage.get_event(started["event_id"])
        assert event["status"] == "discarded"
        # discarded 不计数
        assert services.storage.today_summary()["counts"] == {}


def test_ws_event_unknown_then_correct(settings, make_cats):
    make_cats(settings.cats_dir)

    async def id_fn(frames, refs):
        return "buding", 0.3, True  # 置信度不足

    app = create_app(settings, id_fn=id_fn)
    with TestClient(app) as client:
        services = app.state.services
        client.portal.call(services.detector.stop)
        with client.websocket_connect("/ws/events") as ws:
            assert ws.receive_json()["type"] == "hello"
            _drive_event(client, services)
            started = ws.receive_json()
            event_id = started["event_id"]
            unknown = ws.receive_json()
            # cat_unknown：不带 cat_id，不加计数
            assert unknown == {"type": "cat_unknown", "event_id": event_id}
            assert services.storage.get_event(event_id)["status"] == "unknown"

            # 人工纠正 unknown 事件 → 广播 cat_confirmed + count_updated
            r = client.post(f"/api/events/{event_id}/correct", json={"cat_id": "naigai"})
            assert r.status_code == 200
            confirmed = ws.receive_json()
            assert confirmed["type"] == "cat_confirmed"
            assert confirmed["event_id"] == event_id
            assert confirmed["cat_id"] == "naigai"
            count_msg = ws.receive_json()
            assert count_msg["type"] == "count_updated"
            assert count_msg["counts"] == {"naigai": 1}

        assert services.storage.get_event(event_id)["status"] == "confirmed"


def test_ws_clip_silent_video_gets_silence_audio(settings):
    """I3：无音轨 MP4 回放时注入等长静音 PCM。"""
    day = local_date_str()
    clip_dir = settings.data_dir / "clips" / day
    clip_dir.mkdir(parents=True, exist_ok=True)
    clip_file = clip_dir / "silent.mp4"
    subprocess.run(
        [
            "ffmpeg", "-y",
            "-f", "lavfi", "-i", "testsrc=size=320x240:rate=10:duration=2",
            "-an", "-c:v", "libx264", "-pix_fmt", "yuv420p",
            str(clip_file),
        ],
        check=True, capture_output=True,
    )

    app = create_app(settings)
    with TestClient(app) as client:
        storage = app.state.services.storage
        client.portal.call(app.state.services.detector.stop)
        client.portal.call(app.state.services.ingest.stop)
        event_id = storage.create_event(time.time() - 2)
        storage.finish_event(event_id, time.time())
        storage.set_clip_path(event_id, f"data/clips/{day}/silent.mp4")

        messages = []
        with client.websocket_connect(f"/ws/clip/{event_id}") as ws:
            while True:
                msg = ws.receive()
                if "text" in msg and msg["text"] is not None:
                    break
                messages.append(msg["bytes"])

        parsed = parse_frames(messages)
        videos = [p for p in parsed if p[0] == 0x01]
        audios = [p for p in parsed if p[0] == 0x02]
        assert len(videos) >= 15
        # 静音块与视频等长：2s / 0.2s ≈ 10 块，内容全零
        assert len(audios) >= 8
        for ts_ms, _, payload in [(0, 0, a[2]) for a in audios]:
            assert payload == b"\x00" * len(payload)
        a_ts = [a[1] for a in audios]
        assert a_ts[0] == 0 and a_ts == sorted(a_ts)
