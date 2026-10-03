"""Backup publication, WebDAV upload queue, restore (D1a).

Rules (contract 7-9, DR06):
- Working SQLite stays on a local volume; remote DAV holds immutable
  resources and complete snapshots only.
- Snapshot upload order: records.json, manifest.json, then complete.json
  LAST. A snapshot without its completion marker is never recoverable.
- included_cursor is a change watermark (change_seq-epoch), not a timestamp.
- Restore validates everything in an isolated staging database; the live
  swap is performed by the host process (see Main.restore_snapshot).
  Restored instances require fresh admin bootstrap (no revived auth),
  mark stored devices revoked (rebind required) and create a new epoch.
- Resource uploads are per-file queue jobs with durable state; temporary
  DAV failures reschedule after 60 s; permanent failures stay 'failed'.
"""
from __future__ import annotations

import base64
import json
import os
import secrets
import sqlite3
import threading
import datetime as dt
import time

from .core import ID_RE, Core, sha256_hex, utcnow_iso, valid_id
from .db import Database
from .errors import ApiError
from .packs import normalize_relpath

JOB_RETRY_SECONDS = 60
RESTORE_MAX_ROWS = 20000


# --------------------------------------------------------------- queues


def enqueue_resource_uploads(core: Core, pack_id: str, revision: int) -> None:
    with core.db.tx() as tx:
        rows = tx.cur.execute(
            "SELECT manifest_bytes, storage_subdir FROM packs WHERE pack_id=? AND revision=?",
            (pack_id, revision)).fetchone()
    if not rows:
        raise ApiError("NOT_FOUND", "pack missing")
    manifest = json.loads(bytes(rows["manifest_bytes"]).decode("utf-8"))
    with core.db.tx() as tx:
        for entry in manifest["files"]:
            file_job_id = "res-" + sha256_hex(
                f"{pack_id}/{revision}/{entry['file_id']}".encode())[:16]
            tx.cur.execute(
                "INSERT INTO upload_jobs(job_id, account_id, kind, target_path, ref_key,"
                " state, created_at, updated_at) VALUES (?,?,?,?,?, 'queued', ?, ?)"
                " ON CONFLICT(job_id) DO NOTHING",
                (file_job_id, core.account_id, "resource",
                 dav_resource_path(core, pack_id, revision) + entry["path"],
                 json.dumps({"pack_id": pack_id, "revision": revision,
                             "file_id": entry["file_id"]}),
                 utcnow_iso(), utcnow_iso()))
        manifest_job_id = "res-" + sha256_hex(
            f"{pack_id}/{revision}/manifest".encode())[:16]
        tx.cur.execute(
            "INSERT INTO upload_jobs(job_id, account_id, kind, target_path, ref_key,"
            " state, created_at, updated_at) VALUES (?,?,?,?,?, 'queued', ?, ?)"
            " ON CONFLICT(job_id) DO NOTHING",
            (manifest_job_id, core.account_id, "resource",
             dav_resource_path(core, pack_id, revision) + "manifest.json",
             json.dumps({"pack_id": pack_id, "revision": revision,
                         "file_id": "__manifest__"}),
             utcnow_iso(), utcnow_iso()))


def dav_resource_path(core: Core, pack_id: str, revision: int) -> str:
    return (f"{core.dav_prefix}/accounts/{core.account_id}/resources/"
            f"{pack_id}/{revision}/")


def dav_snapshot_path(core: Core, snapshot_id: str) -> str:
    return f"{core.dav_prefix}/accounts/{core.account_id}/snapshots/{snapshot_id}/"


def _dav(core: Core):
    if core.dav is None:
        raise ApiError("DAV_PENDING", "WebDAV not configured", retry_after_seconds=0)
    return core.dav


