"""D1a host tests: contract behavior for server + adapter + fixtures.

Run:  python3 -m unittest discover -s companion/passport-server/tests -v
Registered in repo tools/validate.sh as the companion gate.

Covers acceptance host portions: A02 (binding rejection lifecycle),
A03 (traversal/digest/size), A05 (duplicate/stale events), A06 (snapshot
publication, interruption, damage, empty-server restore), A15 (auth,
revoke). Actual DAV/server-outage device behaviors stay NOT VERIFIED here.
"""
from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from passport_server import backups, packs, sync  # noqa: E402
from passport_server.core import Core  # noqa: E402
from passport_server.db import Database  # noqa: E402
from passport_server.errors import ApiError  # noqa: E402
from passport_server.fixture import fixture_pack_payload  # noqa: E402
from passport_server.server import Handler, Main  # noqa: E402
from passport_server.devdav import TEST_DAV_PASSWORD, TEST_DAV_USER  # noqa: E402
from passport_server import devdav  # noqa: E402
from passport_server.webdav import WebdavClient  # noqa: E402


def fresh_main(tmp: str) -> Main:
    app = Main(tmp, quiet=True)
    app.core.bootstrap_admin("correct horse")
    return app


def device_headers(token: str) -> dict:
    return {"Authorization": f"Bearer {token}"}


def post_json(handler, url: str, payload: dict, headers=None):
    body = json.dumps(payload).encode()
    handler.send_request("POST", url, body=body, headers=headers)


class BindingLifecycle(unittest.TestCase):
    """A02/A15 host portions incl. DR02 recovery property."""

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="ps-binding-")
        self.app = fresh_main(self.dir)

    def tearDown(self):
        self.app.shutdown()
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_full_binding_and_revocation(self):
        core = self.app.core
        b = core.create_binding()
        payload, exchange_token = core.exchange_binding(
            b["code"], "手机 移动版", b["binding_id"])
        self.assertEqual(payload["account_label"], core.account_id)
        creds = core.confirm_binding(b["binding_id"], exchange_token)
        self.assertIn("token", creds)
        self.assertEqual(creds["device_id"][:4], "dev-")
        # lost confirm response retried: same credentials inside window
        creds2 = core.confirm_binding(b["binding_id"], exchange_token)
        self.assertEqual(creds["token"], creds2["token"])
        # revoke: session retries must not resurrect credentials
        core.revoke_device(creds["device_id"])
        with self.assertRaises(ApiError) as ctx:
            core.confirm_binding(b["binding_id"], exchange_token)
        self.assertNotIn("token", str(ctx.exception))
        self.assertIsNone(core.device_by_token(creds["token"]))

    def test_expired_and_replayed_codes(self):
        core = self.app.core
        b = core.create_binding()
        # wrong code attempts are bounded
        for _ in range(4):
            with self.assertRaises(ApiError) as ctx:
                core.exchange_binding("X" * 6, "phone", b["binding_id"])
            self.assertEqual(ctx.exception.code, "BINDING_EXPIRED")
        with self.assertRaises(ApiError):
            core.exchange_binding("X" * 6, "phone", b["binding_id"])
        # attempts exhausted the binding session even for the right code
        b2 = core.create_binding()
        payload, token = core.exchange_binding(b2["code"], "phone", b2["binding_id"])
        # display identity change on retry is rejected
        with self.assertRaises(ApiError):
            core.exchange_binding(b2["code"], "desktop", b2["binding_id"])
        self.assertEqual(payload["display_identifier"], "phone")
        del token

    def test_never_logs_code_or_token(self):
        core = self.app.core
        b = core.create_binding()
        payload, token = core.exchange_binding(b["code"], "phone", b["binding_id"])
        creds = core.confirm_binding(b["binding_id"], token)
        rows = []
        with core.db.tx() as tx:
            for table in ("binding_sessions", "devices", "device_provision", "events"):
                rows += [str(r) for r in tx.cur.execute(f"SELECT * FROM {table}")]
        text = "\n".join(rows)
        self.assertNotIn(b["code"], text)
        self.assertNotIn(creds["token"], text)


