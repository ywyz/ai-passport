"""Resource pack publication, validation and device catalog (contract section 4).

- Actual SHA-256 per file, computed at publication (never fake digests).
- Path normalization rejects absolute paths, '..' segments, empty segments,
  backslashes and directory escapes after normalization (A03 traversal rule).
- Per-revision unique file_id (DR05): file_id is the download identity,
  never the relative path.
- Per-document 10 MiB cap and per-fixture kind cap of 768 KiB.
- glyph inventory is stored with the pack; activation completeness is
  checked by the device (device rejects missing glyphs).
"""
from __future__ import annotations

import base64
import binascii
import json

from .core import FIXTURE_PACK_CAP, MAX_DOCUMENT_BYTES, Core, sha256_hex, valid_id
from .errors import ApiError

ALLOWED_MEDIA = {
    ".json": "application/json",
    ".txt": "text/plain",
    ".pcm": "audio/x-passport-pcm",
    ".pem": None,  # not allowed as pack content; listed for rejection clarity
}
ALLOWED_BASES = ("data", "text", "fonts", "audio")
REJECTED_EXTS = (".exe", ".sh", ".bin", ".js", ".dll", ".so", ".py", ".elf")


def normalize_relpath(path: str) -> str:
    if not isinstance(path, str) or not path:
        raise ApiError("BAD_REQUEST", "invalid file path")
    if "\\" in path or path.startswith("/") or path.startswith("~"):
        raise ApiError("BAD_REQUEST", "invalid file path")
    segments = path.split("/")
    for seg in segments:
        if not seg or seg in (".", "..") or seg.startswith("."):
            raise ApiError("BAD_REQUEST", "invalid file path")
    if not segments or segments[0] not in ALLOWED_BASES:
        raise ApiError("BAD_REQUEST", "file path must live under " + "/".join(ALLOWED_BASES))
    for seg in segments[:-1]:
        # Resource side: indexes/text live in fixed top-level bases; keep any
        # deeper nesting allowed but reject directory traversal above (done).
        pass
    if segments[-1].endswith(REJECTED_EXTS):
        raise ApiError("BAD_REQUEST", "rejected file type")
    return "/".join(segments)


def validate_pack_payload(payload: dict) -> tuple[list[dict], dict]:
    """Validate a publication document; returns normalized file descriptors."""
    if payload.get("schema_version") != 1:
        raise ApiError("SCHEMA_UNSUPPORTED", "unsupported schema_version")
    pack_id = valid_id(payload.get("pack_id"), "pack_id")
    kind = payload.get("kind")
    if kind not in ("fixture", "province", "guide"):
        raise ApiError("BAD_REQUEST", "unsupported pack kind")
    label = payload.get("label")
    if not isinstance(label, str) or not 1 <= len(label) <= 128:
        raise ApiError("BAD_REQUEST", "pack label required")
    files_in = payload.get("files")
    if not isinstance(files_in, list) or not files_in:
        raise ApiError("BAD_REQUEST", "pack files required")
    if len(files_in) > 128:
        raise ApiError("SIZE_LIMIT", "too many files")
    glyph_inventory = payload.get("glyph_inventory", [])
    if not isinstance(glyph_inventory, list) or len(glyph_inventory) > 8192:
        raise ApiError("BAD_REQUEST", "glyph inventory out of bounds")
    for g in glyph_inventory:
        if not isinstance(g, int) or not 0 <= g <= 0x10FFFF:
            raise ApiError("BAD_REQUEST", "glyph inventory must list code points")

    seen_paths: set[str] = set()
    files: list[dict] = []
    total = 0
    for item in files_in:
        if not isinstance(item, dict):
            raise ApiError("BAD_REQUEST", "file descriptor required")
        rel = normalize_relpath(item.get("path"))
        if rel in seen_paths:
            raise ApiError("BAD_REQUEST", "duplicate file path")
        seen_paths.add(rel)
        if "text" in item:
            if not isinstance(item["text"], str):
                raise ApiError("BAD_REQUEST", "file text must be UTF-8 string")
            content = item["text"].encode("utf-8")
        elif "content_base64" in item:
            try:
                content = base64.b64decode(item["content_base64"], validate=True)
            except (binascii.Error, ValueError) as exc:
                raise ApiError("BAD_REQUEST", "invalid content_base64") from exc
        else:
            raise ApiError("BAD_REQUEST", "file content required")
        if len(content) == 0:
            raise ApiError("BAD_REQUEST", "empty file rejected")
        media = item.get("media_type")
        if (not isinstance(media, str) or media in ("application/x-executable",)
                or not 1 <= len(media) <= 64):
            raise ApiError("BAD_REQUEST", "media_type required")
        total += len(content)
        cap = FIXTURE_PACK_CAP if kind == "fixture" else MAX_DOCUMENT_BYTES
        if total > cap:
            raise ApiError("SIZE_LIMIT", "pack exceeds size cap")
        files.append({"path": rel, "size_bytes": len(content), "content": content,
                      "media_type": media, "format": item.get("format", "plain")})
    return files, {"pack_id": pack_id, "kind": kind, "label": label,
                   "glyph_inventory": glyph_inventory,
                   "fixture": payload.get("fixture", kind == "fixture")}


