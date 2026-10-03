"""SQLite persistence for the D1a companion server.

Contract rules implemented here (passport-travel-v1-draft):
- Stable logical record identity: one record_id per
  (account, kind, context_id, target_id) preassigned before events (DR01).
- Immutable change log in commit order; pull cursors are opaque
  strings with an epoch; tombstones included (DR03).
- Event deduplication by (account, device_id, event_id): identical retries
  return the original result; reused IDs with different content rejected.
- Waiter marker: `backup` watermark covers events, not wall-clock.
- Every write that future HTTP responses depend on happens inside one
  transaction committed before any acknowledgement is returned.
"""
from __future__ import annotations

import os
import secrets
import sqlite3
import threading
from datetime import datetime, timezone

from .errors import ApiError

DB_SCHEMA_VERSION = 1


def utcnow_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


class Database:
    """Thin SQLite wrapper; all mutations go through `_tx` for durability."""

    def __init__(self, path: str) -> None:
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        self._lock = threading.RLock()
        self._active: "Database._Tx | None" = None
        self._conn = sqlite3.connect(
            path, check_same_thread=False, isolation_level=None,
            timeout=30, uri=path.startswith("file:"),
        )
        self._conn.row_factory = sqlite3.Row
        self._conn.execute("PRAGMA journal_mode=WAL")
        self._conn.execute("PRAGMA foreign_keys=ON")
        self._conn.execute("PRAGMA synchronous=FULL")
        self._bootstrap_db(path)

    # -- schema ----------------------------------------------------------
    def _bootstrap_db(self, path: str) -> None:
        with self._tx() as tx:
            tx.cur.execute("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)")
            row = tx.cur.execute("SELECT value FROM meta WHERE key='schema_version'").fetchone()
            if row is None:
                tx.cur.execute(
                    "INSERT INTO meta(key, value) VALUES ('schema_version', ?)",
                    (str(DB_SCHEMA_VERSION),),
                )
                self._create_tables(tx.cur)
            elif int(row[0]) != DB_SCHEMA_VERSION:
                raise ApiError("SCHEMA_UNSUPPORTED", "database schema mismatch, restore may be required")
            else:
                self._create_tables(tx.cur, if_not_exists_only=True)

    def _create_tables(self, cur: sqlite3.Cursor, if_not_exists_only: bool = False) -> None:
        ifnot = "IF NOT EXISTS " if if_not_exists_only else ""
        cur.execute(f"CREATE TABLE {ifnot}accounts ("
                    "  account_id TEXT PRIMARY KEY,"
                    "  label TEXT NOT NULL,"
                    "  auth_salt TEXT NOT NULL,"
                    "  auth_digest TEXT NOT NULL,"
                    "  created_at TEXT NOT NULL)")
        cur.execute(f"CREATE TABLE {ifnot}devices ("
                    "  device_id TEXT PRIMARY KEY,"
                    "  account_id TEXT NOT NULL REFERENCES accounts(account_id),"
                    "  display_label TEXT NOT NULL,"
                    "  token_digest TEXT NOT NULL UNIQUE,"
                    "  status TEXT NOT NULL CHECK (status IN ('active','revoked')),"
                    "  bound_at TEXT NOT NULL,"
                    "  revoked_at TEXT)")
        cur.execute(f"CREATE TABLE {ifnot}binding_sessions ("
                    "  binding_id TEXT PRIMARY KEY,"
                    "  code_hash TEXT NOT NULL,"
                    "  account_id TEXT NOT NULL,"
                    "  created_at TEXT NOT NULL,"
                    "  expires_at TEXT NOT NULL,"
                    "  attempts INTEGER NOT NULL DEFAULT 0,"
                    "  state TEXT NOT NULL CHECK (state IN ('pending','exchanged','confirmed','consumed','expired')),"
                    "  display_identifier TEXT,"
                    "  exchange_token_hash TEXT)")
        cur.execute(f"CREATE TABLE {ifnot}packs ("
                    "  pack_id TEXT NOT NULL,"
                    "  revision INTEGER NOT NULL,"
                    "  account_id TEXT NOT NULL,"
                    "  kind TEXT NOT NULL,"
                    "  fixture INTEGER NOT NULL,"
                    "  label TEXT NOT NULL,"
                    "  total_bytes INTEGER NOT NULL,"
                    "  manifest_sha256 TEXT NOT NULL,"
                    "  manifest_bytes BLOB NOT NULL,"
                    "  storage_subdir TEXT NOT NULL,"
                    "  published_at TEXT NOT NULL,"
                    "  PRIMARY KEY (pack_id, revision))")
        cur.execute(f"CREATE TABLE {ifnot}record_map ("
                    "  account_id TEXT NOT NULL,"
                    "  kind TEXT NOT NULL,"
                    "  context_id TEXT,"
                    "  target_id TEXT NOT NULL,"
                    "  record_id TEXT NOT NULL PRIMARY KEY,"
                    "  UNIQUE (account_id, kind, context_id, target_id))")
        cur.execute(f"CREATE TABLE {ifnot}records ("
                    "  record_id TEXT PRIMARY KEY,"
                    "  account_id TEXT NOT NULL,"
                    "  kind TEXT NOT NULL,"
                    "  context_id TEXT,"
                    "  target_id TEXT NOT NULL,"
                    "  revision INTEGER NOT NULL,"
                    "  state TEXT,"
                    "  tombstone INTEGER NOT NULL DEFAULT 0,"
                    "  updated_by TEXT,"
                    "  updated_at TEXT NOT NULL)")
        cur.execute(f"CREATE TABLE {ifnot}change_log ("
                    "  change_seq INTEGER PRIMARY KEY,"  # commit order, part of cursors
                    "  record_id TEXT NOT NULL,"
                    "  account_id TEXT NOT NULL,"
                    "  kind TEXT NOT NULL,"
                    "  context_id TEXT,"
                    "  target_id TEXT NOT NULL,"
                    "  revision INTEGER NOT NULL,"
                    "  state TEXT,"
                    "  tombstone INTEGER NOT NULL,"
                    "  updated_by TEXT,"
                    "  committed_at TEXT NOT NULL)")
        cur.execute(f"CREATE TABLE {ifnot}events ("
                    "  event_key TEXT PRIMARY KEY,"  # account_id + '\0' + device_id + '\0' + event_id
                    "  account_id TEXT NOT NULL,"
                    "  device_id TEXT NOT NULL,"
                    "  event_id TEXT NOT NULL,"
                    "  device_epoch TEXT NOT NULL,"
                    "  sequence INTEGER NOT NULL,"
                    "  result TEXT NOT NULL,"
                    "  result_revision INTEGER,"
                    "  request_digest TEXT NOT NULL,"
                    "  received_at TEXT NOT NULL)")
        cur.execute(f"CREATE TABLE {ifnot}event_deps ("
                    "  event_key TEXT PRIMARY KEY,"
                    "  predecessor_key TEXT NOT NULL,"
                    "  resolved INTEGER NOT NULL DEFAULT 0)")
        cur.execute(f"CREATE TABLE {ifnot}device_provision ("
                    "  binding_id TEXT PRIMARY KEY,"
                    "  device_id TEXT NOT NULL UNIQUE,"
                    "  token_digest TEXT UNIQUE,"
                    "  token_recoverable TEXT NOT NULL,"
                    "  created_at TEXT NOT NULL)")
        cur.execute(f"CREATE TABLE {ifnot}upload_jobs ("
                    "  job_id TEXT PRIMARY KEY,"
                    "  account_id TEXT NOT NULL,"
                    "  kind TEXT NOT NULL,"  # resource | snapshot
                    "  target_path TEXT NOT NULL,"
                    "  ref_key TEXT NOT NULL,"
                    "  state TEXT NOT NULL CHECK (state IN ('queued','uploading','done','failed')),"
                    "  attempts INTEGER NOT NULL DEFAULT 0,"
                    "  error_code TEXT,"
                    "  next_retry_at TEXT,"
                    "  created_at TEXT NOT NULL,"
                    "  updated_at TEXT NOT NULL)")
        cur.execute(f"CREATE TABLE {ifnot}snapshots ("
                    "  snapshot_id TEXT PRIMARY KEY,"
                    "  account_id TEXT NOT NULL,"
                    "  created_at TEXT NOT NULL,"
                    "  included_cursor TEXT NOT NULL,"
                    "  state TEXT NOT NULL CHECK (state IN ('building','complete','failed')),"
                    "  dav_dir TEXT NOT NULL,"
                    "  manifest_sha256 TEXT,"
                    "  records_sha256 TEXT)")
        if not if_not_exists_only:
            self._set_meta(cur, "server_epoch", "srv-" + secrets.token_hex(8))
            self._set_meta(cur, "restore_bootstrap_required", "1")

    # -- plumbing --------------------------------------------------------
    class _Tx:
        def __init__(self, conn: sqlite3.Connection, lock: threading.RLock, db=None) -> None:
            self._conn = conn
            self._lock = lock
            self.cur = None
            self._depth = 0
            self._db = db

        def __enter__(self) -> "Database._Tx":
            self._lock.acquire()
            self._depth += 1
            if self._depth == 1:
                self._conn.execute("BEGIN IMMEDIATE")
            self.cur = self._conn.cursor()
            return self

        def __exit__(self, exc_type, exc, tb) -> bool:
            self._depth -= 1
            if self._depth == 0:
                if exc_type is None:
                    self._conn.commit()
                else:
                    self._conn.rollback()
                self._db._active = None
            self._lock.release()
            return False

    def _tx(self) -> "_Tx":
        if self._active is not None:
            return Database._Nested(self._active)
        tx = Database._Tx(self._conn, self._lock)
        tx._db = self
        self._active = tx
        return tx

    class _Nested:
        """Joins the active outermost transaction; reads share its cursor."""
        def __init__(self, active) -> None:
            self._active = active

        def __enter__(self):
            return self._active.__enter__()

        def __exit__(self, exc_type, exc, tb):
            self._active._lock.release()
            return False

    def tx(self) -> "_Tx":
        return self._tx()

    def _set_meta(self, cur: sqlite3.Cursor, key: str, value: str) -> None:
        cur.execute("INSERT INTO meta(key, value) VALUES (?,?) "
                    "ON CONFLICT(key) DO UPDATE SET value=excluded.value", (key, value))

    def get_meta(self, key: str, default: str | None = None) -> str | None:
        with self._tx() as tx:
            row = tx.cur.execute("SELECT value FROM meta WHERE key=?", (key,)).fetchone()
        return row[0] if row else default

    def set_meta_now(self, key: str, value: str) -> None:
        with self._tx() as tx:
            self._set_meta(tx.cur, key, value)

    def new_epoch(self) -> str:
        epoch = "srv-" + secrets.token_hex(8)
        self.set_meta_now("server_epoch", epoch)
        return epoch

    # -- records and change log ------------------------------------------
    def ensure_record_id(self, account_id: str, kind: str,
                         context_id: str | None, target_id: str) -> str:
        with self._tx() as tx:
            cur = tx.cur
            cur.execute(
                "SELECT record_id FROM record_map WHERE account_id=? AND kind=? "
                "AND context_id IS ? AND target_id=?",
                (account_id, kind, context_id, target_id))
            row = cur.fetchone()
            if row:
                return row[0]
            record_id = "r" + secrets.token_hex(10)
            cur.execute(
                "INSERT INTO record_map(account_id, kind, context_id, target_id, record_id) "
                "VALUES (?,?,?,?,?)", (account_id, kind, context_id, target_id, record_id))
            return record_id

    def get_record(self, record_id: str) -> sqlite3.Row | None:
        with self._tx() as tx:
            row = tx.cur.execute("SELECT * FROM records WHERE record_id=?", (record_id,)).fetchone()
        return row

    def commit_record_in(self, cur: sqlite3.Cursor, record_id: str, revision: int,
                         state: str | None, updated_by: str, tombstone: bool) -> None:
        row = cur.execute("SELECT * FROM records WHERE record_id=?", (record_id,)).fetchone()
        now = utcnow_iso()
        if row is None:
            info0 = cur.execute(
                "SELECT account_id, kind, context_id, target_id FROM record_map WHERE record_id=?",
                (record_id,)).fetchone()
            cur.execute(
                "INSERT INTO records(record_id, account_id, kind, context_id, target_id,"
                " revision, state, tombstone, updated_by, updated_at)"
                " VALUES (?,?,?,?,?,?,?,?,?,?)",
                (record_id, info0["account_id"], info0["kind"], info0["context_id"],
                 info0["target_id"], revision, state, int(tombstone), updated_by, now))
        else:
            cur.execute(
                "UPDATE records SET account_id=?, kind=?, context_id=?, target_id=?,"
                " revision=?, state=?, tombstone=?, updated_by=?, updated_at=? WHERE record_id=?",
                (row["account_id"], row["kind"], row["context_id"], row["target_id"],
                 revision, state, int(tombstone), updated_by, now, record_id))
        info = cur.execute(
            "SELECT account_id, kind, context_id, target_id FROM record_map WHERE record_id=?",
            (record_id,)).fetchone()
        cur.execute(
            "INSERT INTO change_log(change_seq, record_id, account_id, kind, context_id,"
            " target_id, revision, state, tombstone, updated_by, committed_at)"
            " VALUES ((SELECT COALESCE(MAX(change_seq),0)+1 FROM change_log),?,?,?,?,?,?,?,?,?,?)",
            (record_id, info["account_id"], info["kind"], info["context_id"], info["target_id"],
             revision, state, int(tombstone), updated_by, now))