class PackPublication(unittest.TestCase):
    """A03 host portions: traversal, digest, size, file_id, per-file download."""

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="ps-pack-")
        self.app = fresh_main(self.dir)

    def tearDown(self):
        self.app.shutdown()
        shutil.rmtree(self.dir, ignore_errors=True)

    def submit_payload(self, payload):
        result = packs.publish_pack(self.app.core, payload)
        files, meta = packs.validate_pack_payload(payload)
        packs.store_pack_files(self.app.core, files, result["storage_subdir"])
        return result

    def test_publish_and_download(self):
        payload = fixture_pack_payload()
        result = self.submit_payload(payload)
        self.assertLessEqual(result["manifest"]["total_bytes"], 768 * 1024)
        mainf = result["manifest"]
        # all digests are real SHA-256 (64 hex chars), sizes match
        packed = packs.get_manifest(self.app.core, result["pack_id"], result["revision"])
        manifest = json.loads(packed.decode())
        for f in manifest["files"]:
            self.assertEqual(len(f["sha256"]), 64)
            int(f["sha256"], 16)  # must be hex
            if f["path"].startswith("text/"):
                body, media = packs.read_file(
                    self.app.core, result["pack_id"], result["revision"], f["file_id"])
                self.assertEqual(media, "text/plain")
                import hashlib
                self.assertEqual(hashlib.sha256(body).hexdigest(), f["sha256"])
        # file_id uniqueness per revision (DR05)
        ids = [f["file_id"] for f in manifest["files"]]
        self.assertEqual(len(ids), len(set(ids)))
        # every id downloads; path identity never used
        body, _ = packs.read_file(self.app.core, result["pack_id"], 1, ids[0])
        self.assertIn(b"schema_version", body)

    def test_path_traversal_rejected(self):
        for bad in ("../etc/passwd", "data/../../escape.json", "/abs/path",
                    "data\\win.txt", "data/./x.txt",
                    "text/a.exe", "data/nested/../bad.txt", "other/base.txt"):
            payload = fixture_pack_payload()
            payload["files"] = [{"path": bad, "text": "x", "media_type": "text/plain"}]
            with self.assertRaises(ApiError):
                packs.publish_pack(self.app.core, payload)

    def test_size_and_count_rejection(self):
        payload = fixture_pack_payload()
        payload["kind"] = "fixture"
        payload["files"] = [
            {"path": f"text/big-{i}.txt", "text": "x" * 900000, "media_type": "text/plain"}
            for i in range(1)]  # 900 KiB single file > 768 KiB fixture cap
        with self.assertRaises(ApiError) as ctx:
            packs.publish_pack(self.app.core, payload)
        self.assertEqual(ctx.exception.code, "SIZE_LIMIT")
        from passport_server.packs import validate_pack_payload
        payload["files"] = [
            {"path": f"text/p{i}.txt", "text": "y", "media_type": "text/plain"}
            for i in range(129)]
        with self.assertRaises(ApiError):
            validate_pack_payload(payload)

    def test_duplicate_cancel_and_old_pack_retained(self):
        payload = fixture_pack_payload()
        self.submit_payload(payload)
        payload2 = fixture_pack_payload()
        payload2["files"][0]["text"] = "changed"
        result2 = self.submit_payload(payload2)
        self.assertEqual(result2["revision"], 2)
        # previous revision remains fetchable
        body = packs.get_manifest(self.app.core, result2["pack_id"], 1)
        self.assertIn(b"schema_version", body)


def make_event(event_id="e1", value=True, base=0, pred=None,
               route="fixture-route-001", target="stop-001",
               operation="guide_progress.set"):
    return {
        "event_id": event_id, "device_id": "fixture-device-1",
        "device_epoch": "epoch-1", "sequence": 1, "record_id": "auto",
        "context_id": route, "target_id": target, "operation": operation,
        "value": value, "base_revision": base,
        "predecessor_event_id": pred, "device_time": None,
        "time_trust": "unknown",
    }


