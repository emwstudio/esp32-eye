import io
import time

import numpy as np
import pytest
from PIL import Image

from app.config import Settings


@pytest.fixture
def settings(tmp_path):
    s = Settings()
    s.data_dir = tmp_path / "data"
    s.data_dir.mkdir(parents=True)
    s.clips_dir.mkdir(parents=True, exist_ok=True)
    s.cats_dir = tmp_path / "cats"
    s.cats_dir.mkdir()
    s.cam_mock = True
    s.gate_interval_s = 0.0
    return s


def random_jpeg(seed: int, size: int = 96) -> bytes:
    rng = np.random.default_rng(seed)
    img = rng.integers(0, 255, (size, size), dtype=np.uint8)
    buf = io.BytesIO()
    Image.fromarray(img, mode="L").convert("RGB").save(buf, format="JPEG")
    return buf.getvalue()


def flat_jpeg(gray: int = 128, size: int = 96) -> bytes:
    img = np.full((size, size), gray, dtype=np.uint8)
    buf = io.BytesIO()
    Image.fromarray(img, mode="L").convert("RGB").save(buf, format="JPEG")
    return buf.getvalue()


@pytest.fixture
def make_cats(tmp_path):
    """在 settings.cats_dir 下生成两只猫的参考照片。"""
    def _make(cats_dir):
        for cid in ("buding", "naigai"):
            d = cats_dir / cid
            d.mkdir(parents=True, exist_ok=True)
            (d / "ref1.jpg").write_bytes(random_jpeg(hash(cid) % 1000))
        (cats_dir / "buding" / "avatar.jpg").write_bytes(random_jpeg(7))
    return _make
