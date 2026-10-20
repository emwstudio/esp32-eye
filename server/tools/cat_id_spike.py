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
import random
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent  # 仓库根（server/tools/ 的上两级）
CATS_DIR = ROOT / "assets" / "cats"
IMG_EXTS = {".jpg", ".jpeg", ".png", ".webp"}


def b64_url(p: Path) -> str:
    mime = "image/png" if p.suffix.lower() == ".png" else "image/jpeg"
    return f"data:{mime};base64," + base64.b64encode(p.read_bytes()).decode()


def chat(base_url: str, model: str, content: list[dict], max_tokens: int = 64) -> str:
    payload = {"model": model, "messages": [{"role": "user", "content": content}],
               "max_tokens": max_tokens, "temperature": 0.1}
    req = urllib.request.Request(
        f"{base_url}/chat/completions",
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"})
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
    args = ap.parse_args()

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
        refs[name] = shuffled[args.holdout:]

    print(f"猫：{names}，考题 {len(tests)} 张\n")
    for pname, tmpl in PROMPTS.items():
        correct = 0
        matrix: dict[str, dict[str, int]] = {n: {} for n in names}
        t0 = time.time()
        for truth, photo in tests:
            content: list[dict] = []
            for n in names:
                for r in refs[n]:
                    content.append({"type": "image_url", "image_url": {"url": b64_url(r)}})
            content.append({"type": "image_url", "image_url": {"url": b64_url(photo)}})
            content.insert(0, {"type": "text", "text": tmpl.format(n=len(names), names="、".join(names))})
            try:
                out = chat(args.base_url, args.model, content)
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
    return 0


if __name__ == "__main__":
    sys.exit(main())
