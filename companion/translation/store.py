"""Isolated SQLite metadata for translation jobs.

The ledger stores identifiers, hashes, states, byte counts and timing only.
Source text, translation text and audio never enter this database, the task
store or the task journals. Terminal rows are retained as deduplication
evidence; capacity pressure rejects new admissions instead of evicting them.
"""

from contextlib import contextmanager
import os
from pathlib import Path
import re
import sqlite3
import time

from .common import MAX_METADATA_ROWS, TranslationError

TERMINAL = {"DONE", "FAILED", "CANCELLED", "UNKNOWN", "EXPIRED"}
ERROR_PATTERN = re.compile(r"[A-Z][A-Z0-9_]{0,31}\Z")

SCHEMA = """
CREATE TABLE IF NOT EXISTS translation_meta (
    id INTEGER PRIMARY KEY CHECK(id=1),
    account TEXT NOT NULL,
    next_generation INTEGER NOT NULL DEFAULT 1
);
CREATE TABLE IF NOT EXISTS translation_jobs (
    operation_id TEXT PRIMARY KEY,
    payload_hash TEXT NOT NULL,
    job_id TEXT NOT NULL UNIQUE,
    source_lang TEXT NOT NULL,
    target_lang TEXT NOT NULL,
    audio_sha256 TEXT NOT NULL,
    audio_bytes INTEGER NOT NULL,
    generation INTEGER NOT NULL,
    state TEXT NOT NULL,
    stage TEXT NOT NULL DEFAULT 'NONE',
    asr_claimed INTEGER NOT NULL DEFAULT 0,
    mt_claimed INTEGER NOT NULL DEFAULT 0,
    source_hash TEXT,
    target_hash TEXT,
    source_bytes INTEGER NOT NULL DEFAULT 0,
    target_bytes INTEGER NOT NULL DEFAULT 0,
    created INTEGER NOT NULL,
    updated INTEGER NOT NULL,
    error TEXT
);
CREATE INDEX IF NOT EXISTS translation_jobs_job
    ON translation_jobs(job_id);
CREATE TABLE IF NOT EXISTS translation_quota (
    day INTEGER PRIMARY KEY,
    asr_seconds INTEGER NOT NULL DEFAULT 0,
    mt_jobs INTEGER NOT NULL DEFAULT 0,
    updated INTEGER NOT NULL
);
"""


