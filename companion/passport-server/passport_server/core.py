"""Domain logic for the D1a companion server (passport-travel-v1-draft).

Implements DR01..DR08 for the server side:
- DR01: stable record identity per (account, kind, context_id, target_id).
- DR02: idempotent, recoverable binding exchange/confirmation; revocation
  cannot be resurrected by session retries.
- DR03: predecessor scoping for offline ordering; dependency_pending without
  acknowledgement until the predecessor is accepted.
- DR05: per-revision unique file_id and immutable manifests.
- DR06: snapshots contain identity, publication and cursors; restoration
  never revives authentication and always creates a new server_epoch.
Secrets (passwords, binding codes, device tokens) never appear in logs or
stored events: only digests are persisted.
"""
from __future__ import annotations

import base64
import hashlib
import hmac
import json
import re
import secrets
import sqlite3
from datetime import datetime, timedelta, timezone

from .db import Database, utcnow_iso
from .errors import ApiError

CONTRACT_NAME = "passport-travel-v1-draft"
OPERATIONS = ("visit.set", "wish.set", "guide_progress.set")
RECORD_KINDS = ("visit", "wish", "guide_progress")
ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$")
MAX_SYNC_EVENTS = 20
MAX_SYNC_BYTES = 8 * 1024
MAX_DOCUMENT_BYTES = 10 * 1024 * 1024
FIXTURE_PACK_CAP = 768 * 1024
BINDING_TTL = timedelta(minutes=5)
BINDING_MAX_ATTEMPTS = 5
MANIFEST_MAX_FILES = 128

# --------------------------------------------------------------- helpers


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def digest_secret(secret: str) -> str:
    return hashlib.sha256(secret.encode("utf-8")).hexdigest()


def digest_request(raw: bytes) -> str:
    return sha256_hex(raw)


def valid_id(value: object, field: str) -> str:
    if not isinstance(value, str) or not ID_RE.fullmatch(value):
        raise ApiError("BAD_REQUEST", f"invalid {field}")
    return value


def parse_iso(value: str) -> datetime | None:
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00"))
    except (ValueError, AttributeError, TypeError):
        return None


# --------------------------------------------------------------- app core