def backup_state_for_change(core: Core, max_change_seq: int) -> dict:
    """Device-visible backup state for current operations.

    Coverage refers to the change watermark the snapshot includes and the
    absence of queued uploads; never wall-clock timestamps.
    """
    with core.db.tx() as tx:
        snap = tx.cur.execute(
            "SELECT * FROM snapshots WHERE account_id=? AND state='complete'"
            " ORDER BY created_at DESC LIMIT 1", (core.account_id,)).fetchone()
        pending = tx.cur.execute(
            "SELECT COUNT(*) FROM upload_jobs WHERE state IN ('queued','uploading')",
        ).fetchone()[0]
        max_seq = tx.cur.execute("SELECT COALESCE(MAX(change_seq),0) FROM change_log").fetchone()[0]
    if not snap:
        return {"state": "pending", "included_cursor": None, "snapshot_id": None}
    try:
        included = int(base64.b64decode(snap["included_cursor"]).decode().split("-", 1)[0])
    except (ValueError, TypeError):
        included = 0
    covered = max_seq <= included and pending == 0
    if covered:
        return {"state": "complete", "included_cursor": snap["included_cursor"],
                "snapshot_id": snap["snapshot_id"], "created_at": snap["created_at"]}
    return {"state": "pending", "included_cursor": snap["included_cursor"],
            "snapshot_id": snap["snapshot_id"], "created_at": snap["created_at"]}


# --------------------------------------------------------------- snapshots


def start_snapshot_job(core: Core) -> dict:
    snapshot_id = "snap-" + secrets.token_hex(8)
    with core.db.tx() as tx:
        cur = tx.cur
        max_seq = cur.execute(
            "SELECT COALESCE(MAX(change_seq),0) FROM change_log").fetchone()[0]
        included_cursor = base64.b64encode(
            f"{max_seq}-{core.server_epoch}".encode()).decode()
        cur.execute(
            "INSERT INTO snapshots(snapshot_id, account_id, created_at, included_cursor,"
            " state, dav_dir) VALUES (?,?,?,?, 'building', ?)",
            (snapshot_id, core.account_id, utcnow_iso(), included_cursor,
             dav_snapshot_path(core, snapshot_id)))
        cur.execute(
            "INSERT INTO upload_jobs(job_id, account_id, kind, target_path, ref_key,"
            " state, created_at, updated_at) VALUES (?,?,?,?,?, 'queued', ?, ?)",
            ("snap-" + snapshot_id, core.account_id, "snapshot",
             dav_snapshot_path(core, snapshot_id), snapshot_id,
             utcnow_iso(), utcnow_iso()))
    return {"snapshot_id": snapshot_id, "job_id": "snap-" + snapshot_id}


def build_snapshot_document(core: Core, snapshot_id: str) -> tuple[dict, str]:
    """Full snapshot export content; returns (document, records body json)."""
    with core.db.tx() as tx:
        cur = tx.cur
        snap = cur.execute("SELECT * FROM snapshots WHERE snapshot_id=?",
                           (snapshot_id,)).fetchone()
        if not snap:
            raise ApiError("NOT_FOUND", "snapshot missing")
        account = cur.execute(
            "SELECT account_id FROM accounts WHERE account_id=?",
            (core.account_id,)).fetchone()
        if not account:  # data account identity must travel with the snapshot
            raise ApiError("NOT_FOUND", "account identity missing")
        packs = [dict(r) for r in cur.execute(
            "SELECT pack_id, revision, kind, label, total_bytes, manifest_sha256"
            " FROM packs WHERE account_id=? ORDER BY pack_id, revision",
            (core.account_id,)).fetchall()]
        mappings = [dict(r) for r in cur.execute(
            "SELECT kind, context_id, target_id, record_id FROM record_map"
            " WHERE account_id=? ORDER BY record_id", (core.account_id,)).fetchall()]
        records = [dict(r) for r in cur.execute(
            "SELECT record_id, kind, context_id, target_id, revision, state, tombstone,"
            " updated_by, updated_at FROM records ORDER BY record_id").fetchall()]
        change_log = [dict(r) for r in cur.execute(
            "SELECT change_seq, record_id, account_id, kind, context_id, target_id,"
            " revision, state, tombstone, updated_by, committed_at FROM change_log"
            " ORDER BY change_seq").fetchall()]
        events_retained = [dict(r) for r in cur.execute(
            "SELECT event_key, account_id, device_id, event_id, device_epoch, sequence,"
            " result, result_revision, received_at FROM events ORDER BY received_at"
            " LIMIT 4096").fetchall()]
        devices = [dict(r) for r in cur.execute(
            "SELECT device_id, display_label, status, bound_at, revoked_at FROM devices"
            " WHERE account_id=?", (core.account_id,)).fetchall()]
    document = {
        "snapshot_id": snapshot_id,
        "schema_version": 1,
        "contract": "passport-travel-v1-draft",
        "created_at": utcnow_iso(),
        "included_cursor": snap["included_cursor"],
        "server_epoch_at_creation": core.server_epoch,
        "account_id": core.account_id,
        "packs": packs,
        "record_map": mappings,
        "records": records,
        "change_log": change_log,
        "events_retained": events_retained,
        "devices": devices,
    }
    return document, json.dumps(document, ensure_ascii=False, sort_keys=True)


