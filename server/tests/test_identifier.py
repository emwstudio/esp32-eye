"""I1+I2：identifier 的 discarded / unknown / confirmed 三分支与重试。"""
import asyncio
import time

from app.identifier import Identifier
from app.ringbuffer import RingBuffer
from app.storage import Storage


def make_identifier(settings, id_fn):
    storage = Storage(settings.db_path)
    storage.scan_cats(settings.cats_dir)
    ident = Identifier(settings, storage, RingBuffer(40), id_fn=id_fn)
    ident.backoff_s = 0  # 测试不等退避
    return ident, storage


def run(ident: Identifier, event_id: int) -> None:
    ts = time.time()
    asyncio.run(ident.identify(event_id, ts, ts + 1))


def test_discarded_when_model_says_not_drinking(settings):
    seen = []

    async def id_fn(frames, refs):
        return "buding", 0.95, False  # 认出猫，但复核判定并非在喝水

    ident, storage = make_identifier(settings, id_fn)
    ident.on_identified = seen.append
    event_id = storage.create_event(time.time())
    run(ident, event_id)
    event = storage.get_event(event_id)
    assert event["status"] == "discarded"
    assert seen == [event_id]


def test_unknown_on_low_confidence(settings):
    async def id_fn(frames, refs):
        return "buding", 0.3, True

    ident, storage = make_identifier(settings, id_fn)
    event_id = storage.create_event(time.time())
    run(ident, event_id)
    event = storage.get_event(event_id)
    assert event["status"] == "unknown"
    assert event["cat_id"] == "buding"  # 保留模型猜测供人工参考
    assert event["confidence"] == 0.3


def test_unknown_on_empty_cat_id(settings):
    async def id_fn(frames, refs):
        return "", 0.99, True

    ident, storage = make_identifier(settings, id_fn)
    event_id = storage.create_event(time.time())
    run(ident, event_id)
    assert storage.get_event(event_id)["status"] == "unknown"


def test_confirmed_on_high_confidence(settings):
    async def id_fn(frames, refs):
        return "buding", 0.9, True

    ident, storage = make_identifier(settings, id_fn)
    event_id = storage.create_event(time.time())
    run(ident, event_id)
    event = storage.get_event(event_id)
    assert event["status"] == "confirmed"
    assert event["cat_id"] == "buding" and event["confidence"] == 0.9


def test_retry_then_success(settings):
    calls = {"n": 0}

    async def id_fn(frames, refs):
        calls["n"] += 1
        if calls["n"] < 3:
            raise ConnectionError("cloud unreachable")
        return "naigai", 0.8, True

    ident, storage = make_identifier(settings, id_fn)
    event_id = storage.create_event(time.time())
    run(ident, event_id)
    assert calls["n"] == 3
    assert storage.get_event(event_id)["status"] == "confirmed"


def test_gives_up_after_max_attempts_stays_confirming(settings):
    calls = {"n": 0}

    async def id_fn(frames, refs):
        calls["n"] += 1
        raise ConnectionError("cloud unreachable")

    ident, storage = make_identifier(settings, id_fn)
    ident.on_identified = lambda eid: (_ for _ in ()).throw(AssertionError("不应回调"))
    event_id = storage.create_event(time.time())
    run(ident, event_id)
    assert calls["n"] == 3
    assert storage.get_event(event_id)["status"] == "confirming"