def build_manifest(files: list[dict], meta: dict, revision: int,
                   route_stops: dict | None = None) -> dict:
    out_files = []
    file_ids: set[str] = set()
    for item in files:
        file_id = "f" + sha256_hex(item["path"].encode())[:20]
        # Uniqueness inside the revision (DR05): keep the first, rename later
        # collisions deterministically.
        candidate = file_id
        n = 1
        while candidate in file_ids:
            candidate = f"{file_id}-{n}"
            n += 1
        file_ids.add(candidate)
        out_files.append({
            "file_id": candidate,
            "path": item["path"],
            "size_bytes": item["size_bytes"],
            "sha256": sha256_hex(item["content"]),
            "media_type": item["media_type"],
            "format": item["format"],
        })
    if route_stops is not None:
        manifest = {
            "schema_version": 1,
            "pack_id": meta["pack_id"],
            "revision": revision,
            "kind": meta["kind"],
            "fixture": bool(meta["fixture"]),
            "total_bytes": sum(f["size_bytes"] for f in out_files),
            "min_app_contract": "passport-travel-v1-draft",
            "glyph_inventory": meta["glyph_inventory"],
            "route_stops": route_stops,  # guide targets allowed in stop events
            "files": out_files,
        }
    else:
        manifest = {
            "schema_version": 1,
            "pack_id": meta["pack_id"],
            "revision": revision,
            "kind": meta["kind"],
            "fixture": bool(meta["fixture"]),
            "total_bytes": sum(f["size_bytes"] for f in out_files),
            "min_app_contract": "passport-travel-v1-draft",
            "glyph_inventory": meta["glyph_inventory"],
            "files": out_files,
        }
    return manifest


def publish_pack(core: Core, payload: dict) -> dict:
    files, meta = validate_pack_payload(payload)
    pack_id = meta["pack_id"]
    events = payload.get("guide_targets")
    route_stops = None
    if isinstance(events, dict) and events.get("guide_route"):
        route = valid_id(events.get("guide_route"), "guide_route")
        stops = events.get("stops")
        if not isinstance(stops, list) or len(stops) > 64 or not stops:
            raise ApiError("BAD_REQUEST", "guide_targets.stops required")
        route_stops = {"route_id": route,
                       "stop_ids": [valid_id(s, "stop_id") for s in stops]}
    with core.db.tx() as tx:
        cur = tx.cur.execute(
            "SELECT MAX(revision) FROM packs WHERE pack_id=?", (pack_id,)).fetchone()
        revision = (cur[0] or 0) + 1
        # Conflict with concurrent sysc? A newer revision published after a
        # device began a different pack revision keeps old revision active.
        if core.dav is not None:
            from .backups import dav_resource_path  # lazy import to avoid cycle
            storage_subdir = dav_resource_path(core, pack_id, revision)
        else:
            storage_subdir = f"resources/{pack_id}/{revision}"
        manifest = build_manifest(files, meta, revision, route_stops)
        manifest_bytes = json.dumps(
            manifest, ensure_ascii=False, sort_keys=True).encode("utf-8")
        tx.cur.execute(
            "INSERT INTO packs(pack_id, revision, account_id, kind, fixture, label,"
            " total_bytes, manifest_sha256, manifest_bytes, storage_subdir, published_at)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?)",
            (pack_id, revision, core.account_id, meta["kind"], int(meta["fixture"]),
             meta["label"], manifest["total_bytes"], sha256_hex(manifest_bytes),
             manifest_bytes, storage_subdir, __import__("time").strftime(
                 "%Y-%m-%dT%H:%M:%S", __import__("time").gmtime())))
        return {"pack_id": pack_id, "revision": revision,
                "storage_subdir": storage_subdir, "manifest": manifest}


