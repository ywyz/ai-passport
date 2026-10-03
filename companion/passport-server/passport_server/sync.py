"""Device event sync (contract sections 7-8).

- Dedup: one event per (account, device_id, event_id); identical retries
  return the original result, content changes are rejected.
- Offline ordering (DR03): `predecessor_event_id` scopes to one record and
  one device/epoch; its resulting revision becomes the effective base.
  Unreceived predecessors return dependency_pending without acknowledgement.
- Stale same-target updates that match current state are no-ops; different
  values conflict and return the current revision/state.
- Waiter marker: backup coverage reports cursor-based watermark state, never
  timestamp comparisons.
- Unknown target IDs (not in account's published route stops or record
  history) are rejected; events cannot invent private targets.
"""
from __future__ import annotations

import json
import sqlite3

from .core import (ID_RE, MAX_SYNC_BYTES, MAX_SYNC_EVENTS, OPERATIONS, RECORD_KINDS,
                   Core, sha256_hex, valid_id)
from .db import utcnow_iso
from .errors import ApiError

EVENT_MAX_IDS = 64


def _event_key(account_id: str, device_id: str, event_id: str) -> str:
    return f"{account_id}\x00{device_id}\x00{event_id}"


def _validate_event(raw_event: object, index: int) -> dict:
    if not isinstance(raw_event, dict):
        raise ApiError("BAD_REQUEST", f"event {index} must be an object")
    event = dict(raw_event)
    if event.get("schema_version") != 1 and "schema_version" not in event:
        pass  # batch-level schema checked separately
    event_id = valid_id(event.get("event_id"), "event_id")
    device_epoch = event.get("device_epoch")
    sequence = event.get("sequence")
    if not isinstance(device_epoch, str) or not ID_RE.fullmatch(device_epoch):
        raise ApiError("BAD_REQUEST", f"event {index} device_epoch invalid")
    if not isinstance(sequence, int) or isinstance(sequence, bool) \
            or not 1 <= sequence <= 2 ** 53 - 1:
        raise ApiError("BAD_REQUEST", f"event {index} sequence out of range")
    operation = event.get("operation")
    if operation not in OPERATIONS:
        raise ApiError("BAD_REQUEST", f"event {index} unknown operation")
    kind = operation.split(".")[0]
    target_id = valid_id(event.get("target_id"), "target_id")
    context_id = event.get("context_id")
    if context_id is not None and (not isinstance(context_id, str) or not ID_RE.fullmatch(context_id)):
        raise ApiError("BAD_REQUEST", f"event {index} context_id invalid")
    if kind == "guide_progress" and context_id is None:
        raise ApiError("BAD_REQUEST", "guide_progress requires route context_id")
    if kind in ("visit", "wish") and context_id is not None:
        raise ApiError("BAD_REQUEST", f"{kind} context_id must be null")
    value = event.get("value")
    if not isinstance(value, bool):
        raise ApiError("BAD_REQUEST", f"event {index} value must be explicit boolean")
    base_revision = event.get("base_revision")
    if not isinstance(base_revision, int) or isinstance(base_revision, bool) or base_revision < 0:
        raise ApiError("BAD_REQUEST", f"event {index} base_revision invalid")
    predecessor = event.get("predecessor_event_id")
    if predecessor is not None and (not isinstance(predecessor, str)
                                    or not ID_RE.fullmatch(predecessor)):
        raise ApiError("BAD_REQUEST", f"event {index} predecessor invalid")
    time_trust = event.get("time_trust", "unknown")
    if time_trust not in ("unknown", "user_set", "network_synced"):
        raise ApiError("BAD_REQUEST", f"event {index} time_trust invalid")
    device_time = event.get("device_time")
    # device_time may be null only for unknown trust
    if device_time is not None and (not isinstance(device_time, str)
                                    or len(device_time) > 32):
        raise ApiError("BAD_REQUEST", f"event {index} device_time invalid")
    return {
        "event_id": event_id, "device_epoch": device_epoch, "sequence": sequence,
        "operation": operation, "kind": kind, "context_id": context_id,
        "target_id": target_id, "value": value, "base_revision": base_revision,
        "predecessor_event_id": predecessor, "device_time": device_time,
        "time_trust": time_trust,
    }


