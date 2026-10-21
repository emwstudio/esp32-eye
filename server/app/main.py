"""main：uvicorn 入口，组装各模块并拉起后台任务。"""
from __future__ import annotations

import asyncio
import logging
from contextlib import asynccontextmanager
from pathlib import Path
from typing import AsyncIterator

from fastapi import FastAPI, WebSocket

from .api import router
from .config import Settings, load_settings
from .detector import DrinkingDetector, GateFn
from .identifier import Identifier, IdFn
from .ingest import IngestManager
from .models_client import VisionModelClient, gate_judge
from .recorder import Recorder
from .ringbuffer import RingBuffer
from .storage import Storage, local_date_str

log = logging.getLogger(__name__)
logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")


class EventBus:
    """/ws/events 连接集合，负责广播。"""

    def __init__(self) -> None:
        self._connections: set[WebSocket] = set()

    def connect(self, ws: WebSocket) -> None:
        self._connections.add(ws)

    def disconnect(self, ws: WebSocket) -> None:
        self._connections.discard(ws)

    async def broadcast(self, message: dict) -> None:
        for ws in list(self._connections):
            try:
                await ws.send_json(message)
            except Exception:
                self._connections.discard(ws)

    @property
    def count(self) -> int:
        return len(self._connections)


class Services:
    def __init__(
        self,
        settings: Settings,
        gate_fn: GateFn | None = None,
        id_fn: IdFn | None = None,
    ) -> None:
        self.settings = settings
        self.storage = Storage(settings.db_path)
        self.storage.scan_cats(settings.cats_dir)
        self.ring = RingBuffer(settings.ring_buffer_s)
        self.bus = EventBus()
        self.ingest = IngestManager(settings, self.ring)
        self.recorder = Recorder(settings, self.ring, self.storage)
        self.identifier = Identifier(settings, self.storage, self.ring, id_fn=id_fn)
        self.identifier.on_identified = lambda eid: self.spawn(self._on_identified(eid))

        if gate_fn is None:
            gate_client = VisionModelClient(
                base_url=settings.gate_model_base_url,
                api_key=settings.gate_model_api_key,
                model=settings.gate_model_name,
                timeout=30.0,
            )
            gate_fn = lambda jpeg: gate_judge(gate_client, jpeg)
        self.detector = DrinkingDetector(
            settings, self.ring, gate_fn,
            on_event_start=self._on_event_start,
            on_event_end=self._on_event_end,
        )
        self._bg_tasks: set[asyncio.Task] = set()

    def spawn(self, coro) -> None:
        """跟踪后台任务，shutdown 时统一取消。"""
        task = asyncio.get_running_loop().create_task(coro)
        self._bg_tasks.add(task)
        task.add_done_callback(self._bg_tasks.discard)

    # ---- detector 回调（运行在事件循环内） ----

    def _on_event_start(self, start_ts: float) -> int:
        event_id = self.storage.create_event(start_ts)
        self.spawn(
            self.bus.broadcast(
                {"type": "drinking_started", "event_id": event_id, "ts": start_ts}
            )
        )
        return event_id

    def _on_event_end(self, event_id: int, end_ts: float) -> None:
        self.storage.finish_event(event_id, end_ts)
        event = self.storage.get_event(event_id)
        start_ts = event["start_ts"] if event else end_ts
        self.spawn(self.recorder.make_clip(event_id, start_ts, end_ts))
        self.spawn(self.identifier.identify(event_id, start_ts, end_ts))

    async def _on_identified(self, event_id: int) -> None:
        """身份确认出终态后按状态广播。"""
        event = self.storage.get_event(event_id)
        if event is None:
            return
        status = event["status"]
        if status == "confirmed" and event["cat_id"]:
            await self.bus.broadcast(
                {
                    "type": "cat_confirmed",
                    "event_id": event_id,
                    "cat_id": event["cat_id"],
                    "confidence": event["confidence"],
                }
            )
            await self.broadcast_count_updated()
        elif status == "unknown":
            await self.bus.broadcast({"type": "cat_unknown", "event_id": event_id})
        elif status == "discarded":
            await self.bus.broadcast(
                {"type": "event_discarded", "event_id": event_id, "reason": "false_positive"}
            )

    async def broadcast_count_updated(self) -> None:
        summary = self.storage.today_summary()
        await self.bus.broadcast(
            {"type": "count_updated", "date": summary["date"], "counts": summary["counts"]}
        )

    async def debug_trigger_event(self, cat_id: str | None = None) -> int:
        """调试用（POST /api/debug/trigger）：模拟一次完整喝水事件消息流。

        不录像、不调模型——只验证事件广播与桌面屏提醒链路。
        """
        import time

        ts = time.time()
        event_id = self._on_event_start(ts)
        self.storage.finish_event(event_id, ts + 5)
        await asyncio.sleep(2)
        cats = self.storage.list_cats()
        chosen = cat_id or (cats[0]["id"] if cats else None)
        if chosen:
            self.storage.update_identity(event_id, chosen, 0.99, "confirmed")
        else:
            self.storage.update_identity(event_id, None, None, "unknown")
        await self._on_identified(event_id)
        return event_id

    def start_background(self) -> None:
        self.ingest.start()
        self.detector.start()

    async def shutdown(self) -> None:
        await self.detector.stop()
        await self.ingest.stop()
        for t in list(self._bg_tasks):
            t.cancel()
        if self._bg_tasks:
            await asyncio.gather(*self._bg_tasks, return_exceptions=True)
        self.storage.close()


def create_app(
    settings: Settings | None = None,
    gate_fn: GateFn | None = None,
    id_fn: IdFn | None = None,
) -> FastAPI:
    settings = settings or load_settings()

    @asynccontextmanager
    async def lifespan(app: FastAPI) -> AsyncIterator[None]:
        services = Services(settings, gate_fn=gate_fn, id_fn=id_fn)
        app.state.services = services
        services.start_background()
        log.info("esp32-eye server started (mock=%s)", settings.use_mock_cam)
        try:
            yield
        finally:
            await services.shutdown()

    app = FastAPI(title="esp32-eye", lifespan=lifespan)
    app.include_router(router)
    return app


app = create_app()


def main() -> None:
    import uvicorn

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    settings = load_settings()
    uvicorn.run("app.main:app", host=settings.host, port=settings.port, reload=False)


if __name__ == "__main__":
    main()