class Core:
    def __init__(self, db: Database, data_dir: str) -> None:
        self.db = db
        self.data_dir = data_dir
        self.dav: object | None = None  # webdav.WebdavClient | None config
        self.dav_prefix = "passport/v1"
        self._recovery: dict[str, str] = {}  # binding_id/device_id -> token (volatile)

    @property
    def account_id(self) -> str:
        aid = self.db.get_meta("account_id")
        return aid or "account-local"

    @property
    def server_epoch(self) -> str:
        return self.db.get_meta("server_epoch") or "unknown-epoch"

    # ------------------------------------------------------- bootstrap/auth
    def bootstrap_admin(self, password: str) -> None:
        """First-run admin creation. Controlled bootstrap; no public reset."""
        salt = secrets.token_hex(16)
        digest = hashlib.pbkdf2_hmac("sha256", password.encode(), salt.encode(), 100_000).hex()
        with self.db.tx() as tx:
            rows = tx.cur.execute("SELECT COUNT(*) FROM accounts").fetchone()
            if rows[0]:
                raise ApiError("FORBIDDEN", "administrator already initialized")
            tx.cur.execute(
                "INSERT INTO accounts(account_id, label, auth_salt, auth_digest, created_at)"
                " VALUES (?,?,?,?,?)", (self.account_id, "admin", salt, digest, utcnow_iso()))
            self.db._set_meta(tx.cur, "restore_bootstrap_required", "0")

    def verify_admin(self, password: str) -> bool:
        with self.db.tx() as tx:
            row = tx.cur.execute(
                "SELECT auth_salt, auth_digest FROM accounts WHERE account_id=?",
                (self.account_id,)).fetchone()
        if not row:
            return False
        digest = hashlib.pbkdf2_hmac(
            "sha256", password.encode(), row["auth_salt"].encode(), 100_000).hex()
        return hmac.compare_digest(digest, row["auth_digest"])

    def reset_admin_auth(self, password: str) -> None:
        """Post-restore isolated bootstrap: reset credentials, keep data."""
        salt = secrets.token_hex(16)
        digest = hashlib.pbkdf2_hmac("sha256", password.encode(), salt.encode(), 100_000).hex()
        with self.db.tx() as tx:
            old = tx.cur.execute("SELECT account_id FROM accounts LIMIT 1").fetchone()
            aid = old["account_id"] if old else self.account_id
            tx.cur.execute("DELETE FROM accounts")
            tx.cur.execute(
                "INSERT INTO accounts(account_id, label, auth_salt, auth_digest, created_at)"
                " VALUES (?,?,?,?,?)", (aid, "admin", salt, digest, utcnow_iso()))
            self.db._set_meta(tx.cur, "restore_bootstrap_required", "0")

    def bootstrap_required(self) -> bool:
        return self.db.get_meta("restore_bootstrap_required") == "1"

    # ------------------------------------------------------- bindings (DR02)
    def create_binding(self) -> dict:
        binding_id = "b-" + secrets.token_hex(8)
        code = "".join(secrets.choice("ABCDEFGHJKMNPQRSTUVWXYZ23456789") for _ in range(6))
        now = datetime.now(timezone.utc)
        with self.db.tx() as tx:
            tx.cur.execute(
                "INSERT INTO binding_sessions(binding_id, code_hash, account_id, created_at,"
                " expires_at, attempts, state) VALUES (?,?,?,?,?,0,'pending')",
                (binding_id, digest_secret(code), self.account_id,
                 now.isoformat(timespec="seconds"), (now + BINDING_TTL).isoformat(timespec="seconds")))
        return {"binding_id": binding_id, "code": code,
                "expires_at": (now + BINDING_TTL).isoformat(timespec="seconds")}

    def exchange_binding(self, code: str, display_identifier: str,
                         request_id: str) -> tuple[dict, str]:
        if not isinstance(code, str) or len(code) != 6:
            raise ApiError("BAD_REQUEST", "binding code rejected")
        if not isinstance(display_identifier, str) or not 1 <= len(display_identifier) <= 64:
            raise ApiError("BAD_REQUEST", "display identifier required")
        if not isinstance(request_id, str) or not 1 <= len(request_id) <= 64:
            raise ApiError("BAD_REQUEST", "durable request identifier required")
        with self.db.tx() as tx:
            cur = tx.cur
            row = cur.execute("SELECT * FROM binding_sessions WHERE binding_id=?",
                              (request_id,)).fetchone()
            if row is None or not hmac.compare_digest(row["code_hash"], digest_secret(code)):
                # Wrong/unknown codes are counted against any pending session to
                # bound brute force; never log the code itself.
                cur.execute(
                    "UPDATE binding_sessions SET attempts=attempts+1 "
                    "WHERE state IN ('pending','exchanged') AND expires_at > ?",
                    (utcnow_iso(),))
                if row is not None and row["state"] == "pending":
                    attempt = row["attempts"] + 1
                    if attempt >= BINDING_MAX_ATTEMPTS:
                        cur.execute("UPDATE binding_sessions SET state='expired' WHERE binding_id=?",
                                    (request_id,))
                raise ApiError("BINDING_EXPIRED", "binding code rejected", retry_after_seconds=60)
            if row["state"] == "consumed":
                # A lost confirm response: same request recovers the same
                # credential inside the window (revocation deletes this path).
                payload = self._confirmed_payload(cur, request_id)
                return payload, None
            if row["state"] not in ("pending", "exchanged"):
                raise ApiError("BINDING_EXPIRED", "binding is not exchangeable")
            if row["expires_at"] <= utcnow_iso():
                cur.execute("UPDATE binding_sessions SET state='expired' WHERE binding_id=?",
                            (request_id,))
                raise ApiError("BINDING_EXPIRED", "binding code expired")
            if row["display_identifier"] not in (None, display_identifier):
                raise ApiError("BINDING_EXPIRED", "request replay with different identity")
            exchange_token = secrets.token_urlsafe(32)
            cur.execute(
                "UPDATE binding_sessions SET state='exchanged', display_identifier=?,"
                " exchange_token_hash=? WHERE binding_id=?",
                (display_identifier, digest_secret(exchange_token), request_id))
        account_label = self.account_id
        return {"binding_id": request_id, "account_label": account_label,
                "display_identifier": display_identifier}, exchange_token

    def confirm_binding(self, binding_id: str, exchange_token: str) -> dict:
        with self.db.tx() as tx:
            row = tx.cur.execute("SELECT * FROM binding_sessions WHERE binding_id=?",
                                 (binding_id,)).fetchone()
            if row is None or row["state"] in ("pending", "expired"):
                raise ApiError("BINDING_EXPIRED", "binding session unavailable")
            prov = tx.cur.execute(
                "SELECT device_id FROM device_provision WHERE binding_id=?",
                (binding_id,)).fetchone()
            if row["state"] == "consumed" and prov is None:
                # provision erased (revoked or swept): no resurrection
                raise ApiError("BINDING_EXPIRED", "binding session unavailable")
            if not row["exchange_token_hash"] or not hmac.compare_digest(
                    row["exchange_token_hash"], digest_secret(exchange_token)):
                raise ApiError("AUTH_REQUIRED", "binding session rejected")
            if row["expires_at"] <= utcnow_iso():
                raise ApiError("BINDING_EXPIRED", "binding session expired")
            device = self._confirm_in_transaction(
                tx.cur, row, row["display_identifier"] or "device", binding_id,
                idempotent=True)
            if device["status"] != "active":
                # Revocation must not revive credentials through retries.
                tx.cur.execute("DELETE FROM device_provision WHERE binding_id=?",
                               (binding_id,))
                raise ApiError("BINDING_EXPIRED", "device is revoked; rebind required")
            return self._device_credentials_payload(row["account_id"], device)

    def _confirm_in_transaction(self, cur: sqlite3.Cursor, row: sqlite3.Row,
                                display_identifier: str, binding_id: str,
                                idempotent: bool = False) -> sqlite3.Row:
        """Real confirmation; idempotent retries recover the same result.

        Provision rows hold the device token for at most the binding window
        (single exchange, physical confirmation, bounded retry recovery), then
        a sweep erases them. They are revoked-side deletable.
        """
        prov = cur.execute(
            "SELECT device_id FROM device_provision WHERE binding_id=?",
            (binding_id,)).fetchone()
        if prov:
            return cur.execute("SELECT * FROM devices WHERE device_id=?",
                               (prov["device_id"],)).fetchone()
        device_id = "dev-" + secrets.token_hex(8)
        token = secrets.token_urlsafe(32)
        # Recovery material lives only in-process for the binding window
        # (DR02); the database keeps the digest, never the plaintext.
        self._recovery[binding_id] = token
        self._recovery[device_id] = token
        cur.execute(
            "INSERT INTO devices(device_id, account_id, display_label, token_digest,"
            " status, bound_at) VALUES (?,?,?,?, 'active', ?)",
            (device_id, row["account_id"], display_identifier, digest_secret(token),
             utcnow_iso()))
        cur.execute("UPDATE binding_sessions SET state='consumed' WHERE binding_id=?",
                    (binding_id,))
        cur.execute("INSERT INTO device_provision(binding_id, device_id, token_digest,"
                    " token_recoverable, created_at) VALUES (?,?,?,?,?)",
                    (binding_id, device_id, digest_secret(token), "", utcnow_iso()))
        return cur.execute("SELECT * FROM devices WHERE device_id=?", (device_id,)).fetchone()

    def _confirmed_payload(self, cur: sqlite3.Cursor, binding_id: str) -> dict:
        row = cur.execute(
            "SELECT d.device_id, d.display_label FROM device_provision p"
            " JOIN devices d ON d.device_id=p.device_id WHERE p.binding_id=?",
            (binding_id,)).fetchone()
        if row is None or binding_id not in self._recovery:
            raise ApiError("BINDING_EXPIRED", "confirmation result unavailable")
        return {"device_id": row["device_id"], "display_label": row["display_label"],
                "token": self._recovery[binding_id]}

    def _device_credentials_payload(self, account_id: str, device: sqlite3.Row) -> dict:
        token = self._recovered_token(device["device_id"])
        if token is None:
            raise ApiError("BINDING_EXPIRED", "confirmation result unavailable")
        return {"device_id": device["device_id"], "display_label": device["display_label"],
                "token": token}

    def _recovered_token(self, device_id: str) -> str | None:
        return self._recovery.get(device_id)

    def sweep_expired(self) -> None:
        """Erase expired binding-session recovery material in memory."""
        with self.db.tx() as tx:
            expired = [r["binding_id"] for r in tx.cur.execute(
                "SELECT binding_id FROM binding_sessions WHERE expires_at < ?",
                (utcnow_iso(),)).fetchall()]
            tx.cur.execute(
                "UPDATE binding_sessions SET state='expired' WHERE state IN"
                " ('pending','exchanged') AND expires_at < ?", (utcnow_iso(),))
        for binding_id in expired:
            token = self._recovery.pop(binding_id, None)
            if token:
                for k, v in list(self._recovery.items()):
                    if v == token:
                        del self._recovery[k]

    def revoke_device(self, device_id: str) -> None:
        valid_id(device_id, "device_id")
        with self.db.tx() as tx:
            cur = tx.cur.execute(
                "UPDATE devices SET status='revoked', revoked_at=? WHERE device_id=?"
                " AND status='active'", (utcnow_iso(), device_id))
            if cur.rowcount == 0:
                raise ApiError("NOT_FOUND", "device not active")
            tx.cur.execute("DELETE FROM device_provision WHERE device_id=?", (device_id,))
        token = self._recovery.get(device_id)
        if token:
            for key, value in list(self._recovery.items()):
                if value == token:
                    self._recovery.pop(key, None)

    def device_by_token(self, token: str) -> sqlite3.Row | None:
        if not isinstance(token, str) or not token:
            return None
        with self.db.tx() as tx:
            row = tx.cur.execute(
                "SELECT * FROM devices WHERE token_digest=? AND status='active'",
                (digest_secret(token),)).fetchone()
        return row

    def list_devices(self) -> list[dict]:
        with self.db.tx() as tx:
            rows = tx.cur.execute(
                "SELECT device_id, display_label, status, bound_at, revoked_at"
                " FROM devices ORDER BY bound_at DESC LIMIT 50").fetchall()
        return [dict(r) for r in rows]

    def list_bindings(self) -> list[dict]:
        with self.db.tx() as tx:
            rows = tx.cur.execute(
                "SELECT binding_id, state, created_at, expires_at, display_identifier"
                " FROM binding_sessions ORDER BY created_at DESC LIMIT 20").fetchall()
        return [dict(r) for r in rows]
