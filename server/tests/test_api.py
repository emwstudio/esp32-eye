import time

from fastapi.testclient import TestClient

from app.main import create_app


async def gate_negative(jpeg: bytes):
    return False, 0.0


async def id_mock(frames, refs):
    return "buding", 0.9


def make_client(settings):
    app = create_app(settings, gate_fn=gate_negative, id_fn=id_mock)
    return TestClient(app)


def test_cats_empty(settings):
    with make_client(settings) as client:
        r = client.get("/api/cats")
        assert r.status_code == 200
        assert r.json() == []


def test_cats_with_photos(settings, make_cats):
    make_cats(settings.cats_dir)
    with make_client(settings) as client:
        r = client.get("/api/cats")
        assert r.status_code == 200
        cats = r.json()
        assert {c["id"] for c in cats} == {"buding", "naigai"}
        assert cats[0]["avatar_url"] == f"/api/cats/{cats[0]['id']}/avatar"

        r = client.get("/api/cats/buding/avatar")
        assert r.status_code == 200
        assert r.headers["content-type"] == "image/jpeg"
        assert r.content[:2] == b"\xff\xd8"  # JPEG SOI

        r = client.get("/api/cats/ghost/avatar")
        assert r.status_code == 404


def test_health(settings):
    with make_client(settings) as client:
        r = client.get("/api/health")
        assert r.status_code == 200
        body = r.json()
        assert body["ok"] is True
        assert body["cam_connected"] is True  # mock 模式视为已连接
        assert body["audio_connected"] is True  # 新增字段：音频连接状态
        assert body["gate_model"] == settings.gate_model_name
        assert body["id_model"] == settings.id_model_name


def test_today_and_clips_empty(settings):
    with make_client(settings) as client:
        r = client.get("/api/today")
        assert r.status_code == 200
        body = r.json()
        assert set(body.keys()) == {"date", "counts", "events_today"}
        assert body["counts"] == {} and body["events_today"] == 0

        r = client.get("/api/clips")
        assert r.status_code == 200
        assert r.json() == []


def test_correct_event(settings, make_cats):
    make_cats(settings.cats_dir)
    with make_client(settings) as client:
        storage = client.app.state.services.storage
        event_id = storage.create_event(time.time())
        storage.finish_event(event_id, time.time())

        r = client.post(f"/api/events/{event_id}/correct", json={"cat_id": "naigai"})
        assert r.status_code == 200
        body = r.json()
        assert body["cat_id"] == "naigai" and body["status"] == "confirmed"
        assert set(body.keys()) == {
            "id", "cat_id", "confidence", "status",
            "start_ts", "end_ts", "duration_s", "clip_path",
        }

        # 纠正后 today/clips 反映
        today = client.get("/api/today").json()
        assert today["counts"] == {"naigai": 1} and today["events_today"] == 1
        clips = client.get("/api/clips", params={"cat_id": "naigai"}).json()
        assert len(clips) == 1 and clips[0]["id"] == event_id
        clips_none = client.get("/api/clips", params={"cat_id": "buding"}).json()
        assert clips_none == []

        # 不存在的 event / cat → 404
        assert client.post("/api/events/9999/correct", json={"cat_id": "naigai"}).status_code == 404
        assert client.post(f"/api/events/{event_id}/correct", json={"cat_id": "ghost"}).status_code == 404
