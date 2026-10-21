#!/usr/bin/env python3
"""1B 认猫能力实测（spike）

用法：
    .venv/bin/python tools/cat_id_spike.py [--holdout N] [--base-url http://127.0.0.1:8080/v1]

原理：扫描 assets/cats/<cat_id>/ 下照片。每只猫留出 holdout 张作"考题"，
其余作参考照片。对每张考题，把所有猫的参考照片 + 考题一起发给本地
llama-server 上的 MiniCPM-V 多图输入，问"这是哪只猫"，统计准确率与混淆矩阵。
会用 3 种 prompt 写法各测一轮，输出各自结果供比较。
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import random
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent  # 仓库根（server/tools/ 的上两级）
CATS_DIR = ROOT / "assets" / "cats"
IMG_EXTS = {".jpg", ".jpeg", ".png", ".webp"}


def b64_url(p: Path) -> str:
    """读图并归一化：长边压到 1024、转 JPEG q80，再编 base64。

    手机原图（12MP+）直传会让 prompt 体积和推理延迟暴涨，模型收益却很小。
    """
    from io import BytesIO

    from PIL import Image

    img = Image.open(p).convert("RGB")
    img.thumbnail((1024, 1024))
    buf = BytesIO()
    img.save(buf, format="JPEG", quality=80)
    return "data:image/jpeg;base64," + base64.b64encode(buf.getvalue()).decode()


def chat(base_url: str, model: str, content: list[dict], max_tokens: int = 64,
         api_key: str | None = None) -> str:
    payload = {"model": model, "messages": [{"role": "user", "content": content}],
               "max_tokens": max_tokens, "temperature": 0.1}
    headers = {"Content-Type": "application/json"}
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"
    req = urllib.request.Request(
        f"{base_url}/chat/completions",
        data=json.dumps(payload).encode(),
        headers=headers)
    resp = json.loads(urllib.request.urlopen(req, timeout=300).read())
    return resp["choices"][0]["message"]["content"]


PROMPTS = {
    "zh_direct": (
        "前面 {n} 组照片是 {n} 只不同的猫的参考照片，顺序依次是：{names}。"
        "最后一张照片是今天拍到的猫。请判断最后这张照片里是哪一只猫。"
        "只回答猫的名字，不要解释。"
    ),
    "zh_reason": (
        "你是猫咪辨认专家。参考照片依次是：{names}。"
        "最后一张是待辨认的照片。先对比花色、斑纹、脸型特征，然后给出答案。"
        "最后一行必须只写：答案：<名字>"
    ),
    "en_direct": (
        "The first {n} groups of photos are reference photos of {n} different cats, "
        "in order: {names}. The last photo is the cat to identify. "
        "Which cat is it? Answer with the name only."
    ),
}


def parse_answer(text: str, names: list[str]) -> str | None:
    for n in names:
        if n in text:
            return n
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--holdout", type=int, default=1)
    ap.add_argument("--base-url", default="http://127.0.0.1:8080/v1")
    ap.add_argument("--model", default="MiniCPM-V-4.7-1B")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--max-refs", type=int, default=0, help="每只猫最多用几张参考照片，0=全部")
    ap.add_argument("--max-tokens", type=int, default=256)
    ap.add_argument("--strategy", choices=["multi", "binary", "both"], default="multi",
                    help="multi=四选一多图对比；binary=逐只猫二选一验证；both=两种都跑")
    ap.add_argument("--api-key", default=None, help="云端 API Key；默认读 server/.env 的 ID_MODEL_API_KEY")
    args = ap.parse_args()

    if args.api_key is None:
        try:
            from dotenv import load_dotenv
            load_dotenv(Path(__file__).resolve().parent.parent / ".env")
            args.api_key = os.environ.get("ID_MODEL_API_KEY") or None
        except ImportError:
            args.api_key = os.environ.get("ID_MODEL_API_KEY") or None

    cats: dict[str, list[Path]] = {}
    for d in sorted(CATS_DIR.iterdir()):
        if d.is_dir():
            imgs = sorted(p for p in d.iterdir() if p.suffix.lower() in IMG_EXTS)
            if imgs:
                cats[d.name] = imgs
    if len(cats) < 2:
        print(f"需要至少两只猫的照片（assets/cats/<名字>/*.jpg），当前只有 {len(cats)} 只")
        return 1

    names = list(cats)
    random.seed(args.seed)
    refs: dict[str, list[Path]] = {}
    tests: list[tuple[str, Path]] = []
    for name, imgs in cats.items():
        if len(imgs) < args.holdout + 1:
            print(f"[跳过] {name} 只有 {len(imgs)} 张照片，至少需要 {args.holdout + 1} 张")
            continue
        shuffled = imgs[:]
        random.shuffle(shuffled)
        tests += [(name, p) for p in shuffled[: args.holdout]]
        pool = shuffled[args.holdout:]
        refs[name] = pool[: args.max_refs] if args.max_refs > 0 else pool

    print(f"猫：{names}，考题 {len(tests)} 张\n")

    def run_binary() -> None:
        """逐只猫二选一验证：每次只给一只猫的参考图 + 考题，问'是不是它'。"""
        correct = 0
        matrix: dict[str, dict[str, int]] = {n: {} for n in names}
        t0 = time.time()
        for truth, photo in tests:
            votes: list[str] = []
            for n in names:
                content: list[dict] = [{"type": "text", "text": (
                    f"前面的照片是一只名叫 {n} 的猫的参考照片。"
                    "最后一张是待辨认的照片。请判断待辨认的猫是不是这只猫。"
                    "只回答：是 或 不是。")}]
                for r in refs[n]:
                    content.append({"type": "image_url", "image_url": {"url": b64_url(r)}})
                content.append({"type": "text", "text": "待辨认照片："})
                content.append({"type": "image_url", "image_url": {"url": b64_url(photo)}})
                try:
                    out = chat(args.base_url, args.model, content, args.max_tokens, args.api_key)
                except Exception as e:  # noqa: BLE001
                    print(f"  [错误] {truth} vs {n}: {e}")
                    continue
                # "不是" 含 "是"，先剔除再判断
                cleaned = out.replace("不是", "").replace("否", "")
                if "是" in cleaned or cleaned.strip().lower().startswith("yes"):
                    votes.append(n)
            pred = votes[0] if len(votes) == 1 else None
            ok = pred == truth
            correct += ok
            if pred:
                matrix[truth][pred] = matrix[truth].get(pred, 0) + 1
            mark = "✓" if ok else f"✗→{pred or ('多票' if len(votes) > 1 else '无票')}{votes if not ok else ''}"
            print(f"  [binary] {truth}/{photo.name}: {mark}")
        total = len(tests)
        print(f"\n== binary: {correct}/{total} = {correct / total:.0%}，耗时 {time.time() - t0:.0f}s")
        for t_, row in matrix.items():
            wrong = {k: v for k, v in row.items() if k != t_}
            if wrong:
                print(f"   混淆: {t_} 被认成 {wrong}")
        print()

    if args.strategy in ("multi", "both"):
        for pname, tmpl in PROMPTS.items():
            correct = 0
            matrix: dict[str, dict[str, int]] = {n: {} for n in names}
            t0 = time.time()
            for truth, photo in tests:
                content: list[dict] = []
                for n in names:
                    # 每组参考图前加分隔文字块，使模型能把图组映射到候选
                    content.append({"type": "text", "text": f"参考照片：{n}（cat_id={n}）"})
                    for r in refs[n]:
                        content.append({"type": "image_url", "image_url": {"url": b64_url(r)}})
                content.append({"type": "text", "text": "待辨认照片："})
                content.append({"type": "image_url", "image_url": {"url": b64_url(photo)}})
                content.insert(0, {"type": "text", "text": tmpl.format(n=len(names), names="、".join(names))})
                try:
                    out = chat(args.base_url, args.model, content, args.max_tokens, args.api_key)
                except Exception as e:  # noqa: BLE001
                    print(f"  [错误] {truth} {photo.name}: {e}")
                    continue
                pred = parse_answer(out, names)
                ok = pred == truth
                correct += ok
                if pred:
                    matrix[truth][pred] = matrix[truth].get(pred, 0) + 1
                mark = "✓" if ok else f"✗→{pred}"
                print(f"  [{pname}] {truth}/{photo.name}: {mark}  （模型原文: {out.strip()[:60]}）")
            total = len(tests)
            print(f"\n== {pname}: {correct}/{total} = {correct / total:.0%}，耗时 {time.time() - t0:.0f}s")
            for t_, row in matrix.items():
                wrong = {k: v for k, v in row.items() if k != t_}
                if wrong:
                    print(f"   混淆: {t_} 被认成 {wrong}")
            print()

    if args.strategy in ("binary", "both"):
        run_binary()
    return 0


if __name__ == "__main__":
    sys.exit(main())