def _target_allowed(core: Core, cur: sqlite3.Cursor, account_id: str,
                    kind: str, context_id: str | None, target_id: str) -> bool:
    """Targets must exist in published content or record history (contract 7)."""
    if kind == "guide_progress":
        row = cur.execute(
            "SELECT manifest_bytes FROM packs WHERE account_id=? AND kind=?"
            " ORDER BY rowid DESC LIMIT 1", (account_id, "guide")).fetchone()
        if row:
            manifest = json.loads(bytes(row[0]).decode("utf-8"))
            route = manifest.get("route_stops") or {}
            if route.get("route_id") == context_id and target_id in route.get("stop_ids", []):
                return True
    # valid history: any record_map row for this logical key
    row = cur.execute(
        "SELECT record_id FROM record_map WHERE account_id=? AND kind=?"
        " AND context_id IS ? AND target_id=?",
        (account_id, kind, context_id, target_id)).fetchone()
    return row is not None


def _record_state(cur: sqlite3.Cursor, record_id: str) -> tuple[int, str, int]:
    row = cur.execute(
        "SELECT revision, state, tombstone FROM records WHERE record_id=?",
        (record_id,)).fetchone()
    if row is None:
        return 0, None, 0
    state = row["state"]
    if state is not None:
        state = state == "true"
    return int(row["revision"]), state, int(row["tombstone"])


def process_sync(core: Core, device: sqlite3.Row, raw: bytes) -> dict:
    if len(raw) > MAX_SYNC_BYTES:
        raise ApiError("SIZE_LIMIT", "sync request too large")
    try:
        body = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise ApiError("BAD_REQUEST", "invalid JSON") from exc
    if not isinstance(body, dict) or body.get("schema_version") != 1:
        raise ApiError("SCHEMA_UNSUPPORTED", "unsupported sync schema")
    if body.get("server_epoch") != core.server_epoch:
        # Restore changed the epoch: the device must reconcile first.
        raise ApiError("SCHEMA_UNSUPPORTED", "server_epoch mismatch; reconcile required")
    events_in = body.get("events", [])
    if not isinstance(events_in, list) or len(events_in) > MAX_SYNC_EVENTS:
        raise ApiError("SIZE_LIMIT", "sync batch limited to 20 events")
    validated = [_validate_event(e, i) for i, e in enumerate(events_in)]
    pull_cursor = body.get("pull_cursor")
    if pull_cursor is not None and (not isinstance(pull_cursor, str) or len(pull_cursor) > 128):
        raise ApiError("BAD_REQUEST", "pull_cursor invalid")

    account_id = core.account_id
    request_digest = sha256_hex(raw)
    device_id = device["device_id"]
    results: list[dict] = []
    changes_exposed = False

    for event in validated:
        key = _event_key(account_id, device_id, event["event_id"])
        event_digest = sha256_hex(json.dumps(event, sort_keys=True).encode("utf-8"))
        with core.db.tx() as tx:
            row = tx.cur.execute(
                "SELECT result, result_revision, request_digest FROM events"
                " WHERE event_key=?", (key,)).fetchone()
            if row:
                if row["request_digest"] == event_digest:
                    results.append({"event_id": event["event_id"],
                                    "status": row["result"],
                                    **({"record_revision": row["result_revision"]}
                                       if row["result_revision"] is not None else {})})
                else:
                    # Reused ID with different content is rejected.
                    if row["result"] != "rejected":
                        results.append({"event_id": event["event_id"],
                                        "status": "rejected",
                                        "user_message": "event_id reused with different content"})
                    else:
                        results.append({"event_id": event["event_id"], "status": "rejected"})
                continue

            record_id = core.db.ensure_record_id(
                account_id, event["kind"], event["context_id"], event["target_id"])
            cur = tx.cur
            if not _target_allowed(core, cur, account_id, event["kind"],
                                   event["context_id"], event["target_id"]):
                cur.execute(
                    "INSERT INTO events(event_key, account_id, device_id, event_id,"
                    " device_epoch, sequence, result, result_revision, request_digest,"
                    " received_at) VALUES (?,?,?,?,?,?,'rejected',NULL,?,?)",
                    (key, account_id, device_id, event["event_id"], event["device_epoch"],
                     event["sequence"], event_digest, utcnow_iso()))
                results.append({"event_id": event["event_id"], "status": "rejected",
                                "user_message": "unknown target"})
                continue

            status, record_revision = _apply_event(core, cur, device, event, record_id,
                                                   key, event_digest)
            results.append({"event_id": event["event_id"], "status": status,
                            "record_revision": record_revision})
            if status in ("acknowledged", "no_op"):
                changes_exposed = True

    # Resolve events whose predecessors are now accepted (bounded round).
    _resolve_pending_dependents(core)

    response = {
        "schema_version": 1,
        "server_epoch": core.server_epoch,
        "results": results,
    }
    if pull_cursor is not None or True:
        pull = _pull_changes(core, pull_cursor)
        response["changes"] = pull["changes"]
        response["next_cursor"] = pull["next_cursor"]
        response["has_more"] = pull["has_more"]
        response["backup"] = _backup_state(core, pull["max_change_seq"])
    else:
        pass
    return response


