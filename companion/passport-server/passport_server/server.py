"""HTTP application for the D1a companion server.

- Public device endpoints use revocable `Authorization: Bearer <token>`
  credentials; the server only stores token digests.
- The website uses a signed-off cookie session (HttpOnly; SameSite=Strict)
  plus a per-session CSRF token for every POST. Cross-site request origins
  never touch device-binding actions across origins.
- Reference headers never reveal passwords, binding codes, tokens or seed
  material; request IDs are random and responses carry stable error codes.
"""
from __future__ import annotations

import hmac
import json
import os
import secrets
import shutil
import sqlite3
import threading
from http import cookies
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from . import backups, packs, sync, webpages
from .core import Core, sha256_hex
from .db import Database
from .errors import ApiError

WEB_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "web")
HTML = "text/html; charset=utf-8"
JSONC = "application/json; charset=utf-8"


class Main:
    """Owns the database, core, worker thread and restore swap."""

    def __init__(self, data_dir: str, listen: str = "127.0.0.1", port: int = 8642,
                 quiet: bool = False) -> None:
        self.data_dir = os.path.abspath(data_dir)
        os.makedirs(self.data_dir, exist_ok=True)
        self.db = Database(os.path.join(self.data_dir, "passport.db"))
        self.core = Core(self.db, self.data_dir)
        self.listen = listen
        self.port = port
        self.quiet = quiet
        self._stop = threading.Event()
        self._worker = threading.Thread(
            target=backups.backup_worker_loop,
            args=(self.core, self._stop), daemon=True, name="dav-worker")
        self._worker.start()

    # -- WebDAV configuration (kept out of the database) -----------------
    def configure_dav(self, base_url: str, username: str, password: str,
                      prefix: str = "passport/v1") -> None:
        from .webdav import WebdavClient
        client = WebdavClient(base_url, username, password)
        try:
            root = prefix
            client.mkdirs(root + "/")
        except Exception as exc:  # surface config problems immediately
            raise ApiError("DAV_PENDING", f"WebDAV test failed: {exc}") from exc
        self.core.dav = client
        self.core.dav_prefix = prefix.strip("/")
        os.makedirs(self.data_dir, exist_ok=True)
        cfg_path = os.path.join(self.data_dir, "dav.json")
        pw_path = cfg_path + ".pw"
        with open(cfg_path + ".tmp", "w") as tmpf:
            tmpf.write(json.dumps({"base_url": base_url, "username": username,
                                   "prefix": self.core.dav_prefix}))
        os.replace(cfg_path + ".tmp", cfg_path)
        with open(pw_path + ".tmp", "w") as tmpf:
            tmpf.write(password)
        os.replace(pw_path + ".tmp", pw_path)
        os.chmod(cfg_path, 0o600)
        os.chmod(pw_path, 0o600)
        # .pw file is local-only, mode 0600; never included in backups.

    def load_dav_config(self) -> bool:
        path = os.path.join(self.data_dir, "dav.json")
        if not os.path.exists(path):
            return False
        try:
            with open(path) as handle:
                cfg = json.load(handle)
        except (OSError, ValueError):
            return False
        if os.environ.get("PASSPORT_DAV_PASSWORD"):
            password = os.environ["PASSPORT_DAV_PASSWORD"]
        else:
            # password stored only in ancillary local file with 0600
            pw_path = path + ".pw"
            if not os.path.exists(pw_path):
                return False
            with open(pw_path) as handle:
                password = handle.read().strip()
        from .webdav import WebdavClient
        try:
            self.core.dav = WebdavClient(cfg["base_url"], cfg["username"], password)
            self.core.dav_prefix = cfg.get("prefix", "passport/v1").strip("/")
            return True
        except (KeyError, ValueError):
            return False

    def restart_worker(self) -> None:
        self._stop.set()
        self._stop = threading.Event()
        self._worker = threading.Thread(
            target=backups.backup_worker_loop,
            args=(self.core, self._stop), daemon=True, name="dav-worker")
        self._worker.start()

    # -- restore swap ----------------------------------------------------
    def restore_snapshot(self, snapshot_id: str, confirm: bool) -> dict:
        if not confirm:
            return backups.preview_restore(self.core, snapshot_id)
        if backups.record_count(self.core) != 0:
            raise ApiError("FORBIDDEN", "restore requires an empty test server;"
                           " existing records would be lost")
        stage_db = backups.stage_restored_database(self.core, snapshot_id)
        stage_dir = os.path.dirname(stage_db)
        aid = self.db.get_meta("account_id") or "account-local"
        # Fetch resource manifests/bodies (they belong to the snapshot).
        document, _complete = backups.fetch_snapshot_document(self.core, snapshot_id)
        stage_aid = document["account_id"]
        backups._restore_resource_store(self.core, document)  # noqa: SLF001
        live_path = os.path.join(self.data_dir, "passport.db")
        self.db._conn.close()  # noqa: SLF001
        backup_old = live_path + ".pre-restore"
        os.replace(live_path, backup_old)
        try:
            os.replace(stage_db, live_path)
        except OSError:
            os.replace(backup_old, live_path)
            raise
        for suffix in ("-wal", "-shm"):
            extra = live_path + suffix
            if os.path.exists(extra):
                os.remove(extra)
        self.db = Database(live_path)
        self.core.db = self.db
        self.core.dav_prefix = self.core.dav_prefix  # unchanged config
        new_epoch = self.db.new_epoch()
        self.db.set_meta_now("restore_bootstrap_required", "1")
        self.db.set_meta_now("restored_from_snapshot", snapshot_id)
        if os.path.exists(backup_old):
            os.remove(backup_old)
        shutil.rmtree(stage_dir, ignore_errors=True)
        return {"restored": True, "snapshot_id": snapshot_id,
                "new_server_epoch": new_epoch,
                "rebind_required": True, "account_id": stage_aid,
                "counts": {}}

    def shutdown(self) -> None:
        self._stop.set()
        self.db._conn.close()  # noqa: SLF001


