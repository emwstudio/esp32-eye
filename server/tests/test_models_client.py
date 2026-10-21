"""I5：id_judge 多图参考分组文字块；I1：is_drinking 复核字段解析。"""
import asyncio

from app.models_client import build_id_content, extract_json_object, id_judge


class StubClient:
    """模拟 VisionModelClient.chat_content。"""

    def __init__(self, reply: str) -> None:
        self.reply = reply
        self.content: list[dict] | None = None

    async def chat_content(self, content: list[dict]) -> str:
        self.content = content
        return self.reply


def test_build_id_content_inserts_group_separators():
    content = build_id_content(
        "PROMPT", [b"F1", b"F2"], {"buding": [b"B1", b"B2"], "naigai": [b"N1"]}
    )
    assert content[0] == {"type": "text", "text": "PROMPT"}
    # 事件帧紧跟 prompt
    assert content[1]["type"] == "image_url" and content[2]["type"] == "image_url"
    # 每只猫一组：开始标记 + 参考图 + 结束标记
    assert content[3] == {"type": "text", "text": "参考照片开始：buding"}
    assert content[4]["type"] == "image_url" and content[5]["type"] == "image_url"
    assert content[6] == {"type": "text", "text": "参考照片结束：buding"}
    assert content[7] == {"type": "text", "text": "参考照片开始：naigai"}
    assert content[8]["type"] == "image_url"
    assert content[9] == {"type": "text", "text": "参考照片结束：naigai"}
    assert len(content) == 10


def test_id_judge_parses_is_drinking_false():
    client = StubClient('好的。{"cat_id": "buding", "confidence": 0.8, "is_drinking": false} 以上')
    cat_id, conf, is_drinking = asyncio.run(id_judge(client, [b"F"], {"buding": [b"B"]}))
    assert (cat_id, conf, is_drinking) == ("buding", 0.8, False)
    # 消息里含分组文字块
    texts = [c["text"] for c in client.content if c["type"] == "text"]
    assert any("参考照片开始：buding" in t for t in texts)


def test_id_judge_defaults_is_drinking_true_when_missing():
    client = StubClient('{"cat_id": "naigai", "confidence": 0.7}')
    _, _, is_drinking = asyncio.run(id_judge(client, [b"F"], {"naigai": [b"N"]}))
    assert is_drinking is True


def test_id_judge_unknown_cat_id_returns_empty():
    client = StubClient('{"cat_id": "ghost", "confidence": 0.9, "is_drinking": true}')
    cat_id, conf, _ = asyncio.run(id_judge(client, [b"F"], {"buding": [b"B"]}))
    assert cat_id == "" and conf == 0.9


def test_extract_json_object_tolerant():
    assert extract_json_object('前缀 {"a": 1} 后缀') == {"a": 1}
