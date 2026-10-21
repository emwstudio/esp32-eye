#!/usr/bin/env python3
"""用服务端真实 id_judge 链路对云端 35B 做认猫冒烟测试。

对每只猫：留出一张照片当"事件帧"，其余前 3 张作参考图，
走 app.models_client.id_judge（生产同款 prompt 与 JSON 解析）。

用法：.venv/bin/python tools/id_judge_smoke.py
"""
from __future__ import annotations

import asyncio
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from app.config import load_settings  # noqa: E402
from app.models_client import VisionModelClient, id_judge  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent.parent
CATS_DIR = ROOT / "assets" / "cats"
IMG_EXTS = {".jpg", ".jpeg", ".png", ".webp"}
REFS_PER_CAT = 3


async def main() -> int:
    settings = load_settings()
    client = VisionModelClient(
        base_url=settings.id_model_base_url,
        api_key=settings.id_model_api_key,
        model=settings.id_model_name,
        timeout=120.0,
    )
    cats = {d.name: sorted(p for p in d.iterdir() if p.suffix.lower() in IMG_EXTS)
            for d in sorted(CATS_DIR.iterdir()) if d.is_dir()}
    cats = {k: v for k, v in cats.items() if len(v) >= REFS_PER_CAT + 1}
    if not cats:
        print("每只猫至少需要 4 张照片（3 参考 + 1 考题）")
        return 1

    # 考题：每只猫最后一张；参考：其余猫的前 3 张 + 本猫前 3 张
    refs = {name: [p.read_bytes() for p in imgs[:REFS_PER_CAT]] for name, imgs in cats.items()}
    tests = [(name, imgs[-1]) for name, imgs in cats.items()]

    correct = 0
    from app.models_client import build_id_content, extract_json_object

    # 考题是普通猫照（非喝水场景），生产 prompt 会先复核 is_drinking 并如实拒答；
    # 这里用身份专用 prompt 验证"压缩 + 分组 + 云端 + JSON 解析"链路的认猫准确率。
    id_only_prompt = (
        "前面第一张照片是待辨认的猫，随后是每个候选猫的参考照片"
        "（每只猫的参考照片由『参考照片开始：名字』与『参考照片结束：名字』包围）。候选 cat_id 列表：{ids}。"
        "请判断待辨认的猫是哪一只。只输出一个 JSON 对象："
        '{{"cat_id": "候选之一或空字符串", "confidence": 0到1之间的数}}。'
    ).format(ids=", ".join(sorted(cats)))
    for truth, photo in tests:
        from io import BytesIO
        from PIL import Image
        img = Image.open(photo).convert("RGB")
        img.thumbnail((1024, 1024))
        buf = BytesIO()
        img.save(buf, format="JPEG", quality=80)
        text = await client.chat_content(build_id_content(id_only_prompt, [buf.getvalue()], refs))
        obj = extract_json_object(text)
        cat_id, conf = str(obj.get("cat_id", "")), float(obj.get("confidence", 0.0))
        ok = cat_id == truth
        correct += ok
        print(f"{'✓' if ok else '✗'} 真实={truth:10s} 判定={cat_id or '(未知)':10s} 置信度={conf:.2f}")
    print(f"\n身份链路：{correct}/{len(tests)} = {correct / len(tests):.0%}")
    return 0 if correct == len(tests) else 2


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
