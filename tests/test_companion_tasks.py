"""CalDAV fixtures are synthetic; these tests do not establish SA-11 acceptance."""

from contextlib import redirect_stdout
import io
from http.client import IncompleteRead
import json
import os
from pathlib import Path
import sqlite3
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import Request, urlopen
from xml.sax.saxutils import escape

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from companion.tasks.__main__ import configure, read_config
from companion.tasks.caldav import CalDAV, NoRedirect, Transport, responses
from companion.tasks.common import MAX_DAV_BYTES, MAX_PAGE_BYTES, TaskError, packed
from companion.tasks.model import parse_resource
from companion.tasks.service import API, discover, refresh
from companion.tasks.store import Store
from companion.tasks.wsgi import create_app

BASE = "https://cloud.example.test/"
DAV = BASE + "remote.php/dav/"
PRINCIPAL = DAV + "principals/users/demo/"
HOME = DAV + "calendars/demo/"
COLLECTION = HOME + "tasks/"
NAMESPACES = 'xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav"'


def ics(fields="SUMMARY:Test task\r\n", uid="synthetic-id"):
    return ("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Passport Test//EN\r\n"
            "BEGIN:VTODO\r\nUID:" + uid + "\r\n" + fields + "END:VTODO\r\nEND:VCALENDAR\r\n").encode()


def row(href, props, code="200 OK"):
    return (f"<d:response><d:href>{escape(href)}</d:href><d:propstat><d:prop>"
            f"{props}</d:prop><d:status>HTTP/1.1 {code}</d:status></d:propstat></d:response>")


def multi(*rows):
    return (f"<d:multistatus {NAMESPACES}>" + "".join(rows) + "</d:multistatus>").encode()


def collection_row(href=COLLECTION, name="Work", component="VTODO"):
    return row(href, f"<d:resourcetype><d:collection/><c:calendar/></d:resourcetype>"
               f"<d:displayname>{name}</d:displayname><c:supported-calendar-component-set>"
               f'<c:comp name="{component}"/></c:supported-calendar-component-set>')


def resource_row(raw=None, href=COLLECTION + "one.ics", etag='"one"'):
    return row(href, "<d:getetag>" + escape(etag) + "</d:getetag><c:calendar-data>" +
               escape((raw or ics()).decode()) + "</c:calendar-data>")


class FakeTransport(Transport):
    def __init__(self):
        super().__init__(BASE, "demo", "fixture-only")
        self.calls = []
        self.reports = {}
        self.error = None
        self.listing = multi(collection_row(), collection_row(HOME + "events/", "Events", "VEVENT"))

    def request(self, method, href, body, depth):
        self.calls.append((method, href, body, depth))
        if self.error:
            raise TaskError(self.error)
        if href == DAV:
            return multi(row(DAV, f"<d:current-user-principal><d:href>{PRINCIPAL}</d:href></d:current-user-principal>"))
        if href == PRINCIPAL:
            return multi(row(PRINCIPAL, f"<c:calendar-home-set><d:href>{HOME}</d:href></c:calendar-home-set>"))
        if href == HOME:
            return self.listing
        if method == "REPORT":
            return self.reports.get(href, multi(resource_row()))
        raise AssertionError((method, href))


def parse(raw):
    return parse_resource(raw, account_id="account", collection_id="collection",
                          href=COLLECTION + "one.ics", etag='"one"', display_zone="Asia/Shanghai")