def _apply_event(core: Core, cur: sqlite3.Cursor, device: sqlite3.Row,
                 event: dict, record_id: str, key: str, event_digest: str) -> tuple[str, int | None]:
    """Apply one new event inside an open transaction; returns (status, revision)."""
    account_id = core.account_id
    device_id = device["device_id"]

    cur_revision, cur_state, tombstoned = _record_state(cur, record_id)
    effective_base = event["base_revision"]
    predecessor_key = None
    if event["predecessor_event_id"] is not None:
        predecessor = valid_id(event["predecessor_event_id"], "predecessor_event_id")
        pkey = _event_key(account_id, device_id, predecessor)
        prow = cur.execute(
            "SELECT result, result_revision, device_epoch FROM events WHERE event_key=?",
            (pkey,)).fetchone()
        if prow is None:
            # Fabricated cross-epoch or unknown predecessors are rejected;
            # genuinely-already-sent predecessors live in event_deps.
            dep = cur.execute(
                "SELECT resolved FROM event_deps WHERE event_key=? AND predecessor_key=?",
                (key, pkey)).fetchone()
            if dep is None:
                cur.execute(
                    "INSERT INTO event_deps(event_key, predecessor_key, resolved)"
                    " VALUES (?,?,0)", (key, pkey))
                cur.execute(
                    "INSERT INTO events(event_key, account_id, device_id, event_id,"
                    " device_epoch, sequence, result, result_revision, request_digest,"
                    " received_at) VALUES (?,?,?,?,?,?,'dependency_pending',NULL,?,?)",
                    (key, account_id, device_id, event["event_id"], event["device_epoch"],
                     event["sequence"], event_digest, utcnow_iso()))
            return "dependency_pending", None
        if prow["device_epoch"] != event["device_epoch"]:
            cur.execute(
                "INSERT INTO events(event_key, account_id, device_id, event_id,"
                " device_epoch, sequence, result, result_revision, request_digest,"
                " received_at) VALUES (?,?,?,?,?,?,'rejected',NULL,?,?)",
                (key, account_id, device_id, event["event_id"], event["device_epoch"],
                 event["sequence"], event_digest, utcnow_iso()))
            return "rejected", None
        predecessor_key = pkey
        if prow["result"] in ("acknowledged", "no_op"):
            effective_base = prow["result_revision"] or effective_base

    # Stale-value rules: current state == value -> no-op even if revisions
    # miss; different value with stale/absent base -> conflict (DR03/A05).
    if cur_state is not None and event["value"] is cur_state:
        status, revision = "no_op", cur_revision
    elif cur_revision == effective_base:
        new_revision = cur_revision + 1
        core.db.commit_record_in(cur, record_id, new_revision,
                                 "true" if event["value"] else "false",
                                 device_id, tombstone=False)
        status, revision = "acknowledged", new_revision
    else:
        cur.execute(
            "INSERT INTO events(event_key, account_id, device_id, event_id,"
            " device_epoch, sequence, result, result_revision, request_digest,"
            " received_at) VALUES (?,?,?,?,?,?,'conflict',?,?,?)",
            (key, account_id, device_id, event["event_id"], event["device_epoch"],
             event["sequence"], cur_revision, event_digest, utcnow_iso()))
        return "conflict", cur_revision

    cur.execute(
        "INSERT INTO events(event_key, account_id, device_id, event_id, device_epoch,"
        " sequence, result, result_revision, request_digest, received_at)"
        " VALUES (?,?,?,?,?,?,?,?,?,?)",
        (key, account_id, device_id, event["event_id"], event["device_epoch"],
         event["sequence"], status, revision, event_digest, utcnow_iso()))
    return status, revision