def process_upload_job(core: Core, job_id: str) -> str:
    """Run one job synchronously; the worker wraps this with backoff."""
    with core.db.tx() as tx:
        job = tx.cur.execute("SELECT * FROM upload_jobs WHERE job_id=?",
                             (job_id,)).fetchone()
    if not job or job["state"] == "done":
        return job["state"] if job else "missing"
    client = _dav(core)
    try:
        if job["kind"] == "resource":
            ref = json.loads(job["ref_key"])
            payload = _resource_payload(core, ref["pack_id"], ref["revision"],
                                        ref["file_id"])
            client.mkdirs(job["target_path"].rsplit("/", 1)[0])
            client.put(job["target_path"], payload)
        elif job["kind"] == "snapshot":
            return _process_snapshot_job(core, job, client)
        else:
            raise ApiError("DAV_PENDING", "unknown job kind")
        set_job_done(core, job_id)
        return "done"
    except ApiError as err:
        set_job_error(core, job_id, err)
        raise


def _resource_payload(core: Core, pack_id: str, revision: int, file_id: str) -> bytes:
    with core.db.tx() as tx:
        row = tx.cur.execute(
            "SELECT manifest_bytes, storage_subdir FROM packs WHERE pack_id=?"
            " AND revision=?", (pack_id, revision)).fetchone()
    if not row:
        raise ApiError("NOT_FOUND", "pack missing during backup")
    manifest = json.loads(bytes(row["manifest_bytes"]).decode("utf-8"))
    subdir = row["storage_subdir"]
    root = os.path.realpath(os.path.join(core.data_dir, "resource_store", subdir))
    if file_id == "__manifest__":
        return bytes(row["manifest_bytes"])
    for entry in manifest["files"]:
        if entry["file_id"] == file_id:
            rel = normalize_relpath(entry["path"])
            path = os.path.realpath(os.path.join(root, *rel.split("/")))
            if os.path.commonpath([root, path]) != root:
                raise ApiError("BAD_REQUEST", "path escape rejected")
            with open(path, "rb") as handle:
                data = handle.read(entry["size_bytes"])
            if sha256_hex(data) != entry["sha256"]:
                raise ApiError("HASH_MISMATCH",
                               "local resource digest mismatch during backup")
            return data
    raise ApiError("NOT_FOUND", "file missing during backup")


def _process_snapshot_job(core: Core, job: sqlite3.Row, client) -> str:
    snapshot_id = job["ref_key"]
    document, body = build_snapshot_document(core, snapshot_id)
    body_bytes = body.encode("utf-8")
    manifest = {
        "snapshot_id": snapshot_id,
        "schema_version": 1,
        "records_sha256": sha256_hex(body_bytes),
        "records_size_bytes": len(body_bytes),
    }
    manifest_bytes = json.dumps(manifest, sort_keys=True).encode()
    complete = {
        "snapshot_id": snapshot_id,
        "records_sha256": sha256_hex(body_bytes),
        "manifest_sha256": sha256_hex(manifest_bytes),
        "included_cursor": document["included_cursor"],
        "finished_at": utcnow_iso(),
        "final_marker": True,
    }
    complete_bytes = json.dumps(complete, sort_keys=True).encode()
    dav_dir = job["target_path"]
    client.mkdirs(dav_dir)
    # Upload order is significant: complete.json LAST (final marker).
    client.put(dav_dir + "records.json", body_bytes)
    client.put(dav_dir + "manifest.json", manifest_bytes)
    client.put(dav_dir + "complete.json", complete_bytes)
    with core.db.tx() as tx:
        tx.cur.execute(
            "UPDATE snapshots SET state='complete', manifest_sha256=?, records_sha256=?"
            " WHERE snapshot_id=?", (sha256_hex(manifest_bytes),
                                     sha256_hex(body_bytes), snapshot_id))
    return "done"


def set_job_done(core: Core, job_id: str) -> None:
    with core.db.tx() as tx:
        tx.cur.execute("UPDATE upload_jobs SET state='done', updated_at=? WHERE job_id=?",
                       (utcnow_iso(), job_id))