class ModelTests(unittest.TestCase):
    def test_unicode_folding_escapes_unknown_and_original(self):
        raw = ics("SUMMARY:中文\\,title\r\n continued\r\nDESCRIPTION:line1\\nline2\\;\\,\\\\\r\nX-EXTRA:preserve\r\n")
        item = parse(raw)[0]
        self.assertEqual(item["summary"], "中文,titlecontinued")
        self.assertEqual(item["note"], "line1\nline2;,\\")
        self.assertEqual(item["due"]["kind"], "none")
        self.assertTrue(item["read_only"])

    def test_all_due_kinds(self):
        cases = [
            ("DUE;VALUE=DATE:20261019", "date", "2026-10-19"),
            ("DUE:20261019T010000Z", "utc", "2026-10-19T09:00:00+08:00"),
            ("DUE;TZID=America/New_York:20261019T010000", "zoned", "2026-10-19T13:00:00+08:00"),
            ("DUE:20261019T010000", "floating", "2026-10-19T01:00:00"),
        ]
        for line, kind, display in cases:
            with self.subTest(line=line):
                value = parse(ics(line + "\r\n"))[0]["due"]
                self.assertEqual(value["kind"], kind)
                self.assertEqual(value["display"], display)
                if kind == "zoned":
                    self.assertEqual(value["tzid"], "America/New_York")

    def test_vtimezone(self):
        raw = ics("DUE;TZID=Custom/Test:20261019T010000\r\n")
        zone = ("BEGIN:VTIMEZONE\r\nTZID:Custom/Test\r\nBEGIN:STANDARD\r\nDTSTART:19700101T000000\r\n"
                "TZOFFSETFROM:+0530\r\nTZOFFSETTO:+0530\r\nEND:STANDARD\r\nEND:VTIMEZONE\r\n")
        raw = raw.replace(b"BEGIN:VTODO", zone.encode() + b"BEGIN:VTODO")
        self.assertEqual(parse(raw)[0]["due"]["display"], "2026-10-19T03:30:00+08:00")

    def test_unresolved_timezone_and_invalid_values_fail(self):
        for fields in ("DUE;TZID=Unknown/Zone:20261019T010000\r\n", "DUE:not-a-date\r\n",
                       "SUMMARY:first\r\nSUMMARY:second\r\n", "PERCENT-COMPLETE:101\r\n",
                       "STATUS:BOGUS\r\n"):
            with self.subTest(fields=fields), self.assertRaises(TaskError):
                parse(ics(fields))
        for raw in (b"not ical", ics().replace(b"UID:synthetic-id", b"UID:"), ics() + b"\xff"):
            with self.assertRaises(TaskError):
                parse(raw)

    def test_multibyte_note_preview_and_summary_limit(self):
        item = parse(ics("DESCRIPTION:" + "测" * 2000 + "\r\n"))[0]
        self.assertEqual(len(item["note"].encode()), 4095)
        self.assertTrue(item["note_truncated"])
        self.assertEqual(item["original_bytes"], 6000)
        with self.assertRaisesRegex(TaskError, "LIMIT_EXCEEDED"):
            parse(ics("SUMMARY:" + "测" * 171 + "\r\n"))

    def test_recurrence_and_subtasks_are_explicit_read_only(self):
        item = parse(ics("RRULE:FREQ=DAILY\r\nRECURRENCE-ID:20261019T010000Z\r\nRELATED-TO:parent\r\n"))[0]
        self.assertTrue(item["recurring"])
        self.assertTrue(item["has_parent"])
        self.assertEqual(item["recurrence_id"]["kind"], "utc")

    def test_master_and_explicit_instance_keep_different_identities(self):
        raw = ics("SUMMARY:Series\r\nRRULE:FREQ=DAILY\r\n")
        instance = ("BEGIN:VTODO\r\nUID:synthetic-id\r\nSUMMARY:Instance\r\n"
                    "RECURRENCE-ID;VALUE=DATE:20261019\r\nEND:VTODO\r\n")
        raw = raw.replace(b"END:VCALENDAR", instance.encode() + b"END:VCALENDAR")
        records = parse(raw)
        self.assertEqual(len(records), 2)
        self.assertIsNone(records[0]["recurrence_id"])
        self.assertEqual(records[1]["recurrence_id"]["kind"], "date")