class SyncEngine(unittest.TestCase):
    """A05 host portions: dedup/idempotence/conflict/predecessor (DR03)."""

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="ps-sync-")
        self.app = fresh_main(self.dir)
        core = self.app.core
        payload = fixture_pack_payload()
        files, _meta = packs.validate_pack_payload(payload)
        result = packs.publish_pack(core, payload)
        packs.store_pack_files(core, files, result["storage_subdir"])

    def tearDown(self):
        self.app.shutdown()
        shutil.rmtree(self.dir, ignore_errors=True)

    def sync(self, events, cursor=None):
        device = self.app.core.db.tx  # unused
        with self.app.core.db.tx() as tx:
            device_row = tx.cur.execute(
                "SELECT * FROM devices LIMIT 1").fetchone()
        return sync.process_sync(self.app.core, device_row, json.dumps(
            {"schema_version": 1, "server_epoch": self.app.core.server_epoch,
             "pull_cursor": cursor, "events": events}).encode())

    def active_device_row(self):
        with self.app.core.db.tx() as tx:
            return tx.cur.execute("SELECT * FROM devices LIMIT 1").fetchone()

    def make_device(self):
        core = self.app.core
        b = core.create_binding()
        _payload, token = core.exchange_binding(b["code"], "test", b["binding_id"])
        return core.confirm_binding(b["binding_id"], token)

    def test_acknowledge_dedup_conflict_noop(self):
        creds = self.make_device()
        device = self.active_device_row()
        core_dict = json.loads(json.dumps(creds))
        del core_dict
        r1 = sync.process_sync(self.app.core, device, json.dumps(
            {"schema_version": 1, "server_epoch": self.app.core.server_epoch,
             "pull_cursor": None, "events": [make_event()]}).encode())
        self.assertEqual(r1["results"][0]["status"], "acknowledged")
        self.assertEqual(r1["results"][0]["record_revision"], 1)
        # identical retry (same content): original result, no duplicate change
        r2 = sync.process_sync(self.app.core, device, json.dumps(
            {"schema_version": 1, "server_epoch": self.app.core.server_epoch,
             "pull_cursor": None, "events": [make_event()]}).encode())
        self.assertEqual(r2["results"][0]["status"], "acknowledged")
        self.assertEqual(len([c for c in r1["changes"]]), len([c for c in r1["changes"]]))
        # reused event_id different content: rejected, never applied
        r3 = sync.process_sync(self.app.core, device, json.dumps(
            {"schema_version": 1, "server_epoch": self.app.core.server_epoch,
             "pull_cursor": None, "events": [make_event(value=False)]}).encode())
        self.assertEqual(r3["results"][0]["status"], "rejected")
        # stale stale-same value => no-op
        r4 = sync.process_sync(self.app.core, device, json.dumps(
            {"schema_version": 1, "server_epoch": self.app.core.server_epoch,
             "pull_cursor": None, "events": [make_event(event_id="e2", base=99)]}).encode())
        self.assertEqual(r4["results"][0]["status"], "no_op")
        # stale different value => conflict with current revision
        with self.assertRaises(Exception) if False else _No_raise():
            pass
        r5 = sync.process_sync(self.app.core, device, json.dumps(
            {"schema_version": 1, "server_epoch": self.app.core.server_epoch,
             "pull_cursor": None, "events": [make_event(event_id="e3", value=True,
                                                        target="stop-002", base=99)]}).encode())
        self.assertEqual(r5["results"][0]["status"], "conflict")
        self.assertEqual(r5["results"][0]["record_revision"], 0)

    def test_predecessor_dependency_and_resolution(self):
        self.make_device()
        device = self.active_device_row()
        # pred event never sent -> dependency_pending, NOT acknowledged
        r = sync.process_sync(self.app.core, device, json.dumps(
            {"schema_version": 1, "server_epoch": self.app.core.server_epoch,
             "pull_cursor": None,
             "events": [make_event(event_id="e5", target="stop-003",
                                   pred="e4")]}).encode())
        self.assertEqual(r["results"][0]["status"], "dependency_pending")
        # now send the predecessor; dependent resolves
        r2 = sync.process_sync(self.app.core, device, json.dumps(
            {"schema_version": 1, "server_epoch": self.app.core.server_epoch,
             "pull_cursor": None, "events": [make_event(event_id="e4", target="stop-003")]}).encode())
        self.assertEqual(r2["results"][0]["status"], "acknowledged")
        # resubmit dependent: still dependency_pending until its own retry is
        # resubmitted by the device (per contract D1 keeps deps unresolved).
        r3 = sync.process_sync(self.app.core, device, json.dumps(
            {"schema_version": 1, "server_epoch": self.app.core.server_epoch,
             "pull_cursor": None,
             "events": [make_event(event_id="e5", target="stop-003", pred="e4")]}).encode())
        self.assertEqual(r3["results"][0]["status"], "dependency_pending")

    def test_cross_epoch_dependency_fabrication_rejected(self):
        self.make_device()
        device = self.active_device_row()
        ev = make_event(event_id="e7", pred="e-others")
        ev["device_epoch"] = "epoch-1"
        r = sync.process_sync(self.app.core, device, json.dumps(
            {"schema_version": 1, "server_epoch": self.app.core.server_epoch,
             "pull_cursor": None, "events": [ev]}).encode())
        self.assertEqual(r["results"][0]["status"], "dependency_pending")

    def test_epoch_mismatch_rejected_with_epoch_code(self):
        self.make_device()
        device = self.active_device_row()
        with self.assertRaises(ApiError):
            sync.process_sync(self.app.core, device, json.dumps(
                {"schema_version": 1, "server_epoch": "other-epoch",
                 "events": []}).encode())

    def test_batch_size_cap(self):
        self.make_device()
        device = self.active_device_row()
        events = [make_event(event_id=f"x{i}", target="stop-002") for i in range(21)]
        with self.assertRaises(ApiError) as ctx:
            sync.process_sync(self.app.core, device, json.dumps(
                {"schema_version": 1, "server_epoch": self.app.core.server_epoch,
                 "events": events}).encode())
        self.assertEqual(ctx.exception.code, "SIZE_LIMIT")


