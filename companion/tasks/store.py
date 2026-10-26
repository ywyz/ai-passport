"""Transactional bounded snapshots. Failed refreshes never replace good data."""

import hashlib
import json
from contextlib import contextmanager
import os
from pathlib import Path
import sqlite3
import time
import uuid
import random

from .common import (MAX_CACHE_BYTES, MAX_COLLECTION_BYTES, MAX_COLLECTIONS,
                     MAX_ITEMS, MAX_ORIGINAL_BYTES, MAX_PAGE_BYTES, MAX_RECORD_BYTES,
                     STALE_SECONDS, TaskError, bounded, packed)


def device_revision(source_revision):
    # A view change must invalidate cursors issued by the former all-task API.
    return hashlib.sha256(("active-tasks-v2:" + source_revision).encode()).hexdigest()[:32]


class Store:
    def __init__(self, path, account_id, clock=time.time):
        self.path = Path(path)
        self.clock = clock
        self.path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        if self.path.is_symlink():
            raise TaskError("INVALID_CONFIG")
        # Reserve with restrictive mode before sqlite writes task data.
        fd = os.open(self.path, os.O_CREAT | os.O_RDWR, 0o600)
        os.close(fd)
        if os.name != "nt" and self.path.stat().st_mode & 0o077:
            raise TaskError("PRIVATE_FILE_REQUIRED")
        with self.connect() as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS state (
                    id INTEGER PRIMARY KEY CHECK(id=1), account TEXT NOT NULL,
                    collections TEXT NOT NULL DEFAULT '[]',
                    selection TEXT NOT NULL DEFAULT '[]', selection_revision INTEGER NOT NULL DEFAULT 0,
                    snapshot TEXT, snapshot_selection INTEGER, revision TEXT, downloaded INTEGER,
                    last_error TEXT, auth_halted INTEGER NOT NULL DEFAULT 0);
                CREATE TABLE IF NOT EXISTS originals (
                    href TEXT PRIMARY KEY, etag TEXT NOT NULL, body BLOB NOT NULL);
                CREATE TABLE IF NOT EXISTS completions (
                    operation_id TEXT PRIMARY KEY, payload_hash TEXT NOT NULL,
                    task_id TEXT NOT NULL, base_etag TEXT NOT NULL, intent TEXT NOT NULL,
                    record TEXT NOT NULL, state TEXT NOT NULL, created INTEGER NOT NULL,
                    updated INTEGER NOT NULL, attempts INTEGER NOT NULL DEFAULT 0,
                    due INTEGER NOT NULL, lease INTEGER NOT NULL DEFAULT 0,
                    submitted_sha TEXT, error TEXT);
            """)
            db.execute("INSERT OR IGNORE INTO state(id,account) VALUES(1,?)", (account_id,))
            if db.execute("SELECT account FROM state WHERE id=1").fetchone()[0] != account_id:
                raise TaskError("ACCOUNT_MISMATCH")

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

    def state(self):
        with self.connect() as db:
            return dict(db.execute("SELECT * FROM state WHERE id=1").fetchone())

    def set_collections(self, collections):
        if len(collections) > MAX_COLLECTIONS:
            raise TaskError("LIMIT_EXCEEDED")
        value = bounded(collections, MAX_COLLECTION_BYTES - MAX_COLLECTIONS * 68 - 256).decode("utf-8")
        with self.connect() as db:
            db.execute("UPDATE state SET collections=? WHERE id=1", (value,))

    def select(self, ids, revision):
        if (not isinstance(ids, list) or len(ids) > MAX_COLLECTIONS
                or any(not isinstance(i, str) for i in ids) or len(set(ids)) != len(ids)
                or type(revision) is not int or revision < 0):
            raise TaskError("INVALID_SELECTION")
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            state = db.execute("SELECT * FROM state WHERE id=1").fetchone()
            if revision != state["selection_revision"]:
                raise TaskError("REVISION_CONFLICT")
            known = {c["id"] for c in json.loads(state["collections"])}
            if not set(ids) <= known:
                raise TaskError("UNKNOWN_COLLECTION")
            db.execute("UPDATE state SET selection=?, selection_revision=selection_revision+1 WHERE id=1",
                       (packed(sorted(ids)).decode(),))
        return revision + 1

    def publish(self, records, originals, selection_revision):
        revision, downloaded = uuid.uuid4().hex, int(self.clock())
        if len(records) > MAX_ITEMS or sum(len(raw) for _, _, raw in originals) > MAX_ORIGINAL_BYTES:
            raise TaskError("LIMIT_EXCEEDED")
        items = [dict(r, snapshot_revision=revision, downloaded_at=downloaded) for r in records]
        for record in items:
            bounded(record, MAX_RECORD_BYTES)
        payload = bounded(items, MAX_CACHE_BYTES).decode("utf-8")
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            state = db.execute("SELECT selection_revision FROM state WHERE id=1").fetchone()
            if state[0] != selection_revision:
                raise TaskError("REVISION_CONFLICT")
            db.execute("DELETE FROM originals")
            db.executemany("INSERT INTO originals(href,etag,body) VALUES(?,?,?)", originals)
            db.execute("""UPDATE state SET snapshot=?, snapshot_selection=?, revision=?, downloaded=?,
                          last_error=NULL, auth_halted=0 WHERE id=1""",
                       (payload, selection_revision, revision, downloaded))
        return device_revision(revision)

    def failure(self, code):
        with self.connect() as db:
            db.execute("UPDATE state SET last_error=?, auth_halted=MAX(auth_halted,?) WHERE id=1",
                       (code, int(code == "AUTH_FAILED")))

    def resume_auth(self):
        # Explicit operator action only, after privately repairing credentials.
        with self.connect() as db:
            db.execute("UPDATE state SET auth_halted=0 WHERE id=1")
            db.execute("UPDATE completions SET state='SERVER_DURABLE',attempts=0,due=?,lease=0,error=NULL WHERE state='AUTH_FAILED'",(int(self.clock()),))
            if db.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name='creations'").fetchone():
                db.execute("UPDATE creations SET state='SERVER_DURABLE',attempts=0,due=?,lease=0,error=NULL WHERE state='AUTH_FAILED'",(int(self.clock()),))

    def collections(self):
        state = self.state()
        result = {"schema": 1, "collections": json.loads(state["collections"]),
                  "selected": json.loads(state["selection"]),
                  "selection_revision": state["selection_revision"]}
        bounded(result, MAX_COLLECTION_BYTES)
        return result

    def page(self, revision=None, offset=0):
        from .model import task_id
        state = self.state()
        if state["snapshot"] is None or state["snapshot_selection"] != state["selection_revision"]:
            raise TaskError("SNAPSHOT_NOT_READY")
        view_revision = device_revision(state["revision"])
        if revision is not None and revision != view_revision:
            raise TaskError("REVISION_CONFLICT")
        # Keep complete history and original calendars in storage. Only the
        # device-facing view excludes terminal tasks, before counting/paging.
        records = [dict(record, snapshot_revision=view_revision, task_id=task_id(record),
                        completion_allowed=bool(record.get("completion_allowed")))
                   for record in json.loads(state["snapshot"])
                   if record["status"] in {"NEEDS-ACTION", "IN-PROCESS"}]
        if type(offset) is not int or offset < 0 or offset > len(records) or (offset and revision is None):
            raise TaskError("INVALID_CURSOR")
        stale = int(self.clock()) - state["downloaded"] >= STALE_SECONDS
        status = "AUTH_FAILED" if state["auth_halted"] else (
            "STALE" if stale or state["last_error"] else ("READY" if records else "EMPTY"))
        result = {"schema": 1, "snapshot_revision": view_revision,
                  "selection_revision": state["snapshot_selection"],
                  "downloaded_at": state["downloaded"], "state": status,
                  "last_error": state["last_error"], "read_only": True,
                  "total": len(records), "offset": offset, "next_offset": None, "items": []}
        for index in range(offset, len(records)):
            result["items"].append(records[index])
            result["next_offset"] = index + 1 if index + 1 < len(records) else None
            if len(packed(result)) > MAX_PAGE_BYTES:
                result["items"].pop()
                result["next_offset"] = index
                break
        bounded(result, MAX_PAGE_BYTES)
        if offset < len(records) and not result["items"]:
            raise TaskError("LIMIT_EXCEEDED")
        return result

    @staticmethod
    def _receipt(row):
        return {"schema":1,"operation_id":row["operation_id"],"task_id":row["task_id"],
                "state":row["state"],"error":row["error"],"updated_at":row["updated"]}

    def operation(self, identifier):
        with self.connect() as db:
            row=db.execute("SELECT * FROM completions WHERE operation_id=?",(identifier,)).fetchone()
            if row is None: raise TaskError("NOT_FOUND")
            return self._receipt(row)

    def accept_completion(self, intent, retry=False):
        from .completion import validate_intent
        from .model import task_id
        validate_intent(intent)
        now=int(self.clock())
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            old=db.execute("SELECT * FROM completions WHERE operation_id=?",(intent["operation_id"],)).fetchone()
            if old is not None:
                if old["payload_hash"]!=intent["payload_hash"]: raise TaskError("OPERATION_CONFLICT")
                if retry and old["state"]=="RETRY_PAUSED":
                    db.execute("UPDATE completions SET state='SERVER_DURABLE',attempts=0,due=?,lease=0,error=NULL,updated=? WHERE operation_id=?",
                               (now,now,intent["operation_id"]))
                    old=db.execute("SELECT * FROM completions WHERE operation_id=?",(intent["operation_id"],)).fetchone()
                return self._receipt(old)
            if (db.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name='creations'").fetchone() and
                    db.execute("SELECT 1 FROM creations WHERE operation_id=?",(intent["operation_id"],)).fetchone()):
                raise TaskError("OPERATION_CONFLICT")
            if db.execute("SELECT COUNT(*) FROM completions").fetchone()[0]>=1024:
                raise TaskError("QUEUE_FULL")
            state=db.execute("SELECT * FROM state WHERE id=1").fetchone()
            if state["snapshot"] is None or state["snapshot_selection"]!=state["selection_revision"]:
                raise TaskError("SNAPSHOT_NOT_READY")
            record=next((r for r in json.loads(state["snapshot"]) if task_id(r)==intent["task_id"]),None)
            if record is None: raise TaskError("NOT_FOUND")
            if record["etag"]!=intent["base_etag"]: raise TaskError("CONFLICT")
            if not record.get("completion_allowed") or record["recurring"] or record["has_parent"]:
                raise TaskError("READ_ONLY")
            db.execute("""INSERT INTO completions(operation_id,payload_hash,task_id,base_etag,intent,record,state,created,updated,due)
                          VALUES(?,?,?,?,?,?,'SERVER_DURABLE',?,?,?)""",
                       (intent["operation_id"],intent["payload_hash"],intent["task_id"],intent["base_etag"],
                        packed(intent).decode(),packed(record).decode(),now,now,now))
            return self._receipt(db.execute("SELECT * FROM completions WHERE operation_id=?",(intent["operation_id"],)).fetchone())

    def claim_completion(self):
        now=int(self.clock())
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row=db.execute("""SELECT * FROM completions WHERE
                (state IN ('SERVER_DURABLE','RETRY_WAIT') AND due<=?) OR
                (state='REMOTE_APPLYING' AND lease<=?) ORDER BY created,operation_id LIMIT 1""",(now,now)).fetchone()
            if row is None: return None
            db.execute("UPDATE completions SET state='REMOTE_APPLYING',attempts=attempts+1,lease=?,updated=? WHERE operation_id=?",
                       (now+600,now,row["operation_id"]))
            result=dict(row); result["record"]=json.loads(result["record"])
            return result

    def submitting(self, identifier, digest):
        with self.connect() as db:
            db.execute("UPDATE completions SET submitted_sha=? WHERE operation_id=? AND state='REMOTE_APPLYING'",(digest,identifier))

    def completion_failed(self, identifier, code):
        terminal={"CONFLICT","NOT_FOUND","READ_ONLY","AUTH_FAILED"}
        with self.connect() as db:
            row=db.execute("SELECT attempts FROM completions WHERE operation_id=?",(identifier,)).fetchone()
            attempts=row[0]; now=int(self.clock())
            status=code if code in terminal else "RETRY_PAUSED" if attempts>=5 else "RETRY_WAIT"
            delay=int((2,5,15,60,300)[min(attempts-1,4)]*random.SystemRandom().uniform(.8,1.2))
            db.execute("UPDATE completions SET state=?,error=?,updated=?,due=?,lease=0 WHERE operation_id=?",
                       (status,code if code in terminal else "UNAVAILABLE",now,now+delay,identifier))
            if code=="AUTH_FAILED": db.execute("UPDATE state SET auth_halted=1,last_error='AUTH_FAILED' WHERE id=1")

    def completion_succeeded(self, identifier, records, href, etag, raw):
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            op=db.execute("SELECT * FROM completions WHERE operation_id=?",(identifier,)).fetchone()
            state=db.execute("SELECT * FROM state WHERE id=1").fetchone()
            existing=json.loads(state["snapshot"])
            resource=[r for r in existing if r["href"]==href]
            # A concurrent source refresh may already contain a newer edit.
            if resource and all(r["etag"] in {op["base_etag"],etag} for r in resource):
                revision=uuid.uuid4().hex
                items=[r for r in existing if r["href"]!=href]+[dict(r,completion_allowed=False) for r in records]
                from .model import sort_key
                items.sort(key=sort_key)
                items=[dict(r,snapshot_revision=revision,downloaded_at=state["downloaded"]) for r in items]
                for item in items: bounded(item,MAX_RECORD_BYTES)
                payload=bounded(items,MAX_CACHE_BYTES).decode()
                db.execute("UPDATE originals SET etag=?,body=? WHERE href=?",(etag,raw,href))
                db.execute("UPDATE state SET snapshot=?,revision=? WHERE id=1",(payload,revision))
            db.execute("UPDATE completions SET state='SUCCEEDED',error=NULL,updated=?,lease=0 WHERE operation_id=?",(int(self.clock()),identifier))