class CalDAVTests(unittest.TestCase):
    def setUp(self):
        self.transport = FakeTransport()
        self.client = CalDAV(self.transport, "account")

    def test_discovery_report_and_preservation(self):
        collections = self.client.discover()
        self.assertEqual(len(collections), 1)
        self.assertEqual([call[3] for call in self.transport.calls], [0, 0, 1])
        records, originals = self.client.fetch(collections)
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["etag"], '"one"')
        self.assertIn(b"VTODO", self.transport.calls[-1][2])
        self.assertEqual(originals[0][2], ics().replace(b"\r\n", b"\n"))
        self.assertEqual(self.transport.calls[-1][0], "REPORT")

    def test_same_uid_in_different_lists_is_distinct(self):
        other = HOME + "other/"
        self.transport.listing = multi(collection_row(), collection_row(other))
        self.transport.reports[other] = multi(resource_row(href=other + "one.ics"))
        records, _ = self.client.fetch(self.client.discover())
        self.assertEqual(len(records), 2)
        self.assertNotEqual(records[0]["collection_id"], records[1]["collection_id"])

    def test_missing_props_and_duplicate_identity_abort(self):
        collections = self.client.discover()
        bad = [multi(row(COLLECTION + "one.ics", "<d:getetag>x</d:getetag>")),
               multi(resource_row(), resource_row(href=COLLECTION + "two.ics"))]
        for response in bad:
            self.transport.reports[COLLECTION] = response
            with self.assertRaises(TaskError):
                self.client.fetch(collections)

    def test_collection_and_item_limits(self):
        self.transport.listing = multi(*(collection_row(HOME + f"list{i}/") for i in range(21)))
        with self.assertRaisesRegex(TaskError, "LIMIT_EXCEEDED"):
            self.client.discover()
        self.transport.listing = multi(collection_row())
        self.transport.reports[COLLECTION] = multi(*(resource_row(ics(uid=f"id{i}"), COLLECTION + f"{i}.ics") for i in range(201)))
        with self.assertRaisesRegex(TaskError, "LIMIT_EXCEEDED"):
            self.client.fetch(self.client.discover())

    def test_xml_entities_and_wrong_roots_rejected(self):
        for raw in (b'<!DOCTYPE x [<!ENTITY x SYSTEM "file:///private">]><x>&x;</x>', b"<html>login</html>", b"<broken"):
            with self.assertRaisesRegex(TaskError, "INVALID_DAV"):
                responses(raw)

    def test_href_boundaries(self):
        for href in ("https://other.example.test/remote.php/dav/", "http://cloud.example.test/remote.php/dav/",
                     "/secret", DAV + "%2e%2e/secret", DAV + "%2e%2e%2fsecret", DAV + "x?secret=1",
                     DAV + "x#fragment", DAV + "x\\private", DAV + "x\r\nHeader:secret"):
            with self.subTest(href=href), self.assertRaisesRegex(TaskError, "UNSAFE_HREF"):
                self.transport.resolve(href)
        self.assertEqual(self.transport.resolve("one.ics", COLLECTION), COLLECTION + "one.ics")

    def test_transport_does_not_write_or_follow_redirect(self):
        real = Transport(BASE, "demo", "fixture-only")
        with self.assertRaisesRegex(TaskError, "READ_ONLY"):
            real.request("PUT", DAV, b"", 0)
        with self.assertRaisesRegex(TaskError, "REDIRECT_REJECTED"):
            NoRedirect().redirect_request(None, None, 302, "", {}, "https://other.test")
        for code, expected in ((401, "AUTH_FAILED"), (403, "AUTH_FAILED"), (500, "DAV_STATUS")):
            with patch.object(real.opener, "open", side_effect=HTTPError(DAV, code, "PRIVATE BODY", {}, None)):
                with self.assertRaisesRegex(TaskError, expected):
                    real.request("PROPFIND", DAV, b"", 0)

    def test_response_bound(self):
        class Response(io.BytesIO):
            status = 207
            headers = {}
        real = Transport(BASE, "demo", "fixture-only")
        with patch.object(real.opener, "open", return_value=Response(b" " * (MAX_DAV_BYTES + 1))):
            with self.assertRaisesRegex(TaskError, "LIMIT_EXCEEDED"):
                real.request("PROPFIND", DAV, b"", 0)

    def test_partial_body_and_response_deadline(self):
        class Partial(io.BytesIO):
            status = 207
            headers = {}

            def read1(self, size):
                raise IncompleteRead(b"private task fragment")

        real = Transport(BASE, "demo", "fixture-only")
        with patch.object(real.opener, "open", return_value=Partial()):
            with self.assertRaisesRegex(TaskError, "UNAVAILABLE"):
                real.request("REPORT", COLLECTION, b"", 1)
        class Slow(io.BytesIO):
            status = 207
            headers = {}
        with patch.object(real.opener, "open", return_value=Slow(b"x")), \
                patch("companion.tasks.caldav.time.monotonic", side_effect=[0, 61]):
            with self.assertRaisesRegex(TaskError, "UNAVAILABLE"):
                real.request("REPORT", COLLECTION, b"", 1)


class CacheFixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.now = 100000
        self.store = Store(Path(self.temp.name) / "tasks.sqlite3", "account", clock=lambda: self.now)
        self.transport = FakeTransport()
        self.client = CalDAV(self.transport, "account")
        self.collections = discover(self.client, self.store)
        self.store.select([self.collections[0]["id"]], 0)