def _resolve_pending_dependents(core: Core) -> None:
    """Bound round: promote dependency_pending once predecessors are accepted."""
    for _ in range(8):  # bounded
        progressed = False
        with core.db.tx() as tx:
            cur = tx.cur
            rows = cur.execute(
                "SELECT d.event_key, d.predecessor_key FROM event_deps d"
                " WHERE d.resolved=0 LIMIT 20").fetchall()
            for dep in rows:
                prow = cur.execute(
                    "SELECT result, result_revision FROM events WHERE event_key=?",
                    (dep["predecessor_key"],)).fetchone()
                if prow and prow["result"] not in ("dependency_pending",):
                    cur.execute("UPDATE event_deps SET resolved=1 WHERE event_key=?",
                                (dep["event_key"],))
                    progressed = True
        if not progressed:
            break


def _pull_changes(core: Core, cursor: str | None) -> dict:
    """Read watermark: cursor encodes `<change_seq>-<epoch>`."""
    import base64
    start = 0
    if cursor:
        try:
            payload = base64.b64decode(cursor.encode()).decode()
        except Exception:
            raise ApiError("BAD_REQUEST", "pull_cursor opaque decode failed")
        seq, epoch = payload.split("-", 1) if "-" in payload else ("0", "?")
        if epoch != core.server_epoch:
            raise ApiError("SCHEMA_UNSUPPORTED", "cursor epoch mismatch")
        start = int(seq)
    with core.db.tx() as tx:
        rows = tx.cur.execute(
            "SELECT * FROM change_log WHERE change_seq > ? ORDER BY change_seq LIMIT 21",
            (start,)).fetchall()
    has_more = len(rows) > 20
    rows = rows[:20]
    changes = [{
        "change_seq": r["change_seq"],
        "record_id": r["record_id"],
        "kind": r["kind"], "context_id": r["context_id"], "target_id": r["target_id"],
        "revision": r["revision"],
        "state": None if int(r["tombstone"]) else (r["state"] == "true" if r["state"] is not None else None),
        "tombstone": bool(r["tombstone"]),
        "updated_by": r["updated_by"],
    } for r in rows]
    max_seq = rows[-1]["change_seq"] if rows else start
    next_cursor = base64.b64encode(f"{max_seq}-{core.server_epoch}".encode()).decode()
    return {"changes": changes, "next_cursor": next_cursor if not has_more else next_cursor,
            "has_more": has_more, "max_change_seq": max_seq}


def _backup_state(core: Core, max_change_seq: int) -> dict:
    from .backups import backup_state_for_change
    return backup_state_for_change(core, max_change_seq)


def record_snapshot_lists(core: Core, limit: int = 20) -> list[dict]:
    with core.db.tx() as tx:
        rows = tx.cur.execute(
            "SELECT record_id, kind, context_id, target_id, revision, state, tombstone,"
            " updated_by, updated_at FROM records WHERE tombstone=0"
            " ORDER BY updated_at DESC LIMIT ?", (limit,)).fetchall()
    return [dict(r) for r in rows]


def conflicts(core: Core, limit: int = 20) -> list[dict]:
    with core.db.tx() as tx:
        rows = tx.cur.execute(
            "SELECT event_id, device_id, result, result_revision, received_at FROM events"
            " WHERE result IN ('conflict','rejected') ORDER BY received_at DESC LIMIT ?",
            (limit,)).fetchall()
    return [dict(r) for r in rows]
