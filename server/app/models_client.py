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


def scale_to_jpeg(data: bytes, max_edge: int = 1024, quality: int = 80) -> bytes:
    """把任意图片压到长边 max_edge 的 JPEG。解码失败时原样返回（容错）。"""
    from io import BytesIO

    from PIL import Image

    try:
        img = Image.open(BytesIO(data)).convert("RGB")
        img.thumbnail((max_edge, max_edge))
        buf = BytesIO()
        img.save(buf, format="JPEG", quality=quality)
        return buf.getvalue()
    except Exception:  # noqa: BLE001
        log.warning("image rescale failed, sending original bytes", exc_info=True)
        return data


@dataclass
class VisionModelClient:
    base_url: str
    api_key: str
    model: str
    timeout: float = 60.0

    def _bypass_proxy(self) -> bool:
        """本地地址（localhost/127.x/*.local/内网 IP）不走系统代理。"""
        from urllib.parse import urlparse

        host = urlparse(self.base_url).hostname or ""
        if host in ("localhost",) or host.endswith(".local"):
            return True
        if host.startswith("127.") or host.startswith("192.168.") or host.startswith("10."):
            return True
        return False

    async def chat_content(self, content: list[dict]) -> str:
        """发送多模态 content（text/image_url 交错）并返回文本。"""
        payload = {
            "model": self.model,
            "messages": [{"role": "user", "content": content}],
            "temperature": 0,
        }
        headers = {"Authorization": f"Bearer {self.api_key}"} if self.api_key else {}
        url = self.base_url.rstrip("/") + "/chat/completions"
        async with httpx.AsyncClient(
            timeout=self.timeout, trust_env=not self._bypass_proxy()
        ) as client:
            resp = await client.post(url, json=payload, headers=headers)
            resp.raise_for_status()
            data = resp.json()
        return str(data["choices"][0]["message"]["content"])

    async def chat_with_images(self, prompt: str, images: list[bytes]) -> str:
        content: list[dict] = [{"type": "text", "text": prompt}]
        for img in images:
            content.append(
                {"type": "image_url", "image_url": {"url": jpeg_data_url(img)}}
            )
        return await self.chat_content(content)


GATE_PROMPT = (
    "这是一张固定在猫水碗碗沿、朝斜上方拍摄的摄像头画面（拍不到水）。"
    "问题：画面里有没有猫？"
    "只输出一个 JSON 对象，不要输出任何其他文字："
    '{"cat_present": true 或 false, "confidence": 0到1之间的数}'
)

ID_PROMPT_TEMPLATE = (
    "前面若干张是一次猫喝水事件的监控画面（摄像头固定在水碗碗沿直拍猫脸特写，看不到水），"
    "随后是每个候选猫的参考照片"
    "（每只猫的参考照片由『参考照片开始：名字』与『参考照片结束：名字』包围）。"
    "候选 cat_id 列表：{cat_ids}。"
    "请先复核监控画面里是否真的有猫的头部特写几乎占满画面（正在碗边喝水）；"
    "若确实在喝水，根据毛色花纹判断是哪一只。"
    "只输出一个 JSON 对象："
    '{{"cat_id": "候选之一或空字符串", "confidence": 0到1之间的数, '
    '"is_drinking": true 或 false}}。'
    "若并非在喝水，is_drinking=false；若无法判断是哪只猫，confidence 给低分。"
)


async def gate_judge(client: VisionModelClient, jpeg: bytes) -> tuple[bool, float]:
    """第 1 层门控（1B 的职责：画面里有没有猫）。返回 (cat_present, confidence)。

    "占没占满画面"不由它判断（实测 1B 不具备空间占比判断能力），
    占比由检测器的背景差分给出，两者相与才是肯定票。
    """
    text = await client.chat_with_images(GATE_PROMPT, [jpeg])
    obj = extract_json_object(text)
    present = bool(obj.get("cat_present", obj.get("is_drinking")))
    conf = float(obj.get("confidence", 0.0))
    return present, min(conf, 1.0)


def build_id_content(
    prompt: str, frames: list[bytes], refs: dict[str, list[bytes]]
) -> list[dict]:
    """事件帧在前，随后每只猫一组：开始/结束标记包住该猫的参考图。

    实测发现：只用"组前分隔文字"时，模型会把文字关联到前一组图片，
    造成整体错位一位（见 tools/id_judge_smoke.py 的循环错位现象）。
    用开始+结束标记双向包围后分组无歧义。
    所有图片统一经 scale_to_jpeg 压缩——手机原图直传会触发云端 413。
    """
    content: list[dict] = [{"type": "text", "text": prompt}]
    for img in frames:
        content.append({"type": "image_url", "image_url": {"url": jpeg_data_url(scale_to_jpeg(img))}})
    for cat_id in sorted(refs.keys()):
        content.append({"type": "text", "text": f"参考照片开始：{cat_id}"})
        for img in refs[cat_id]:
            content.append(
                {"type": "image_url", "image_url": {"url": jpeg_data_url(scale_to_jpeg(img))}}
            )
        content.append({"type": "text", "text": f"参考照片结束：{cat_id}"})
    return content


async def id_judge(
    client: VisionModelClient,
    frames: list[bytes],
    refs: dict[str, list[bytes]],
) -> tuple[str, float, bool]:
    """身份确认 + 喝水复核：返回 (cat_id, confidence, is_drinking)。未知时 cat_id 为 ""。"""
    cat_ids = sorted(refs.keys())
    if not cat_ids:
        return "", 0.0, True
    prompt = ID_PROMPT_TEMPLATE.format(cat_ids=", ".join(cat_ids))
    text = await client.chat_content(build_id_content(prompt, frames, refs))
    obj = extract_json_object(text)
    cat_id = str(obj.get("cat_id", ""))
    confidence = float(obj.get("confidence", 0.0))
    is_drinking = bool(obj.get("is_drinking", True))
    if cat_id not in refs:
        log.info("id model returned unknown cat_id %r", cat_id)
        cat_id = ""
    return cat_id, confidence, is_drinking
