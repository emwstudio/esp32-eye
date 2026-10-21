#!/usr/bin/env python3
"""本地 1B 门控 prompt 冒烟：对普通猫照应判 is_drinking=false。

用法：.venv/bin/python tools/gate_smoke.py [--base-url http://127.0.0.1:8080/v1]
"""
from __future__ import annotations

import argparse
import asyncio
import sys
from io import BytesIO
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from app.models_client import VisionModelClient, gate_judge  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent.parent


async def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-url", default="http://127.0.0.1:8080/v1")
    args = ap.parse_args()
    client = VisionModelClient(base_url=args.base_url, api_key="", model="MiniCPM-V-4.7-1B")

    photos = sorted(ROOT.glob("assets/cats/*/*.jpeg"))[:4]
    if not photos:
        print("没有找到猫照片")
        return 1
    for p in photos:
        img = Image.open(p).convert("RGB")
        img.thumbnail((800, 800))
        buf = BytesIO()
        img.save(buf, format="JPEG", quality=80)
        try:
            is_drinking, conf = await gate_judge(client, buf.getvalue())
            print(f"{p.parent.name}/{p.name}: is_drinking={is_drinking} confidence={conf:.2f}")
        except Exception as e:  # noqa: BLE001
            print(f"{p.name}: 解析失败 {e}")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
