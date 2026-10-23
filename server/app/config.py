"""服务端配置：全部项可经 .env 覆盖（见 .env.example）。"""
from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path

from dotenv import load_dotenv

SERVER_ROOT = Path(__file__).resolve().parent.parent
REPO_ROOT = SERVER_ROOT.parent


def _get_bool(key: str, default: bool) -> bool:
    v = os.getenv(key)
    if v is None:
        return default
    return v.strip().lower() in ("1", "true", "yes", "on")


@dataclass
class Settings:
    cam_base_url: str = ""
    cam_audio_ws_url: str = ""  # 为空时从 cam_base_url 推导 host + :81/audio
    cam_mock: bool = False

    gate_model_base_url: str = "http://127.0.0.1:8080/v1"
    gate_model_name: str = "MiniCPM-V-4.7-1B"
    gate_model_api_key: str = "none"

    id_model_base_url: str = "https://api.modelbest.cn/v1"
    id_model_name: str = "MiniCPM-V-4.7-35B-A3B"
    id_model_api_key: str = ""

    occupancy_ratio_threshold: float = 0.35  # 猫占画面比例超过该值判"在喝水"（碗沿机位语义）
    gate_interval_s: float = 1.5
    trigger_consecutive: int = 3
    release_consecutive: int = 4
    identity_confidence_min: float = 0.6

    data_dir: Path = field(default_factory=lambda: SERVER_ROOT / "data")
    cats_dir: Path = field(default_factory=lambda: REPO_ROOT / "assets" / "cats")
    ring_buffer_s: float = 40.0
    clip_pre_s: float = 5.0
    clip_post_s: float = 3.0

    host: str = "0.0.0.0"
    port: int = 8000

    @property
    def use_mock_cam(self) -> bool:
        return self.cam_mock or not self.cam_base_url

    def audio_ws_url(self) -> str:
        """音频 WS 地址：显式配置优先，否则从 cam_base_url 推导 host + :81/audio。"""
        if self.cam_audio_ws_url:
            return self.cam_audio_ws_url
        if not self.cam_base_url:
            return ""
        from urllib.parse import urlparse

        parsed = urlparse(self.cam_base_url)
        scheme = "wss" if parsed.scheme == "https" else "ws"
        host = parsed.hostname or parsed.path
        return f"{scheme}://{host}:81/audio"

    @property
    def db_path(self) -> Path:
        return self.data_dir / "app.db"

    @property
    def clips_dir(self) -> Path:
        return self.data_dir / "clips"


def load_settings(env_file: Path | None = None) -> Settings:
    load_dotenv(env_file or SERVER_ROOT / ".env")
    s = Settings()
    s.cam_base_url = os.getenv("CAM_BASE_URL", s.cam_base_url).strip()
    s.cam_audio_ws_url = os.getenv("CAM_AUDIO_WS_URL", s.cam_audio_ws_url).strip()
    s.cam_mock = _get_bool("CAM_MOCK", s.cam_mock)
    s.gate_model_base_url = os.getenv("GATE_MODEL_BASE_URL", s.gate_model_base_url)
    s.gate_model_name = os.getenv("GATE_MODEL_NAME", s.gate_model_name)
    s.gate_model_api_key = os.getenv("GATE_MODEL_API_KEY", s.gate_model_api_key)
    s.id_model_base_url = os.getenv("ID_MODEL_BASE_URL", s.id_model_base_url)
    s.id_model_name = os.getenv("ID_MODEL_NAME", s.id_model_name)
    s.id_model_api_key = os.getenv("ID_MODEL_API_KEY", s.id_model_api_key)
    s.occupancy_ratio_threshold = float(os.getenv("OCCUPANCY_RATIO_THRESHOLD", s.occupancy_ratio_threshold))
    s.gate_interval_s = float(os.getenv("GATE_INTERVAL_S", s.gate_interval_s))
    s.trigger_consecutive = int(os.getenv("TRIGGER_CONSECUTIVE", s.trigger_consecutive))
    s.release_consecutive = int(os.getenv("RELEASE_CONSECUTIVE", s.release_consecutive))
    s.identity_confidence_min = float(os.getenv("IDENTITY_CONFIDENCE_MIN", s.identity_confidence_min))
    s.ring_buffer_s = float(os.getenv("RING_BUFFER_S", s.ring_buffer_s))
    s.clip_pre_s = float(os.getenv("CLIP_PRE_S", s.clip_pre_s))
    s.clip_post_s = float(os.getenv("CLIP_POST_S", s.clip_post_s))
    if v := os.getenv("DATA_DIR"):
        p = Path(v)
        s.data_dir = p if p.is_absolute() else SERVER_ROOT / p
    if v := os.getenv("CATS_DIR"):
        p = Path(v)
        s.cats_dir = p if p.is_absolute() else SERVER_ROOT / p
    if v := os.getenv("HOST"):
        s.host = v
    if v := os.getenv("PORT"):
        s.port = int(v)
    s.data_dir.mkdir(parents=True, exist_ok=True)
    s.clips_dir.mkdir(parents=True, exist_ok=True)
    return s