class Sessions:
    """Website cookie sessions + CSRF (memory-only)."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._sessions: dict[str, dict] = {}

    def create(self) -> tuple[str, str]:
        sid = secrets.token_urlsafe(24)
        csrf = secrets.token_urlsafe(24)
        with self._lock:
            self._sessions[sid] = {"csrf": csrf}
        return sid, csrf

    def get(self, sid: str | None) -> dict | None:
        if not sid:
            return None
        with self._lock:
            return self._sessions.get(sid)

    def drop(self, sid: str | None) -> None:
        if not sid:
            return
        with self._lock:
            self._sessions.pop(sid, None)


SESSIONS = Sessions()


class Handler(BaseHTTPRequestHandler):
    server_version = "passport-server/1"
    protocol_version = "HTTP/1.1"
    main: Main = None  # type: ignore[assignment]

    # -- plumbing --------------------------------------------------------
    def log_message(self, fmt, *args):  # noqa: A003
        if not self.main.quiet:
            super().log_message(fmt, *args)

    def request_id(self) -> str:
        return "req-" + secrets.token_hex(6)

    def send_json(self, status: int, payload: dict, request_id: str,
                  extra_headers: dict | None = None) -> None:
        payload = dict(payload, request_id=request_id)
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", JSONC)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Request-ID", request_id)
        for key, value in (extra_headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(body)

    def send_api_error(self, err: ApiError, request_id: str) -> None:
        headers = {}
        if err.retry_after_seconds:
            headers["Retry-After"] = str(err.retry_after_seconds)
        self.send_json(err.status, err.to_body(request_id), request_id, headers)

    def read_json_body(self, max_bytes: int) -> dict:
        length = int(self.headers.get("Content-Length") or 0)
        if length > max_bytes:
            raise ApiError("SIZE_LIMIT", "request body too large")
        raw = self.rfile.read(length) if length else b""
        try:
            data = json.loads(raw.decode("utf-8"))
        except (ValueError, UnicodeDecodeError) as exc:
            raise ApiError("BAD_REQUEST", "invalid JSON body") from exc
        if not isinstance(data, dict):
            raise ApiError("BAD_REQUEST", "JSON object body required")
        self._last_body = raw
        return data

    def read_body_raw(self, max_bytes: int) -> bytes:
        length = int(self.headers.get("Content-Length") or 0)
        if length > max_bytes:
            raise ApiError("SIZE_LIMIT", "request body too large")
        return self.rfile.read(length)

    def request_token(self) -> str | None:
        auth = self.headers.get("Authorization") or ""
        if auth.startswith("Bearer "):
            return auth[len("Bearer "):].strip()
        return None

    # -- cookies / auth --------------------------------------------------
    def session(self):
        raw = self.headers.get("Cookie")
        if not raw:
            return None
        jar = cookies.SimpleCookie()
        try:
            jar.load(raw)
        except cookies.CookieError:
            return None
        morsel = jar.get("ps_session")
        return SESSIONS.get(morsel.value) if morsel else None

    def csrf_ok(self, data: dict) -> bool:
        sess = self.session()
        if sess is None:
            return False
        token = data.get("csrf") or self.headers.get("X-CSRF") or ""
        return hmac.compare_digest(sess.get("csrf", ""), str(token))

    def try_web_page(self, method: str) -> bool:
        from urllib.parse import unquote, urlsplit
        path = unquote(urlsplit(self.path).path)
        if method != "GET":
            return False
        if path.startswith("/api/"):
            return False
        if path in ("/", "/login"):
            if self.session() is not None:
                self._redirect("/app")
            else:
                self._send_html(webpages.login_page(), 200)
            return True
        if path == "/app":
            sess = self.session()
            if sess is None:
                self._redirect("/login")
            else:
                self._send_html(webpages.app_page(sess["csrf"]), 200)
            return True
        static = webpages.serve_static(path)
        if static:
            body, ctype = static
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return True
        if path in ("/favicon.ico",):
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return True
        return False

    def _send_html(self, body: bytes, status: int = 200) -> None:
        self.send_response(status)
        self.send_header("Content-Type", HTML)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _redirect(self, location: str) -> None:
        self.send_response(302)
        self.send_header("Location", location)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):  # noqa: N802
        if not self.try_web_page("GET"):
            self.dispatch("GET", self.path)

    def do_POST(self):  # noqa: N802
        self.dispatch("POST", self.path)

    def handle_one_request(self):  # noqa: D102
        try:
            super().handle_one_request()
        except (ConnectionResetError, BrokenPipeError):
            pass

    def _require_device(self):
        token = self.request_token()
        device = self.main.core.device_by_token(token or "")
        if device is None:
            raise ApiError("AUTH_REQUIRED", "device authorization required")
        return device

    def _require_web(self):
        sess = self.session()
        if sess is None:
            raise ApiError("AUTH_REQUIRED", "login required")
        return sess

    def max_change_seq(self) -> int:
        with self.main.core.db.tx() as tx:
            return tx.cur.execute(
                "SELECT COALESCE(MAX(change_seq),0) FROM change_log").fetchone()[0]

    # main dispatch implemented in `dispatch` (see below)
    def dispatch(self, method: str, path: str) -> bool:
        core = self.main.core
        request_id = self.request_id()
        from urllib.parse import urlsplit, unquote
        path = unquote(urlsplit(path).path)
        try:
            # public device endpoints
            if method == "POST" and path == "/api/v1/device-bindings/exchange":
                data = self.read_json_body(4096)
                code = data.get("code")
                payload, exchange_token = core.exchange_binding(
                    str(code or ""), str(data.get("display_identifier") or ""),
                    str(data.get("request_id") or ""))
                self.send_json(200, {**payload, "exchange_token": exchange_token},
                               request_id)
                return True
            if method == "POST" and path.startswith(
                    "/api/v1/device-bindings/") and path.endswith("/confirm"):
                binding_id = valid_id_str(path.split("/")[4])
                data = self.read_json_body(4096)
                payload = core.confirm_binding(binding_id, str(data.get("exchange_token") or ""))
                self.send_json(200, payload, request_id)
                return True
            # authenticated device endpoints
            token = self.request_token()
            device = core.device_by_token(token) if token else None
            if method == "GET" and path == "/api/v1/device/catalog":
                self._require_device()
                from urllib.parse import parse_qs, urlsplit
                query = parse_qs(urlsplit(self.path).query)
                page = packs.catalog_page(core, (query.get("cursor") or [None])[0])
                self.send_json(200, {"schema_version": 1, **page}, request_id)
                return True
            if method == "GET" and path.startswith("/api/v1/device/packs/") \
                    and path.endswith("/manifest"):
                self._require_device()
                parts = path.split("/")
                pack_id = valid_id_str(parts[4])
                revision = int(parts[5])
                body = packs.get_manifest(core, pack_id, revision)
                self.send_response(200)
                self.send_header("Content-Type", JSONC)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return True
            if method == "GET" and path.startswith("/api/v1/device/packs/") \
                    and "/files/" in path:
                self._require_device()
                parts = path.split("/")
                pack_id = valid_id_str(parts[4])
                revision = int(parts[5])
                file_id = valid_id_str("/".join(parts[7:]))
                body, media = packs.read_file(core, pack_id, revision, file_id)
                self.send_response(200)
                self.send_header("Content-Type", media)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return True
            if method == "POST" and path == "/api/v1/device/sync":
                device_row = self._require_device()
                raw = self.read_body_raw(8192 + 512)
                response = sync.process_sync(core, device_row, raw)
                self.send_json(200, response, request_id)
                return True
            if method == "GET" and path == "/api/v1/device/status":
                self._require_device()
                state = backups.backup_state_for_change(
                    core, self.max_change_seq())
                self.send_json(200, {
                    "schema_version": 1,
                    "server_epoch": core.server_epoch,
                    "server_time": now_iso(),
                    "backup": state,
                }, request_id)
                return True
            # website endpoints
            if method == "POST" and path == "/api/v1/web/session":
                data = self.read_json_body(4096)
                password = str(data.get("password") or "")
                if not self.main.core.verify_admin(password):
                    raise ApiError("AUTH_REQUIRED", "login rejected")
                sid, csrf = SESSIONS.create()
                cookie = (f"ps_session={sid}; Path=/; HttpOnly; SameSite=Strict;"
                          " Max-Age=3600")
                self.send_json(200, {"csrf": csrf}, request_id,
                               {"Set-Cookie": cookie})
                return True
            if method == "POST" and path == "/api/v1/web/devices":
                sess = self._require_web()
                data = self.read_json_body(4096)
                if not self.csrf_ok(data):
                    raise ApiError("AUTH_REQUIRED", "CSRF rejected")
                payload = core.create_binding()
                self.send_json(200, payload, request_id)
                return True
            if method == "POST" and path.startswith("/api/v1/web/devices/") \
                    and path.endswith("/revoke"):
                sess = self._require_web()
                data = self.read_json_body(4096)
                if not self.csrf_ok(data):
                    raise ApiError("AUTH_REQUIRED", "CSRF rejected")
                device_id = valid_id_str(path.split("/")[5])
                core.revoke_device(device_id)
                self.send_json(200, {"revoked": device_id}, request_id)
                return True
            if method == "POST" and path == "/api/v1/web/packs":
                sess = self._require_web()
                data = self.read_json_body(12 * 1024 * 1024)
                if not self.csrf_ok(data):
                    raise ApiError("AUTH_REQUIRED", "CSRF rejected")
                files, _meta = packs.validate_pack_payload(data)
                result = packs.publish_pack(core, data)
                packs.store_pack_files(core, files, result["storage_subdir"])
                if core.dav is not None:
                    backups.enqueue_resource_uploads(core, result["pack_id"],
                                                     result["revision"])
                self.send_json(200, result, request_id)
                return True
            if method == "GET" and path == "/api/v1/web/records":
                self._require_web()
                self.send_json(200, {"records": sync.record_snapshot_lists(core),
                                     "conflicts": sync.conflicts(core),
                                     "devices": core.list_devices()}, request_id)
                return True
            if method == "POST" and path == "/api/v1/web/webdav":
                sess = self._require_web()
                data = self.read_json_body(4096)
                if not self.csrf_ok(data):
                    raise ApiError("AUTH_REQUIRED", "CSRF rejected")
                self.main.configure_dav(
                    str(data.get("base_url") or ""),
                    str(data.get("username") or ""),
                    str(data.get("password") or ""))
                # password never persisted to db or snapshots
                self.send_json(200, {"dav_configured": True,
                                     "state": "ok"}, request_id)
                return True
            if method == "POST" and path == "/api/v1/web/backups":
                sess = self._require_web()
                data = self.read_json_body(4096)
                if not self.csrf_ok(data):
                    raise ApiError("AUTH_REQUIRED", "CSRF rejected")
                payload = backups.start_snapshot_job(core)
                self.send_json(202, {**payload, "state": "queued"}, request_id)
                return True
            if method == "GET" and path.startswith("/api/v1/web/backups/"):
                self._require_web()
                job_id = valid_id_str(path.rsplit("/", 1)[1])
                with core.db.tx() as tx:
                    job = tx.cur.execute(
                        "SELECT job_id, kind, state, error_code FROM upload_jobs"
                        " WHERE job_id=?", (job_id,)).fetchone()
                    snap = tx.cur.execute(
                        "SELECT snapshot_id, state FROM snapshots WHERE snapshot_id=?",
                        (job_id.removeprefix("snap-"),)).fetchone()
                if not job:
                    raise ApiError("NOT_FOUND", "backup job not found")
                self.send_json(200, {
                    "job_id": job["job_id"], "kind": job["kind"],
                    "state": job["state"], "error_code": job["error_code"],
                    "snapshot_id": snap["snapshot_id"] if snap else None,
                    "snapshot_state": snap["state"] if snap else None,
                }, request_id)
                return True
            if method == "GET" and path == "/api/v1/web/backups":
                self._require_web()
                self.send_json(200, backups.list_snapshots(core), request_id)
                return True
            if method == "POST" and path == "/api/v1/web/restore-previews":
                sess = self._require_web()
                data = self.read_json_body(4096)
                if not self.csrf_ok(data):
                    raise ApiError("AUTH_REQUIRED", "CSRF rejected")
                preview = backups.preview_restore(
                    core, valid_id_str(str(data.get("snapshot_id") or "")))
                self.send_json(200, preview, request_id)
                return True
            if method == "POST" and path == "/api/v1/web/restore":
                sess = self._require_web()
                data = self.read_json_body(4096)
                if not self.csrf_ok(data):
                    raise ApiError("AUTH_REQUIRED", "CSRF rejected")
                payload = self.main.restore_snapshot(
                    valid_id_str(str(data.get("snapshot_id") or "")),
                    bool(data.get("confirm")))
                self.send_json(200, payload, request_id)
                return True
            if method == "GET" and path == "/api/v1/web/status":
                self._require_web()
                self.send_json(200, {
                    "server_epoch": core.server_epoch,
                    "server_time": now_iso(),
                    "upload_jobs": backups.list_jobs(core),
                    "snapshots": backups.list_snapshots(core),
                    "dav_configured": core.dav is not None,
                }, request_id)
                return True
            raise ApiError("NOT_FOUND", "endpoint not found")
        except ApiError as err:
            self.send_api_error(err, request_id)
            return True
        except (BrokenPipeError, ConnectionResetError):
            return True
        return False


def valid_id_str(value: str) -> str:
    if not isinstance(value, str) or not value or len(value) > 64 \
            or any(c in value for c in "/\\") :
        raise ApiError("BAD_REQUEST", "invalid identifier")
    return value


def now_iso() -> str:
    import datetime as _dt
    return _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds")
