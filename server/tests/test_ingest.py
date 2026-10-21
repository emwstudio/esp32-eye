"""C1/I4：音频 WS 地址推导与设备时间戳换算。"""
import logging
import time

from app.config import Settings, load_settings
from app.ingest import CamIngest
from app.ringbuffer import RingBuffer


def make_ingest(settings) -> CamIngest:
    return CamIngest(settings, RingBuffer(40))


# ---- C1：CAM_AUDIO_WS_URL ----

def test_audio_ws_url_derived_from_http():
    s = Settings(cam_base_url="http://catcam.local")
    assert s.audio_ws_url() == "ws://catcam.local:81/audio"


def test_audio_ws_url_derived_strips_port_and_https():
    s = Settings(cam_base_url="https://cam.example.com:8080")
    assert s.audio_ws_url() == "wss://cam.example.com:81/audio"


def test_audio_ws_url_explicit_override():
    s = Settings(cam_base_url="http://catcam.local",
                 cam_audio_ws_url="ws://192.168.1.50:9000/audio")
    assert s.audio_ws_url() == "ws://192.168.1.50:9000/audio"


def test_audio_ws_url_from_env(monkeypatch, tmp_path):
    monkeypatch.setenv("CAM_BASE_URL", "http://catcam.local")
    monkeypatch.setenv("CAM_AUDIO_WS_URL", "ws://10.0.0.2:81/audio")
    s = load_settings(tmp_path / ".env")  # 文件不存在也没关系
    assert s.cam_audio_ws_url == "ws://10.0.0.2:81/audio"


# ---- I4：设备时间戳 ----

def test_device_ts_used_when_ntp_synced():
    ingest = make_ingest(Settings(cam_base_url="http://cam"))
    now = time.time()
    device_us = int((now - 0.05) * 1e6)  # 设备时钟与本地一致，领先 50ms 传输延迟
    ts = ingest.audio_chunk_ts(device_us, now)
    assert ts == device_us / 1e6


def test_device_ts_fallback_when_unsynced(caplog):
    ingest = make_ingest(Settings(cam_base_url="http://cam"))
    now = time.time()
    uptime_us = 5_000_000  # 设备未同步 NTP，时间戳是上电微秒数
    with caplog.at_level(logging.WARNING):
        ts1 = ingest.audio_chunk_ts(uptime_us, now)
        ts2 = ingest.audio_chunk_ts(uptime_us + 100_000, now + 0.1)
    assert ts1 == now and ts2 == now + 0.1
    warnings = [r for r in caplog.records if r.levelno >= logging.WARNING]
    assert len(warnings) == 1  # 只警告一次