class CacheTests(CacheFixture):
    def test_active_view_preserves_terminal_history_and_invalidates_old_cursors(self):
        records, originals = [], []
        for status in ("NEEDS-ACTION", "COMPLETED", "IN-PROCESS", "CANCELLED"):
            raw = ics("STATUS:" + status + "\r\n", uid=status)
            records.extend(parse_resource(raw, account_id="account",
                           collection_id=self.collections[0]["id"],
                           href=COLLECTION + status + ".ics", etag='"one"', display_zone="UTC"))
            originals.append((COLLECTION + status + ".ics", '"one"', raw))
        revision = self.store.publish(records, originals, 1)
        before = self.store.state()
        page = self.store.page()
        self.assertEqual(page["total"], 2)
        self.assertEqual(page["state"], "READY")
        self.assertEqual([r["status"] for r in page["items"]], ["NEEDS-ACTION", "IN-PROCESS"])
        self.assertTrue(all(r["snapshot_revision"] == revision for r in page["items"]))
        self.assertEqual(page["snapshot_revision"], revision)
        self.assertEqual(page["downloaded_at"], before["downloaded"])
        self.assertEqual(self.store.state(), before)
        self.assertEqual(len(json.loads(before["snapshot"])), 4)
        with self.store.connect() as db:
            self.assertEqual([(r["href"], r["etag"], r["body"])
                              for r in db.execute("SELECT * FROM originals ORDER BY href")],
                             sorted(originals))
        with self.assertRaisesRegex(TaskError, "REVISION_CONFLICT"):
            self.store.page(before["revision"], 1)
        with self.assertRaisesRegex(TaskError, "INVALID_CURSOR"):
            self.store.page(revision, 3)

    def test_terminal_only_view_is_empty_with_stale_and_auth_states_retained(self):
        self.store.publish(parse(ics("STATUS:COMPLETED\r\n")), [], 1)
        reopened = Store(self.store.path, "account", clock=lambda: self.now)
        page = reopened.page()
        self.assertEqual((page["state"], page["total"], page["items"], page["next_offset"]),
                         ("EMPTY", 0, [], None))
        self.now += 86400
        self.assertEqual(reopened.page()["state"], "STALE")
        reopened.failure("AUTH_FAILED")
        self.assertEqual(reopened.page()["state"], "AUTH_FAILED")

    def test_refresh_offline_stale_auth_halt_and_restart(self):
        revision = refresh(self.client, self.store)
        self.assertEqual(self.store.page()["state"], "READY")
        self.now += 86400
        self.assertEqual(self.store.page()["state"], "STALE")
        self.transport.error = "UNAVAILABLE"
        with self.assertRaises(TaskError):
            refresh(self.client, self.store)
        self.assertEqual(self.store.page()["snapshot_revision"], revision)
        self.transport.error = "AUTH_FAILED"
        with self.assertRaises(TaskError):
            refresh(self.client, self.store)
        calls = len(self.transport.calls)
        self.transport.error = None
        with self.assertRaisesRegex(TaskError, "AUTH_HALTED"):
            refresh(self.client, self.store)
        self.assertEqual(calls, len(self.transport.calls))
        reopened = Store(self.store.path, "account", clock=lambda: self.now)
        self.assertEqual(reopened.page()["state"], "AUTH_FAILED")
        reopened.resume_auth()
        refresh(self.client, reopened)
        self.assertEqual(reopened.page()["state"], "READY")

    def test_selection_compare_and_swap_and_missing_collection(self):
        revision = refresh(self.client, self.store)
        with self.assertRaisesRegex(TaskError, "REVISION_CONFLICT"):
            self.store.select([], 0)
        with self.assertRaisesRegex(TaskError, "UNKNOWN_COLLECTION"):
            self.store.select(["missing"], 1)
        self.transport.listing = multi()
        with self.assertRaisesRegex(TaskError, "COLLECTION_MISSING"):
            refresh(self.client, self.store)
        self.assertEqual(self.store.page()["snapshot_revision"], revision)

    def test_selection_changed_during_refresh_never_publishes(self):
        refresh(self.client, self.store)
        records, originals = self.client.fetch(self.collections)
        self.store.select([], 1)
        with self.assertRaisesRegex(TaskError, "REVISION_CONFLICT"):
            self.store.publish(records, originals, 1)
        with self.assertRaisesRegex(TaskError, "SNAPSHOT_NOT_READY"):
            self.store.page()
        refresh(self.client, self.store)
        self.assertEqual(self.store.page()["state"], "EMPTY")

    def test_atomic_rollback_restores_originals_and_snapshot(self):
        revision = refresh(self.client, self.store)
        records, originals = self.client.fetch(self.collections)
        # Duplicate primary key fails after DELETE inside the same transaction.
        with self.assertRaises(sqlite3.IntegrityError):
            self.store.publish(records, originals + originals, 1)
        self.assertEqual(self.store.page()["snapshot_revision"], revision)
        with self.store.connect() as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM originals").fetchone()[0], 1)

    def test_overflow_preserves_snapshot(self):
        revision = refresh(self.client, self.store)
        item = self.store.page()["items"][0]
        with self.assertRaisesRegex(TaskError, "LIMIT_EXCEEDED"):
            self.store.publish([item] * 201, [], 1)
        self.assertEqual(self.store.page()["snapshot_revision"], revision)

    def test_page_bound_revision_cursor_and_account_isolation(self):
        item = parse(ics("DESCRIPTION:" + "a" * 4096 + "\r\n"))[0]
        items = [dict(item, uid=str(i), status="COMPLETED" if i % 3 == 0 else
                      "CANCELLED" if i % 3 == 1 else "IN-PROCESS") for i in range(60)]
        revision = self.store.publish(items, [], 1)
        seen, offset = [], 0
        while offset is not None:
            page = self.store.page(revision, offset)
            self.assertLessEqual(len(packed(page)), MAX_PAGE_BYTES)
            self.assertEqual(page["total"], 20)
            self.assertTrue(all(i["snapshot_revision"] == revision for i in page["items"]))
            seen.extend(i["uid"] for i in page["items"])
            offset = page["next_offset"]
        self.assertEqual(seen, [str(i) for i in range(60) if i % 3 == 2])
        self.store.publish(items, [], 1)
        with self.assertRaisesRegex(TaskError, "REVISION_CONFLICT"):
            self.store.page(revision, 1)
        with self.assertRaisesRegex(TaskError, "INVALID_CURSOR"):
            self.store.page(offset=1)
        with self.assertRaisesRegex(TaskError, "ACCOUNT_MISMATCH"):
            Store(self.store.path, "different")