def set_job_error(core: Core, job_id: str, err: ApiError) -> None:
    permanent = err.code != "DAV_PENDING"
    with core.db.tx() as tx:
        row = tx.cur.execute("SELECT attempts FROM upload_jobs WHERE job_id=?",
                             (job_id,)).fetchone()
        attempts = (row["attempts"] if row else 0) + 1
        if permanent:
            tx.cur.execute(
                "UPDATE upload_jobs SET state='failed', error_code=?, attempts=?,"
                " updated_at=? WHERE job_id=?",
                (err.code, attempts, utcnow_iso(), job_id))
        else:
            delay = err.retry_after_seconds or JOB_RETRY_SECONDS
            nxt = (dt.datetime.fromisoformat(utcnow_iso()) + dt.timedelta(seconds=delay))
            iso = nxt.strftime("%Y-%m-%dT%H:%M:%S")
            tx.cur.execute(
                "UPDATE upload_jobs SET state='queued', error_code=?, attempts=?,"
                " next_retry_at=?, updated_at=? WHERE job_id=?",
                (err.code, attempts, iso, utcnow_iso(), job_id))


def record_count(core: Core) -> int:
    with core.db.tx() as tx:
        return tx.cur.execute("SELECT COUNT(*) FROM records").fetchone()[0]


def list_snapshots(core: Core) -> dict:
    with core.db.tx() as tx:
        rows = tx.cur.execute(
            "SELECT snapshot_id, created_at, included_cursor, state, records_sha256"
            " FROM snapshots WHERE state='complete' ORDER BY created_at DESC LIMIT 20"
            ).fetchall()
    return {"complete": [dict(r) for r in rows]}


def list_jobs(core: Core) -> list[dict]:
    with core.db.tx() as tx:
        rows = tx.cur.execute(
            "SELECT job_id, kind, state, error_code, attempts, updated_at"
            " FROM upload_jobs ORDER BY updated_at DESC LIMIT 50").fetchall()
    return [dict(r) for r in rows]


# --------------------------------------------------------------- restore


def fetch_snapshot_document(core: Core, snapshot_id: str) -> tuple[dict, dict]:
    """Load records/manifest/complete from DAV and validate binding digests."""
    valid_id(snapshot_id, "snapshot_id")
    client = _dav(core)
    dav_dir = dav_snapshot_path(core, snapshot_id)
    try:
        complete = json.loads(client.get(dav_dir + "complete.json").decode("utf-8"))
        manifest = json.loads(client.get(dav_dir + "manifest.json").decode("utf-8"))
        records_body = client.get(dav_dir + "records.json")
    except ApiError:
        raise
    except Exception as exc:  # JSON decode or network via adapter
        raise ApiError("DAV_PENDING", f"snapshot fetch failed: {exc}") from exc
    if not complete.get("final_marker") or complete.get("snapshot_id") != snapshot_id:
        raise ApiError("HASH_MISMATCH", "snapshot completion marker invalid")
    if manifest.get("snapshot_id") != snapshot_id:
        raise ApiError("HASH_MISMATCH", "snapshot manifest identity mismatch")
    if sha256_hex(records_body) != manifest.get("records_sha256") \
            or sha256_hex(records_body) != complete.get("records_sha256"):
        raise ApiError("HASH_MISMATCH", "snapshot records digest mismatch")
    try:
        document = json.loads(records_body.decode("utf-8"))
    except ValueError as exc:
        raise ApiError("HASH_MISMATCH", "snapshot records not JSON") from exc
    if not isinstance(document, dict) or document.get("schema_version") != 1:
        raise ApiError("SCHEMA_UNSUPPORTED", "snapshot schema unsupported")
    return document, complete