class TranslationStore:
    def __init__(self, path, account_id="translation-t1", clock=time.time,
                 capacity=MAX_METADATA_ROWS):
        if (type(account_id) is not str or not 1 <= len(account_id) <= 64
                or type(capacity) is not int
                or not 1 <= capacity <= MAX_METADATA_ROWS):
            raise TranslationError("INVALID_CONFIG")
        self.path = Path(path)
        self.clock = clock
        self.capacity = capacity
        self.path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        if self.path.is_symlink():
            raise TranslationError("INVALID_CONFIG")
        fd = os.open(self.path, os.O_CREAT | os.O_RDWR, 0o600)
        os.close(fd)
        if os.name != "nt" and self.path.stat().st_mode & 0o077:
            raise TranslationError("PRIVATE_FILE_REQUIRED")
        with self.connect() as db:
            db.executescript(SCHEMA)
            db.execute("INSERT OR IGNORE INTO translation_meta(id,account) "
                       "VALUES(1,?)", (account_id,))
            if db.execute("SELECT account FROM translation_meta WHERE id=1"
                          ).fetchone()[0] != account_id:
                raise TranslationError("ACCOUNT_MISMATCH")

    @contextmanager
    def connect(self):
        db = sqlite3.connect(self.path, timeout=5)
        try:
            db.row_factory = sqlite3.Row
            db.execute("PRAGMA synchronous=FULL")
            db.execute("PRAGMA secure_delete=ON")
            with db:
                yield db
        finally:
            db.close()

    @staticmethod
    def _row(row):
        return dict(row) if row is not None else None

    def row(self, operation_id):
        with self.connect() as db:
            return self._row(db.execute(
                "SELECT * FROM translation_jobs WHERE operation_id=?",
                (operation_id,)).fetchone())

    def row_by_job(self, job_id):
        with self.connect() as db:
            return self._row(db.execute(
                "SELECT * FROM translation_jobs WHERE job_id=?",
                (job_id,)).fetchone())

    def count(self):
        with self.connect() as db:
            return db.execute("SELECT COUNT(*) FROM translation_jobs"
                              ).fetchone()[0]

    @staticmethod
    def _quota_day(day):
        if type(day) is not int or day < 0:
            raise TranslationError("INVALID_CONFIG")
        return day

    def quota(self, day):
        day = self._quota_day(day)
        with self.connect() as db:
            row = db.execute(
                "SELECT asr_seconds,mt_jobs FROM translation_quota WHERE day=?",
                (day,)).fetchone()
            if row is None:
                return {"day": day, "asr_seconds": 0, "mt_jobs": 0}
            return {"day": day, "asr_seconds": row[0], "mt_jobs": row[1]}

    def reserve_asr(self, day, seconds, limit):
        """Atomically reserve billed ASR seconds for one UTC day."""
        day = self._quota_day(day)
        if (type(seconds) is not int or seconds < 0
                or type(limit) is not int or not 1 <= limit <= 86400):
            raise TranslationError("INVALID_CONFIG")
        now = int(self.clock())
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute(
                "SELECT asr_seconds FROM translation_quota WHERE day=?",
                (day,)).fetchone()
            used = row[0] if row is not None else 0
            if used + seconds > limit:
                raise TranslationError("QUOTA")
            db.execute(
                """INSERT INTO translation_quota(day,asr_seconds,mt_jobs,updated)
                   VALUES(?,?,0,?)
                   ON CONFLICT(day) DO UPDATE SET
                       asr_seconds=asr_seconds+excluded.asr_seconds,
                       updated=excluded.updated""",
                (day, seconds, now))
            db.execute("DELETE FROM translation_quota WHERE day < ?",
                       (day - 8,))
            return used + seconds

    def reserve_mt(self, day, limit):
        """Atomically reserve one billed MT job for one UTC day."""
        day = self._quota_day(day)
        if type(limit) is not int or not 1 <= limit <= 100000:
            raise TranslationError("INVALID_CONFIG")
        now = int(self.clock())
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute(
                "SELECT mt_jobs FROM translation_quota WHERE day=?",
                (day,)).fetchone()
            used = row[0] if row is not None else 0
            if used + 1 > limit:
                raise TranslationError("QUOTA")
            db.execute(
                """INSERT INTO translation_quota(day,asr_seconds,mt_jobs,updated)
                   VALUES(?,0,1,?)
                   ON CONFLICT(day) DO UPDATE SET
                       mt_jobs=mt_jobs+1,
                       updated=excluded.updated""",
                (day, now))
            db.execute("DELETE FROM translation_quota WHERE day < ?",
                       (day - 8,))
            return used + 1

    def _next_generation(self, db):
        row = db.execute("SELECT next_generation FROM translation_meta "
                         "WHERE id=1").fetchone()
        generation = row[0]
        db.execute("UPDATE translation_meta SET next_generation=? WHERE id=1",
                   (generation + 1,))
        return generation

    def admit(self, request, payload_hash, job_id, now):
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            old = db.execute("SELECT * FROM translation_jobs WHERE operation_id=?",
                             (request["operation_id"],)).fetchone()
            if old is not None:
                if old["payload_hash"] != payload_hash:
                    raise TranslationError("OPERATION_CONFLICT")
                return dict(old)
            if db.execute("SELECT COUNT(*) FROM translation_jobs"
                          ).fetchone()[0] >= self.capacity:
                raise TranslationError("BUSY")
            generation = self._next_generation(db)
            db.execute("""INSERT INTO translation_jobs(
                    operation_id,payload_hash,job_id,source_lang,target_lang,
                    audio_sha256,audio_bytes,generation,state,stage,created,updated)
                    VALUES(?,?,?,?,?,?,?,?,'ADMITTED','NONE',?,?)""",
                       (request["operation_id"], payload_hash, job_id,
                        request["source_lang"], request["target_lang"],
                        request["audio_sha256"], request["audio_bytes"],
                        generation, now, now))
            return self._row(db.execute(
                "SELECT * FROM translation_jobs WHERE operation_id=?",
                (request["operation_id"],)).fetchone())

    def claim(self, operation_id, stage, generation):
        if stage not in {"ASR", "MT"}:
            raise TranslationError("INVALID_STAGE")
        now = int(self.clock())
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT * FROM translation_jobs WHERE operation_id=?",
                             (operation_id,)).fetchone()
            if row is None:
                raise TranslationError("NOT_FOUND")
            if row["generation"] != generation:
                raise TranslationError("GENERATION_MISMATCH")
            if row["state"] in TERMINAL:
                raise TranslationError("STATE")
            if stage == "ASR":
                if row["asr_claimed"]:
                    raise TranslationError("ALREADY_CLAIMED")
                if row["state"] != "ADMITTED":
                    raise TranslationError("STATE")
                db.execute("""UPDATE translation_jobs
                              SET asr_claimed=1,state='RUNNING',stage='ASR',updated=?
                              WHERE operation_id=?""",
                           (now, operation_id))
            else:
                if row["mt_claimed"]:
                    raise TranslationError("ALREADY_CLAIMED")
                if not row["asr_claimed"] or row["stage"] != "ASR_DONE":
                    raise TranslationError("STAGE_ORDER")
                db.execute("""UPDATE translation_jobs
                              SET mt_claimed=1,state='RUNNING',stage='MT',updated=?
                              WHERE operation_id=?""",
                           (now, operation_id))
            return self._row(db.execute(
                "SELECT * FROM translation_jobs WHERE operation_id=?",
                (operation_id,)).fetchone())

    def complete_asr(self, operation_id, generation, source_hash, source_bytes):
        now = int(self.clock())
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT * FROM translation_jobs WHERE operation_id=?",
                             (operation_id,)).fetchone()
            if row is None:
                raise TranslationError("NOT_FOUND")
            if row["generation"] != generation:
                raise TranslationError("GENERATION_MISMATCH")
            if row["state"] != "RUNNING" or row["stage"] != "ASR":
                raise TranslationError("STATE")
            db.execute("""UPDATE translation_jobs
                          SET stage='ASR_DONE',source_hash=?,source_bytes=?,updated=?
                          WHERE operation_id=?""",
                       (source_hash, source_bytes, now, operation_id))
            return self._row(db.execute(
                "SELECT * FROM translation_jobs WHERE operation_id=?",
                (operation_id,)).fetchone())

    def complete_mt(self, operation_id, generation, target_hash, target_bytes):
        now = int(self.clock())
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT * FROM translation_jobs WHERE operation_id=?",
                             (operation_id,)).fetchone()
            if row is None:
                raise TranslationError("NOT_FOUND")
            if row["generation"] != generation:
                raise TranslationError("GENERATION_MISMATCH")
            if row["state"] != "RUNNING" or row["stage"] != "MT":
                raise TranslationError("STATE")
            db.execute("""UPDATE translation_jobs
                          SET state='DONE',stage='MT_DONE',target_hash=?,
                              target_bytes=?,updated=?
                          WHERE operation_id=?""",
                       (target_hash, target_bytes, now, operation_id))
            return self._row(db.execute(
                "SELECT * FROM translation_jobs WHERE operation_id=?",
                (operation_id,)).fetchone())

    def fail(self, operation_id, code, generation):
        if type(code) is not str or not ERROR_PATTERN.fullmatch(code):
            raise TranslationError("INVALID_ERROR")
        now = int(self.clock())
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT * FROM translation_jobs WHERE operation_id=?",
                             (operation_id,)).fetchone()
            if row is None:
                raise TranslationError("NOT_FOUND")
            if row["generation"] != generation:
                raise TranslationError("GENERATION_MISMATCH")
            if row["state"] in TERMINAL:
                return dict(row)
            db.execute("""UPDATE translation_jobs
                          SET state='FAILED',error=?,updated=?
                          WHERE operation_id=?""",
                       (code, now, operation_id))
            return self._row(db.execute(
                "SELECT * FROM translation_jobs WHERE operation_id=?",
                (operation_id,)).fetchone())

    def expire_audio(self, operation_id, generation, now=None):
        """Terminalize one unclaimed admission whose queued WAV is gone."""
        if type(generation) is not int:
            raise TranslationError("INVALID_CONFIG")
        now = int(self.clock()) if now is None else int(now)
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT * FROM translation_jobs WHERE operation_id=?",
                             (operation_id,)).fetchone()
            if row is None:
                return None
            if (row["generation"] != generation or row["state"] != "ADMITTED"
                    or row["asr_claimed"]):
                return dict(row)
            db.execute("""UPDATE translation_jobs
                          SET state='EXPIRED',error='AUDIO_EXPIRED',updated=?
                          WHERE operation_id=?""",
                       (now, operation_id))
            return self._row(db.execute(
                "SELECT * FROM translation_jobs WHERE operation_id=?",
                (operation_id,)).fetchone())

    def expire_stale_admissions(self, cutoff, now=None):
        """Expire unclaimed admissions older than the audio lifetime."""
        if type(cutoff) is not int:
            raise TranslationError("INVALID_CONFIG")
        now = int(self.clock()) if now is None else int(now)
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            cursor = db.execute(
                """UPDATE translation_jobs
                   SET state='EXPIRED',error='AUDIO_EXPIRED',updated=?
                   WHERE state='ADMITTED' AND asr_claimed=0 AND updated<=?""",
                (now, cutoff))
            db.execute("DELETE FROM translation_quota WHERE day < ?",
                       (now // 86400 - 8,))
            return cursor.rowcount

    def touch_admitted(self, operation_id, now=None):
        """Record the audio-enqueue time for the admission expiry sweep."""
        now = int(self.clock()) if now is None else int(now)
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            cursor = db.execute(
                """UPDATE translation_jobs SET updated=?
                   WHERE operation_id=? AND state='ADMITTED'
                     AND asr_claimed=0""",
                (now, operation_id))
            return cursor.rowcount

    def shutdown_cancel(self, now=None):
        """Cancel every pending/active job so no later paid stage can start."""
        now = int(self.clock()) if now is None else int(now)
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            cursor = db.execute(
                """UPDATE translation_jobs
                   SET state='CANCELLED',generation=generation+1,
                       source_hash=NULL,target_hash=NULL,source_bytes=0,
                       target_bytes=0,error='SHUTDOWN',updated=?
                   WHERE state IN ('ADMITTED','RUNNING')""",
                (now,))
            return cursor.rowcount

    def cancel(self, operation_id, generation=None):
        now = int(self.clock())
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT * FROM translation_jobs WHERE operation_id=?",
                             (operation_id,)).fetchone()
            if row is None:
                raise TranslationError("NOT_FOUND")
            if row["state"] == "CANCELLED":
                return dict(row)
            if generation is not None and row["generation"] != generation:
                raise TranslationError("GENERATION_MISMATCH")
            if row["state"] in TERMINAL:
                raise TranslationError("STATE")
            db.execute("""UPDATE translation_jobs
                          SET state='CANCELLED',generation=generation+1,
                              source_hash=NULL,target_hash=NULL,
                              source_bytes=0,target_bytes=0,error=NULL,updated=?
                          WHERE operation_id=?""",
                       (now, operation_id))
            return self._row(db.execute(
                "SELECT * FROM translation_jobs WHERE operation_id=?",
                (operation_id,)).fetchone())

    def cancel_by_job(self, job_id, generation=None):
        row = self.row_by_job(job_id)
        if row is None:
            raise TranslationError("NOT_FOUND")
        return self.cancel(row["operation_id"], generation)

    def recover(self):
        """A claimed stage cannot be replayed automatically after restart."""
        now = int(self.clock())
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            cursor = db.execute(
                "UPDATE translation_jobs SET state='UNKNOWN',updated=? "
                "WHERE state='RUNNING'", (now,))
            return cursor.rowcount

