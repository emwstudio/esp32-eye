"""SQLite 存储：events 表 + cats 表（从 assets/cats/ 扫描初始化）。"""
from __future__ import annotations

import logging
import sqlite3
import threading
import time
from datetime import datetime
from pathlib import Path

log = logging.getLogger(__name__)

IMAGE_EXTS = (".jpg", ".jpeg", ".png")


def local_date_str(ts: float | None = None) -> str:
    dt = datetime.fromtimestamp(ts if ts is not None else time.time()).astimezone()
    return dt.strftime("%Y-%m-%d")


def day_range(date: str) -> tuple[float, float]:
    start = datetime.strptime(date, "%Y-%m-%d").astimezone()
    end = start.timestamp() + 86400
    return start.timestamp(), end


class Storage:
    def __init__(self, db_path: Path) -> None:
        self.db_path = db_path
        db_path.parent.mkdir(parents=True, exist_ok=True)
        self._conn = sqlite3.connect(str(db_path), check_same_thread=False)
        self._conn.row_factory = sqlite3.Row
        self._lock = threading.Lock()
        self._create_tables()

    def _create_tables(self) -> None:
        with self._lock, self._conn:
            self._conn.execute(
                """
                CREATE TABLE IF NOT EXISTS events (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    cat_id TEXT,
                    confidence REAL,
                    status TEXT NOT NULL,
                    start_ts REAL NOT NULL,
                    end_ts REAL,
                    duration_s REAL,
                    clip_path TEXT,
                    created_at REAL NOT NULL
                )
                """
            )
            self._conn.execute(
                """
                CREATE TABLE IF NOT EXISTS cats (
                    id TEXT PRIMARY KEY,
                    name TEXT NOT NULL,
                    avatar_path TEXT
                )
                """
            )

    def scan_cats(self, cats_dir: Path) -> None:
        """从 assets/cats/ 扫描建表：子目录名=cat_id，avatar.jpg 优先，否则第一张参考图。"""
        with self._lock, self._conn:
            self._conn.execute("DELETE FROM cats")
            if not cats_dir.is_dir():
                log.warning("cats dir not found: %s", cats_dir)
                return
            for sub in sorted(cats_dir.iterdir()):
                if not sub.is_dir():
                    continue
                images = sorted(
                    p for p in sub.iterdir() if p.suffix.lower() in IMAGE_EXTS
                )
                avatar = sub / "avatar.jpg"
                avatar_path = avatar if avatar.exists() else (images[0] if images else None)
                self._conn.execute(
                    "INSERT OR REPLACE INTO cats (id, name, avatar_path) VALUES (?, ?, ?)",
                    (sub.name, sub.name, str(avatar_path) if avatar_path else None),
                )
            count = self._conn.execute("SELECT COUNT(*) FROM cats").fetchone()[0]
            log.info("cats scanned: %d", count)

    def list_cats(self) -> list[dict]:
        with self._lock:
            rows = self._conn.execute(
                "SELECT id, name, avatar_path FROM cats ORDER BY id"
            ).fetchall()
        return [
            {
                "id": r["id"],
                "name": r["name"],
                "avatar_url": f"/api/cats/{r['id']}/avatar",
                "_avatar_path": r["avatar_path"],
            }
            for r in rows
        ]

    def cat_avatar_path(self, cat_id: str) -> str | None:
        with self._lock:
            row = self._conn.execute(
                "SELECT avatar_path FROM cats WHERE id = ?", (cat_id,)
            ).fetchone()
        return row["avatar_path"] if row else None

    def cat_ids(self) -> list[str]:
        with self._lock:
            return [r[0] for r in self._conn.execute("SELECT id FROM cats ORDER BY id")]

    def cat_reference_images(self, cats_dir: Path) -> dict[str, list[Path]]:
        refs: dict[str, list[Path]] = {}
        for cid in self.cat_ids():
            sub = cats_dir / cid
            if sub.is_dir():
                imgs = sorted(p for p in sub.iterdir() if p.suffix.lower() in IMAGE_EXTS)
                if imgs:
                    refs[cid] = imgs
        return refs

    # ---- events ----

    def create_event(self, start_ts: float) -> int:
        with self._lock, self._conn:
            cur = self._conn.execute(
                "INSERT INTO events (status, start_ts, created_at) VALUES ('confirming', ?, ?)",
                (start_ts, time.time()),
            )
            event_id = int(cur.lastrowid)  # type: ignore[arg-type]
        log.info("event %d created (start_ts=%.3f)", event_id, start_ts)
        return event_id

    def finish_event(self, event_id: int, end_ts: float) -> None:
        with self._lock, self._conn:
            row = self._conn.execute(
                "SELECT start_ts FROM events WHERE id = ?", (event_id,)
            ).fetchone()
            if not row:
                return
            self._conn.execute(
                "UPDATE events SET end_ts = ?, duration_s = ? WHERE id = ?",
                (end_ts, end_ts - row["start_ts"], event_id),
            )
        log.info("event %d finished (duration=%.1fs)", event_id, end_ts - row["start_ts"])

    def update_identity(
        self, event_id: int, cat_id: str | None, confidence: float | None, status: str
    ) -> None:
        with self._lock, self._conn:
            self._conn.execute(
                "UPDATE events SET cat_id = ?, confidence = ?, status = ? WHERE id = ?",
                (cat_id, confidence, status, event_id),
            )

    def set_status(self, event_id: int, status: str) -> None:
        with self._lock, self._conn:
            self._conn.execute(
                "UPDATE events SET status = ? WHERE id = ?", (status, event_id)
            )

    def set_clip_path(self, event_id: int, clip_path: str) -> None:
        with self._lock, self._conn:
            self._conn.execute(
                "UPDATE events SET clip_path = ? WHERE id = ?", (clip_path, event_id)
            )

    def correct_event(self, event_id: int, cat_id: str) -> dict | None:
        with self._lock, self._conn:
            row = self._conn.execute(
                "SELECT id FROM cats WHERE id = ?", (cat_id,)
            ).fetchone()
            if not row:
                return None
            cur = self._conn.execute(
                "UPDATE events SET cat_id = ?, status = 'confirmed' WHERE id = ?",
                (cat_id, event_id),
            )
            if cur.rowcount == 0:
                return None
        return self.get_event(event_id)

    @staticmethod
    def _event_dict(r: sqlite3.Row) -> dict:
        return {
            "id": r["id"],
            "cat_id": r["cat_id"],
            "confidence": r["confidence"],
            "status": r["status"],
            "start_ts": r["start_ts"],
            "end_ts": r["end_ts"],
            "duration_s": r["duration_s"],
            "clip_path": r["clip_path"],
        }

    def get_event(self, event_id: int) -> dict | None:
        with self._lock:
            r = self._conn.execute(
                "SELECT * FROM events WHERE id = ?", (event_id,)
            ).fetchone()
        return self._event_dict(r) if r else None

    def today_summary(self, date: str | None = None) -> dict:
        date = date or local_date_str()
        start, end = day_range(date)
        with self._lock:
            rows = self._conn.execute(
                "SELECT cat_id, status FROM events WHERE start_ts >= ? AND start_ts < ?",
                (start, end),
            ).fetchall()
        counts: dict[str, int] = {}
        events_today = 0
        for r in rows:
            if r["status"] in ("confirmed", "unknown"):
                events_today += 1
            if r["status"] == "confirmed" and r["cat_id"]:
                counts[r["cat_id"]] = counts.get(r["cat_id"], 0) + 1
        return {"date": date, "counts": counts, "events_today": events_today}

    def list_clips(self, cat_id: str | None = None, date: str | None = None) -> list[dict]:
        sql = "SELECT * FROM events WHERE status IN ('confirmed', 'unknown')"
        params: list = []
        if cat_id:
            sql += " AND cat_id = ?"
            params.append(cat_id)
        if date:
            start, end = day_range(date)
            sql += " AND start_ts >= ? AND start_ts < ?"
            params.extend([start, end])
        sql += " ORDER BY start_ts DESC"
        with self._lock:
            rows = self._conn.execute(sql, params).fetchall()
        return [self._event_dict(r) for r in rows]

    def close(self) -> None:
        with self._lock:
            self._conn.close()