def validate_snapshot_document(core: Core, document: dict) -> dict:
    """Structural checks; no side effects on the live database."""
    counts = {}
    mapping_ids: set[str] | None = None
    for section, minimal in (
        ("packs", {"pack_id", "revision"}),
        ("record_map", {"record_id", "target_id"}),
        ("records", {"record_id", "revision"}),
        ("change_log", {"change_seq", "record_id"}),
    ):
        rows = document.get(section)
        if not isinstance(rows, list):
            raise ApiError("HASH_MISMATCH", f"snapshot section {section} missing")
        if len(rows) > RESTORE_MAX_ROWS:
            raise ApiError("SIZE_LIMIT", f"snapshot section {section} too large")
        for row in rows:
            if not isinstance(row, dict) or not minimal.issubset(row.keys()):
                raise ApiError("HASH_MISMATCH", f"snapshot {section} row invalid")
        counts[section] = len(rows)
        if section == "record_map":
            mapping_ids = {row["record_id"] for row in rows}
    if mapping_ids is None:
        raise ApiError("HASH_MISMATCH", "snapshot record_map missing")
    for row in document["records"] + document["change_log"]:
        if row["record_id"] not in mapping_ids:
            raise ApiError("HASH_MISMATCH", "snapshot record without identity mapping")
    if not isinstance(document.get("account_id"), str) or not ID_RE.fullmatch(
            document["account_id"]):
        raise ApiError("HASH_MISMATCH", "snapshot account identity invalid")
    seqs = [row["change_seq"] for row in document["change_log"]]
    if seqs != sorted(set(seqs)) and seqs and min(seqs) >= 0:
        from .errors import ApiError as _A
        raise _A("HASH_MISMATCH", "change log not in commit order")
    device_ids = {row["device_id"] for row in document["devices"]}
    for event in document["events_retained"]:
        if event["device_id"] not in device_ids:
            raise ApiError("HASH_MISMATCH", "event references unknown device")
    return counts


def preview_restore(core: Core, snapshot_id: str) -> dict:
    document, _complete = fetch_snapshot_document(core, snapshot_id)
    counts = validate_snapshot_document(core, document)
    return {"snapshot_id": snapshot_id, "counts": counts,
            "preview": {"restore_would_create_new_epoch": True,
                        "devices_would_require_rebind": True,
                        "current_record_count": record_count(core),
                        "restore_allowed": record_count(core) == 0}}


def _snapshot_pack_manifests(core: Core, document: dict) -> list[tuple[str, int, bytes]]:
    manifests = []
    client = _dav(core)
    for pack in document["packs"]:
        dav_dir = (f"{core.dav_prefix}/accounts/{document['account_id']}/resources/"
                   f"{pack['pack_id']}/{pack['revision']}/")
        body = client.get(dav_dir + "manifest.json")
        manifest = json.loads(body.decode("utf-8"))
        if manifest.get("pack_id") != pack["pack_id"] or \
                manifest.get("revision") != pack["revision"]:
            raise ApiError("HASH_MISMATCH", "published manifest identity mismatch")
        if sha256_hex(body) != pack["manifest_sha256"]:
            raise ApiError("HASH_MISMATCH", "published manifest digest mismatch")
        manifests.append((pack["pack_id"], pack["revision"], body))
    return manifests


def _restore_resource_store(core: Core, document: dict) -> None:
    """Re-download published resource bodies referenced by the snapshot."""
    client = _dav(core)
    aid = document["account_id"]
    for pack in document["packs"]:
        dav_dir = (f"{core.dav_prefix}/accounts/{aid}/resources/"
                   f"{pack['pack_id']}/{pack['revision']}/")
        body = client.get(dav_dir + "manifest.json")
        manifest = json.loads(body.decode("utf-8"))
        local = os.path.join(core.data_dir, "resource_store",
                             f"restore/{pack['pack_id']}/{pack['revision']}")
        with core.db.tx() as tx:
            tx.cur.execute(
                "INSERT INTO packs(pack_id, revision, account_id, kind, fixture, label,"
                " total_bytes, manifest_sha256, manifest_bytes, storage_subdir,"
                " published_at) VALUES (?,?,?,?,?,?,?,?,?,?,?)"
                " ON CONFLICT(pack_id, revision) DO UPDATE SET label=excluded.label,"
                " total_bytes=excluded.total_bytes",
                (pack["pack_id"], pack["revision"], aid, pack["kind"], 0,
                 pack["label"], pack["total_bytes"], pack["manifest_sha256"], body,
                 f"restore/{pack['pack_id']}/{pack['revision']}", utcnow_iso()))
        for entry in manifest["files"]:
            rel = normalize_relpath(entry["path"])
            data = client.get(dav_dir + rel)
            if sha256_hex(data) != entry["sha256"]:
                raise ApiError("HASH_MISMATCH", "restored resource digest mismatch")
            path = os.path.realpath(os.path.join(local, *rel.split("/")))
            if os.path.commonpath([os.path.realpath(local), path]) != os.path.realpath(local):
                raise ApiError("BAD_REQUEST", "path escape rejected")
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as handle:
                handle.write(data)


