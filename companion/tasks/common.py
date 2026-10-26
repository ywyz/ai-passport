"""Wire limits and safe errors shared by the adapter and snapshot API."""

import hashlib
import json

MAX_ITEMS = 200
MAX_COLLECTIONS = 20
MAX_COLLECTION_BYTES = 8192
MAX_RECORD_BYTES = 8192
MAX_PAGE_BYTES = 32768
MAX_CACHE_BYTES = 1024 * 1024
MAX_ORIGINAL_BYTES = 8 * 1024 * 1024
MAX_ICS_BYTES = 256 * 1024
MAX_DAV_BYTES = 4 * 1024 * 1024
STALE_SECONDS = 24 * 60 * 60


class TaskError(Exception):
    """Only a fixed code crosses API/CLI boundaries; never upstream bodies."""

    def __init__(self, code):
        self.code = code
        super().__init__(code)


def packed(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"),
                      sort_keys=True, allow_nan=False).encode("utf-8")


def bounded(value, limit):
    data = packed(value)
    if len(data) > limit:
        raise TaskError("LIMIT_EXCEEDED")
    return data


def stable_id(*parts):
    return hashlib.sha256(packed(parts)).hexdigest()


def text_limit(value, limit):
    if not isinstance(value, str) or len(value.encode("utf-8")) > limit:
        raise TaskError("LIMIT_EXCEEDED")
    return value
