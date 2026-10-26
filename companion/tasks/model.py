"""Normalize VTODOs without rewriting the original VCALENDAR resources."""

from datetime import date, datetime, timezone
from zoneinfo import ZoneInfo

from icalendar import Calendar

from .common import MAX_ICS_BYTES, MAX_ITEMS, MAX_RECORD_BYTES
from .common import TaskError, bounded, text_limit, stable_id


def _one(todo, key, default=None):
    value = todo.get(key, default)
    if isinstance(value, list):
        raise TaskError("INVALID_CALENDAR")
    return value


def time_value(prop, display_zone):
    if prop is None:
        return {"kind": "none", "value": None, "display": None}
    value = getattr(prop, "dt", None)
    if not isinstance(value, (date, datetime)):
        raise TaskError("INVALID_CALENDAR")
    original = prop.to_ical().decode("utf-8")
    if not isinstance(value, datetime):
        return {"kind": "date", "value": original, "display": value.isoformat()}
    tzid = prop.params.get("TZID")
    if tzid:
        if value.tzinfo is None:
            raise TaskError("UNRESOLVED_TIMEZONE")
        result = {"kind": "zoned", "value": original, "tzid": str(tzid)}
    elif original.endswith("Z"):
        result = {"kind": "utc", "value": original}
    else:
        return {"kind": "floating", "value": original, "display": value.isoformat()}
    result["display"] = value.astimezone(ZoneInfo(display_zone)).isoformat()
    result["utc"] = value.astimezone(timezone.utc).isoformat()
    return result


def parse_resource(raw, *, account_id, collection_id, href, etag, display_zone):
    """Return explicit resources/instances, never expand or complete a series."""
    if len(raw) > MAX_ICS_BYTES:
        raise TaskError("LIMIT_EXCEEDED")
    try:
        raw.decode("utf-8", errors="strict")
        calendar = Calendar.from_ical(raw)
        if calendar.name != "VCALENDAR" or calendar.errors:
            raise TaskError("INVALID_CALENDAR")
        if any(part.errors for part in calendar.walk()):
            raise TaskError("INVALID_CALENDAR")
        todos = calendar.walk("VTODO")
        if not todos:
            raise TaskError("INVALID_CALENDAR")
        if len(todos) > MAX_ITEMS:
            raise TaskError("LIMIT_EXCEEDED")
        records = []
        for todo in todos:
            uid = text_limit(str(_one(todo, "UID", "")), 1024)
            if not uid:
                raise TaskError("INVALID_CALENDAR")
            status = str(_one(todo, "STATUS", "NEEDS-ACTION"))
            if status not in {"NEEDS-ACTION", "IN-PROCESS", "COMPLETED", "CANCELLED"}:
                raise TaskError("INVALID_CALENDAR")
            percent = int(_one(todo, "PERCENT-COMPLETE", 0))
            if not 0 <= percent <= 100:
                raise TaskError("INVALID_CALENDAR")
            note = str(_one(todo, "DESCRIPTION", "")).encode("utf-8")
            recurrence = time_value(_one(todo, "RECURRENCE-ID"), display_zone)
            record = {
                "schema": 1, "account_id": account_id, "collection_id": collection_id,
                "href": text_limit(href, 2048), "uid": uid,
                "recurrence_id": None if recurrence["kind"] == "none" else recurrence,
                "etag": text_limit(etag, 256),
                "summary": text_limit(str(_one(todo, "SUMMARY", "")), 512),
                "status": status, "percent_complete": percent,
                "due": time_value(_one(todo, "DUE"), display_zone),
                "completed_at": time_value(_one(todo, "COMPLETED"), display_zone),
                "modified_at": time_value(_one(todo, "LAST-MODIFIED"), display_zone),
                "note": note[:4096].decode("utf-8", errors="ignore"),
                "note_truncated": len(note) > 4096, "original_bytes": len(note),
                "read_only": True,
                "recurring": any(k in todo for k in ("RRULE", "RDATE", "EXDATE", "RECURRENCE-ID")),
                "has_parent": "RELATED-TO" in todo,
            }
            # Leave room for snapshot metadata added by the store.
            record["task_id"] = task_id(record)
            bounded(record, MAX_RECORD_BYTES - 320)
            records.append(record)
        return records
    except TaskError:
        raise
    except (ValueError, TypeError, UnicodeError, KeyError, OverflowError) as exc:
        raise TaskError("INVALID_CALENDAR") from exc


def identity(record):
    return (record["account_id"], record["collection_id"], record["uid"],
            str(record["recurrence_id"]))


def task_id(record):
    return stable_id(record["account_id"], record["collection_id"], record["uid"], record["recurrence_id"])


def sort_key(record):
    due = record["due"]
    # Floating times remain visibly floating; do not fabricate an offset.
    return (due["kind"] == "none", due.get("utc", due.get("display") or ""),
            record["summary"].casefold(), identity(record))