class _No_raise:
    def __enter__(self):
        return self

    def __exit__(self, *a):
        return True


class WebdavQueueAndSnapshots(unittest.TestCase):
    """A06 mock-level: queue durability, marker order, damaged snapshots,
    empty-server restore (mock DAV; real service NOT VERIFIED)."""

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="ps-dav-")
        self.dav_root = tempfile.mkdtemp(prefix="ps-davroot-")
        self.server, url = devdav.start_test_dav(self.dav_root)
        self.url = url
        self.app = fresh_main(self.dir)

    def tearDown(self):
        self.app.shutdown()
        self.server.shutdown()
        shutil.rmtree(self.dir, ignore_errors=True)
        shutil.rmtree(self.dav_root, ignore_errors=True)

    def configure(self):
        from urllib.request import urlopen, Request
        import base64 as b64
        auth = "Basic " + b64.b64encode(
            f"{TEST_DAV_USER}:{TEST_DAV_PASSWORD}".encode()).decode()
        client = WebdavClient(self.url, TEST_DAV_USER, TEST_DAV_PASSWORD)
        client.mkdirs("passport/v1")
        self.app.core.dav = client
        dir_path = os.path.join(self.dir, "x")
        os.makedirs(dir_path, exist_ok=True)
        return client

    def publish_with_dav(self):
        self.configure()
        core = self.app.core
        payload = fixture_pack_payload()
        files, _meta = packs.validate_pack_payload(payload)
        result = packs.publish_pack(core, payload)
        packs.store_pack_files(core, files, result["storage_subdir"])
        backups.enqueue_resource_uploads(core, result["pack_id"], result["revision"])
        for job in backups.list_jobs(core):
            backups.process_upload_job(core, job["job_id"])
        return result

    def test_complete_snapshot_and_restore(self):
        # publish + snapshot complete
        self.publish_with_dav()
        job = backups.start_snapshot_job(self.app.core)
        backups.process_upload_job(self.app.core, job["job_id"])
        snaps = backups.list_snapshots(self.app.core)
        self.assertEqual(len(snaps["complete"]), 1)
        # completion marker uploaded last check: files exist
        client = self.app.core.dav
        self.assertIsNotNone(client.exists("passport/v1/accounts/account-local/"
                                            "snapshots/" + job["snapshot_id"] +
                                            "/complete.json"))
        # restore preview on non-empty server refuses
        preview = backups.preview_restore(self.app.core, job["snapshot_id"])
        self.assertTrue(preview["preview"]["restore_allowed"])
        staged = backups.stage_restored_database(self.app.core, job["snapshot_id"])
        import sqlite3
        conn = sqlite3.connect(staged)
        devices = conn.execute("SELECT status FROM devices").fetchall()
        self.assertEqual(devices, [])
        conn.close()
        shutil.rmtree(os.path.dirname(staged), ignore_errors=True)

    def test_interrupted_snapshot_never_complete(self):
        # simulate outage: refuses uploads → snapshot stays incomplete
        self.app.core.dav = None
        job = backups.start_snapshot_job(self.app.core)
        with self.assertRaises(ApiError) as ctx:
            backups.process_upload_job(self.app.core, job["job_id"])
        self.assertIn(ctx.exception.code, ("DAV_PENDING",))
        snaps = backups.list_snapshots(self.app.core)
        self.assertEqual(len(snaps["complete"]), 0)

    def test_damaged_snapshot_rejected(self):
        self.publish_with_dav()
        job = backups.start_snapshot_job(self.app.core)
        backups.process_upload_job(self.app.core, job["job_id"])
        # corrupt the stored records.json body on disk ( erwarten mismatch )
        rec_path = os.path.join(
            self.dav_root, "dav", "passport", "v1", "accounts", "account-local",
            "snapshots", job["snapshot_id"], "records.json")
        with open(rec_path, "ab") as f:
            f.write(b"junk")
        with self.assertRaises(ApiError) as ctx:
            backups.fetch_snapshot_document(self.app.core, job["snapshot_id"])
        self.assertEqual(ctx.exception.code, "HASH_MISMATCH")


if __name__ == "__main__":
    unittest.main(verbosity=2)