def store_pack_files(core: Core, files: list[dict], subdir: str) -> None:
    """Write file bodies locally (server cache) — DAV upload is async."""
    import os
    root = os.path.realpath(os.path.join(core.data_dir, "resource_store", subdir))
    for item in files:
        path = os.path.realpath(os.path.join(root, *item["path"].split("/")))
        if os.path.commonpath([root, path]) != root or path == root:
            raise ApiError("BAD_REQUEST", "path escape rejected")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as handle:
            handle.write(item["content"])


def catalog_page(core: Core, cursor: str | None, limit: int = 20) -> dict:
    """Catalog of latest published revisions, rowid-ordered opaque cursor."""
    limit = min(max(limit, 1), 20)
    after_rowid = -1
    if cursor:
        try:
            after_rowid = int(base64.b64decode(cursor.encode()).decode())
        except (ValueError, TypeError, binascii.Error):
            after_rowid = -1
    with core.db.tx() as tx:
        # Latest revision per pack (guard: a pack publishing rev2 while device
        # still owns rev1 keeps catalog returning the newest row each page run).
        rows = tx.cur.execute(
            "SELECT p1.rowid AS rid, p1.pack_id, p1.revision, p1.label, p1.kind,"
            " p1.fixture, p1.total_bytes FROM packs p1"
            " WHERE p1.account_id=? AND p1.rowid > ? AND p1.revision = ("
            "   SELECT MAX(revision) FROM packs p2 WHERE p2.pack_id=p1.pack_id)"
            " ORDER BY p1.rowid LIMIT ?",
            (core.account_id, after_rowid, limit + 1)).fetchall()
    has_more = len(rows) > limit
    rows = rows[:limit]
    items = [{
        "pack_id": r["pack_id"], "revision": r["revision"], "label": r["label"],
        "kind": r["kind"], "fixture": bool(r["fixture"]),
        "total_bytes": r["total_bytes"], "state": "ready",
        "coverage": None,
    } for r in rows]
    next_cursor = None
    if items and has_more:
        next_cursor = base64.b64encode(str(rows[-1]["rid"]).encode()).decode()
    return {"items": items, "next_cursor": next_cursor, "has_more": has_more}


def get_manifest(core: Core, pack_id: str, revision: int) -> bytes:
    with core.db.tx() as tx:
        row = tx.cur.execute(
            "SELECT manifest_bytes FROM packs WHERE pack_id=? AND revision=?"
            " AND account_id=?", (pack_id, revision, core.account_id)).fetchone()
    if not row:
        raise ApiError("NOT_FOUND", "pack revision not found")
    return bytes(row[0])


def read_file(core: Core, pack_id: str, revision: int, file_id: str) -> tuple[bytes, str]:
    manifest = json.loads(get_manifest(core, pack_id, revision).decode("utf-8"))
    target = None
    for entry in manifest["files"]:
        if entry["file_id"] == file_id:
            target = entry
            break
    if target is None:
        raise ApiError("NOT_FOUND", "file_id not in manifest")
    # Defense in depth: re-normalize the manifest path before local reads.
    normalize_relpath(target["path"])
    import os
    with core.db.tx() as tx:
        row = tx.cur.execute(
            "SELECT storage_subdir FROM packs WHERE pack_id=? AND revision=?",
            (pack_id, revision)).fetchone()
    path = os.path.join(core.data_dir, "resource_store", row["storage_subdir"],
                        *target["path"].split("/"))
    root = os.path.realpath(os.path.join(core.data_dir, "resource_store",
                                         row["storage_subdir"]))
    if not os.path.realpath(path).startswith(root + os.sep):
        raise ApiError("BAD_REQUEST", "path escape rejected")
    try:
        with open(path, "rb") as handle:
            data = handle.read(MAX_DOCUMENT_BYTES)
    except OSError as exc:
        raise ApiError("NOT_FOUND", "file unavailable") from exc
    if sha256_hex(data) != target["sha256"]:
        raise ApiError("HASH_MISMATCH", "stored file digest mismatch")
    return data, target["media_type"]
