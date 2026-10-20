"""统一的 OpenAI 兼容视觉模型客户端（门控 1B 与云端 35B 只是两个配置实例）。"""
from __future__ import annotations

import base64
import json
import logging
from dataclasses import dataclass

import httpx

log = logging.getLogger(__name__)


def extract_json_object(text: str) -> dict:
    """tolerant JSON 提取：取第一个 { 到最后一个 }。"""
    start = text.find("{")
    end = text.rfind("}")
    if start < 0 or end <= start:
        raise ValueError(f"no JSON object in model output: {text!r}")
    return json.loads(text[start : end + 1])


def jpeg_data_url(data: bytes) -> str:
    return "data:image/jpeg;base64," + base64.b64encode(data).decode()


@dataclass
class VisionModelClient:
    base_url: str
    api_key: str
    model: str
    timeout: float = 60.0

    async def chat_with_images(self, prompt: str, images: list[bytes]) -> str:
        content: list[dict] = [{"type": "text", "text": prompt}]
        for img in images:
            content.append(
                {"type": "image_url", "image_url": {"url": jpeg_data_url(img)}}
            )
        payload = {
            "model": self.model,
            "messages": [{"role": "user", "content": content}],
            "temperature": 0,
        }
        headers = {"Authorization": f"Bearer {self.api_key}"} if self.api_key else {}
        url = self.base_url.rstrip("/") + "/chat/completions"
        async with httpx.AsyncClient(timeout=self.timeout) as client:
            resp = await client.post(url, json=payload, headers=headers)
            resp.raise_for_status()
            data = resp.json()
        return str(data["choices"][0]["message"]["content"])


GATE_PROMPT = (
    "这是一张猫饮水机旁的监控画面。判断画面中是否有猫正在喝水。"
    "只输出一个 JSON 对象，不要输出任何其他文字："
    '{"is_drinking": true 或 false, "confidence": 0到1之间的数}'
)

ID_PROMPT_TEMPLATE = (
    "前面若干张是一次猫喝水事件的监控画面，随后是每个候选猫的参考照片"
    "（每组参考照片前的文字标明其 cat_id）。候选 cat_id 列表：{cat_ids}。"
    "请判断事件中的猫最可能是哪一只，只输出一个 JSON 对象："
    '{"cat_id": "候选之一", "confidence": 0到1之间的数}。'
    "若无法判断，confidence 给低分。"
)


async def gate_judge(client: VisionModelClient, jpeg: bytes) -> tuple[bool, float]:
    """第 1 层门控：返回 (is_drinking, confidence)。"""
    text = await client.chat_with_images(GATE_PROMPT, [jpeg])
    obj = extract_json_object(text)
    return bool(obj.get("is_drinking")), float(obj.get("confidence", 0.0))


async def id_judge(
    client: VisionModelClient,
    frames: list[bytes],
    refs: dict[str, list[bytes]],
) -> tuple[str, float]:
    """身份确认：返回 (cat_id, confidence)。未知时 cat_id 为 ""。"""
    cat_ids = sorted(refs.keys())
    if not cat_ids:
        return "", 0.0
    prompt = ID_PROMPT_TEMPLATE.format(cat_ids=", ".join(cat_ids))
    images: list[bytes] = list(frames)
    # 参考图跟在事件帧之后；prompt 中说明顺序
    for cid in cat_ids:
        images.extend(refs[cid])
    text = await client.chat_with_images(prompt, images)
    obj = extract_json_object(text)
    cat_id = str(obj.get("cat_id", ""))
    confidence = float(obj.get("confidence", 0.0))
    if cat_id not in refs:
        log.info("id model returned unknown cat_id %r", cat_id)
        return "", confidence
    return cat_id, confidence