class APITests(CacheFixture):
    def setUp(self):
        super().setUp()
        self.api = API(self.store, "device-" * 6, "admin-" * 7)

    def request(self, method, path, token="device-" * 6, body=b"", query=""):
        statuses = []
        env = {"REQUEST_METHOD": method, "PATH_INFO": path, "QUERY_STRING": query,
               "HTTP_AUTHORIZATION": "Bearer " + token, "CONTENT_LENGTH": str(len(body)),
               "wsgi.input": io.BytesIO(body)}
        data = b"".join(self.api(env, lambda status, headers: statuses.append((status, dict(headers)))))
        self.assertEqual(statuses[0][1]["Cache-Control"], "no-store")
        return statuses[0][0], json.loads(data)

    def test_existing_snapshot_is_filtered_without_refetch_or_database_rewrite(self):
        records = parse(ics("STATUS:COMPLETED\r\n", uid="closed"))
        records += parse(ics("STATUS:IN-PROCESS\r\nPERCENT-COMPLETE:50\r\n", uid="open"))
        revision = self.store.publish(records, [], 1)
        before = self.store.state()
        status, body = self.request("GET", "/suite/v1/tasks/snapshot")
        self.assertEqual(status, "200 OK")
        self.assertEqual(body["total"], 1)
        self.assertEqual(body["items"][0]["uid"], "open")
        self.assertEqual(body["items"][0]["snapshot_revision"], revision)
        self.assertEqual(self.store.state(), before)
        status, body = self.request("GET", "/suite/v1/tasks/snapshot",
                                    query="revision=" + before["revision"] + "&offset=1")
        self.assertEqual((status, body["error"]), ("409 Conflict", "REVISION_CONFLICT"))

    def test_auth_scopes_and_no_completion(self):
        status, body = self.request("GET", "/suite/v1/task-collections", token="wrong")
        self.assertTrue(status.startswith("401"))
        self.assertNotIn("collections", body)
        status, _ = self.request("PUT", "/suite/v1/task-selection", body=packed({"revision": 1, "collection_ids": []}))
        self.assertTrue(status.startswith("403"))
        status, body = self.request("PUT", "/suite/v1/task-selection", token="admin-" * 7,
                                    body=packed({"revision": 1, "collection_ids": []}))
        self.assertEqual(status, "200 OK")
        self.assertEqual(body["selection_revision"], 2)
        status, body = self.request("POST", "/suite/v1/task-completions")
        self.assertEqual((status,body["error"]),("403 Forbidden","READ_ONLY"))

    def test_cached_page_and_bad_queries(self):
        refresh(self.client, self.store)
        status, body = self.request("GET", "/suite/v1/tasks/snapshot")
        self.assertEqual(status, "200 OK")
        self.assertEqual(body["total"], 1)
        for query in ("offset=-1", "offset=bad", "offset=1", "offset=0&offset=1", "unexpected=x"):
            status, _ = self.request("GET", "/suite/v1/tasks/snapshot", query=query)
            self.assertTrue(status.startswith("400"))

    def test_private_config_no_echo_or_overwrite(self):
        path = Path(self.temp.name) / "config.json"
        output = io.StringIO()
        with patch("builtins.input", return_value=BASE), \
                patch("getpass.getpass", side_effect=["demo", "private-fixture"]), redirect_stdout(output):
            configure(path)
        self.assertNotIn("private-fixture", output.getvalue())
        config = read_config(path)
        self.assertNotEqual(config["device_token"], config["admin_token"])
        with self.assertRaisesRegex(TaskError, "CONFIG_EXISTS"):
            configure(path)

    def test_production_factory_does_not_fetch_or_print_secrets(self):
        path = Path(self.temp.name) / "config.json"
        with patch("builtins.input", return_value=BASE), \
                patch("getpass.getpass", side_effect=["demo", "private-fixture"]):
            configure(path)
        with patch.dict("os.environ", {"SUITE_TASKS_CONFIG": str(path),
                                       "SUITE_TASKS_DATABASE": str(Path(self.temp.name) / "api.sqlite3")}), \
                patch.object(Transport, "request", side_effect=AssertionError("No network on startup")):
            self.assertIsInstance(create_app(), API)
        with patch.dict("os.environ", {"SUITE_TASKS_CONFIG": "private-missing-path"}):
            with self.assertRaisesRegex(RuntimeError, "configuration or storage") as caught:
                create_app()
            self.assertNotIn("private-missing-path", str(caught.exception))

    @unittest.skipIf(os.name == "nt", "Gunicorn runs on the Linux deployment host")
    def test_gunicorn_loopback_runtime(self):
        path = Path(self.temp.name) / "config.json"
        with patch("builtins.input", return_value=BASE), \
                patch("getpass.getpass", side_effect=["demo", "private-fixture"]):
            configure(path)
        config = read_config(path)
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        env = dict(os.environ, SUITE_TASKS_CONFIG=str(path),
                   SUITE_TASKS_DATABASE=str(Path(self.temp.name) / "runtime.sqlite3"),
                   PYTHONDONTWRITEBYTECODE="1")
        repo = Path(__file__).resolve().parents[1]
        with subprocess.Popen([sys.executable, "-B", "-m", "gunicorn", "--config",
                               "companion/deploy/gunicorn.conf.py", "--bind", f"127.0.0.1:{port}",
                               "companion.tasks.wsgi:create_app()"], cwd=repo, env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) as process:
            try:
                url = f"http://127.0.0.1:{port}/suite/v1/task-collections"
                deadline = time.monotonic() + 30
                while True:
                    self.assertIsNone(process.poll(), "Gunicorn exited before serving requests")
                    try:
                        with urlopen(Request(url, headers={"Authorization": "Bearer " + config["device_token"]}), timeout=1) as response:
                            self.assertEqual(json.load(response)["collections"], [])
                        break
                    except OSError:
                        if time.monotonic() >= deadline:
                            self.fail("Gunicorn loopback startup timed out")
                        time.sleep(0.1)
                with self.assertRaises(HTTPError) as caught:
                    urlopen(url, timeout=2)
                self.assertEqual(caught.exception.code, 401)
                caught.exception.close()
            finally:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