def stage_restored_database(core: Core, snapshot_id: str) -> str:
    """Build + validate a restored database in isolation. Returns its path.

    No live mutation happens here; the host process swaps this file for the
    empty live database only after re-validating, then restarts epoch/auth.
    """
    document, _complete = fetch_snapshot_document(core, snapshot_id)
    counts = validate_snapshot_document(core, document)
    aid = document["account_id"]
    stage_root = os.path.join(core.data_dir, f"restore-stage-{secrets.token_hex(6)}")
    stage_db_path = os.path.join(stage_root, "restored.db")
    stage = Database(stage_db_path)
    try:
        with stage._tx() as tx:
            cur = tx.cur
            cur.execute(
                "INSERT INTO accounts(account_id, label, auth_salt, auth_digest,"
                " created_at) VALUES (?,?,?,?,?)",
                (aid, "restored-admin-pending", "reset", "reset", utcnow_iso()))
            for mapping in document["record_map"]:
                cur.execute(
                    "INSERT INTO record_map(account_id, kind, context_id, target_id,"
                    " record_id) VALUES (?,?,?,?,?)"
                    " ON CONFLICT(record_id) DO NOTHING",
                    (aid, mapping["kind"], mapping["context_id"],
                     mapping["target_id"], mapping["record_id"]))
            for record in document["records"]:
                cur.execute(
                    "INSERT INTO records(record_id, account_id, kind, context_id,"
                    " target_id, revision, state, tombstone, updated_by, updated_at)"
                    " VALUES (?,?,?,?,?,?,?,?,?,?)",
                    (record["record_id"], aid, record["kind"], record["context_id"],
                     record["target_id"], record["revision"], record["state"],
                     int(record["tombstone"]), record["updated_by"],
                     record["updated_at"]))
            for change in document["change_log"]:
                cur.execute(
                    "INSERT INTO change_log(change_seq, record_id, account_id, kind,"
                    " context_id, target_id, revision, state, tombstone, updated_by,"
                    " committed_at) VALUES (?,?,?,?,?,?,?,?,?,?,?)",
                    (change["change_seq"], change["record_id"], aid, change["kind"],
                     change["context_id"], change["target_id"], change["revision"],
                     change["state"], int(change["tombstone"]), change["updated_by"],
                     change["committed_at"]))
            for event in document["events_retained"]:
                cur.execute(
                    "INSERT OR REPLACE INTO events(event_key, account_id, device_id,"
                    " event_id, device_epoch, sequence, result, result_revision,"
                    " request_digest, received_at) VALUES (?,?,?,?,?,?,?,?,?,?)",
                    (event["event_key"], aid, event["device_id"], event["event_id"],
                     event["device_epoch"], event["sequence"], event["result"],
                     event["result_revision"], "restored", utcnow_iso()))
            for device in document["devices"]:
                # Restored devices return revoked: rebind required (contract 9).
                cur.execute(
                    "INSERT INTO devices(device_id, account_id, display_label,"
                    " token_digest, status, bound_at, revoked_at)"
                    " VALUES (?,?,?,?,?,?,?)"
                    " ON CONFLICT(device_id) DO UPDATE SET status='revoked'",
                    (device["device_id"], aid, device["display_label"],
                     device["token_digest"], "revoked", device["bound_at"],
                     utcnow_iso()))
            now_iso = utcnow_iso()
            stage._set_meta(cur, "account_id", aid)
            stage._set_meta(cur, "restore_bootstrap_required", "1")
            stage._set_meta(cur, "restored_from_snapshot", snapshot_id)
        return stage_db_path
    finally:
        pass  # caller removes the staging directory after the swap


def backup_worker_loop(core: Core, stop: threading.Event) -> None:
    """Sequential queue worker; backoff for temporary DAV errors."""
    while not stop.is_set():
        try:
            with core.db.tx() as tx:
                row = tx.cur.execute(
                    "SELECT job_id FROM upload_jobs WHERE state IN ('queued','uploading')"
                    " AND (next_retry_at IS NULL OR next_retry_at <= ?)"
                    " ORDER BY created_at LIMIT 1", (utcnow_iso(),)).fetchone()
            if not row:
                stop.wait(1.0)
                continue
            process_upload_job(core, row["job_id"])
        except ApiError as err:
            if err.code in ("DAV_PENDING", "DAV_QUOTA"):
                stop.wait(err.retry_after_seconds or JOB_RETRY_SECONDS)
            else:
                stop.wait(5.0)
        except OSError:
            stop.wait(JOB_RETRY_SECONDS)
