"""Synthetic translation T1 core/job boundaries. No provider or account calls."""

import hashlib
import io
import json
import os
from pathlib import Path
import sqlite3
import struct
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from companion.translation.common import (MAX_TEXT_BYTES, MAX_TEXT_SCALARS,
                                           MAX_WAV_BYTES, TranslationError,
                                           validate_direction, validate_job_id,
                                           validate_request, validate_text,
                                           validate_wav)
from companion.translation.mock import SyntheticProvider
from companion.translation.service import TranslationAPI, TranslationJobs
from companion.translation.store import TranslationStore


def wave(payload=9600):
    return (struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36 + payload,
                        b"WAVE", b"fmt ", 16, 1, 1, 16000, 32000, 2, 16,
                        b"data", payload) + b"\0" * payload)


def request(number, raw, source="en", target="zh-CN"):
    return {
        "schema": 1,
        "operation_id": f"{number:032x}",
        "source_lang": source,
        "target_lang": target,
        "format": "wav",
        "audio_sha256": hashlib.sha256(raw).hexdigest(),
        "audio_bytes": len(raw),
    }


class Fixture(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / "translation.sqlite3"
        self.clock_value = 1000
        self.store = TranslationStore(self.path, clock=lambda: self.clock_value)
        self.provider = SyntheticProvider()
        self.jobs = TranslationJobs(self.store, self.provider,
                                    clock=lambda: self.clock_value)
        self.raw = wave()
        self.body = request(1, self.raw)

    def tearDown(self):
        self.tmp.cleanup()

    def assert_code(self, code, call):
        with self.assertRaises(TranslationError) as caught:
            call()
        self.assertEqual(caught.exception.code, code)


class ValidationTests(Fixture):
    def test_text_scalar_byte_and_encoding_limits(self):
        self.assertEqual(validate_text("a" * 512), "a" * 512)
        self.assert_code("TEXT_LIMIT", lambda: validate_text("a" * 513))
        self.assertEqual(validate_text("\U0001f600" * 512),
                         "\U0001f600" * 512)
        self.assert_code("TEXT_LIMIT",
                         lambda: validate_text("\U0001f600" * 513))
        for value in ("", "a\0b", "\ud800"):
            self.assert_code("INVALID_TEXT", lambda value=value: validate_text(value))
        self.assertEqual(MAX_TEXT_BYTES, 2048)
        self.assertEqual(MAX_TEXT_SCALARS, 512)

    def test_direction_is_explicit(self):
        validate_direction("en", "zh-CN")
        validate_direction("zh-CN", "en")
        for source, target in (("en", "en"), ("zh-CN", "zh-CN"),
                               ("fr", "en"), (None, "en")):
            self.assert_code("INVALID_DIRECTION",
                             lambda source=source, target=target:
                             validate_direction(source, target))

    def test_wav_envelope_and_limits(self):
        info = validate_wav(wave(9600))
        self.assertEqual((info["bytes"], info["data_bytes"], info["seconds"]),
                         (9644, 9600, 1))
        self.assertEqual(validate_wav(wave(320000))["bytes"], MAX_WAV_BYTES)
        self.assert_code("AUDIO_LIMIT", lambda: validate_wav(wave(320002)))
        self.assert_code("AUDIO_LIMIT", lambda: validate_wav(wave(9598)))
        self.assert_code("AUDIO_LIMIT", lambda: validate_wav(b""))
        self.assert_code("INVALID_AUDIO", lambda: validate_wav(wave(9601)))
        self.assert_code("INVALID_AUDIO", lambda: validate_wav("text"))
        for offset, value, width in ((24, 8000, "<I"), (22, 2, "<H"),
                                     (34, 8, "<H"), (16, 18, "<I")):
            raw = bytearray(wave())
            raw[offset:offset + struct.calcsize(width)] = struct.pack(width, value)
            self.assert_code("INVALID_AUDIO",
                             lambda raw=bytes(raw): validate_wav(raw))

    def test_request_shape(self):
        normalized = validate_request(self.body)
        self.assertEqual(normalized, self.body)
        for change in ({"schema": 2}, {"schema": True},
                       {"operation_id": "A" * 32},
                       {"format": "mp3"}, {"audio_sha256": "0" * 63},
                       {"audio_bytes": 9643}, {"source_lang": "en",
                                               "target_lang": "en"},
                       {"extra": 1}):
            body = dict(self.body)
            body.update(change)
            with self.assertRaises(TranslationError):
                validate_request(body)


class JobTests(Fixture):
    def test_admission_replay_and_payload_conflict(self):
        first = self.jobs.admit(self.body)
        self.assertEqual(first["state"], "ADMITTED")
        self.assertEqual(len(first["job_id"]), 32)
        self.assertEqual(first["generation"], 1)
        self.assertEqual(self.store.count(), 1)
        replay = self.jobs.admit(self.body)
        self.assertEqual(replay["job_id"], first["job_id"])
        self.assertEqual(replay["generation"], first["generation"])
        self.assertEqual(self.store.count(), 1)
        changed = dict(self.body, source_lang="zh-CN", target_lang="en")
        self.assert_code("OPERATION_CONFLICT", lambda: self.jobs.admit(changed))
        changed = dict(self.body, audio_sha256="0" * 64)
        self.assert_code("OPERATION_CONFLICT", lambda: self.jobs.admit(changed))
        self.assertEqual(self.store.count(), 1)

    def test_synthetic_job_and_repeated_upload(self):
        admitted = self.jobs.admit(self.body)
        view = self.jobs.upload(admitted["operation_id"], self.raw)
        self.assertEqual(view["state"], "DONE")
        self.assertEqual(view["source_text"], "Synthetic English source.")
        self.assertEqual(view["target_text"], "\u5408\u6210\u4e2d\u6587\u8bd1\u6587\u3002")
        self.assertNotEqual(view["source_text"], view["target_text"])
        self.assertEqual((self.provider.asr_calls, self.provider.mt_calls), (1, 1))
        again = self.jobs.upload(admitted["operation_id"], self.raw)
        self.assertEqual(again["state"], "DONE")
        self.assertEqual((self.provider.asr_calls, self.provider.mt_calls), (1, 1))
        status = self.jobs.status(admitted["job_id"])
        self.assertEqual(status["source_text"], view["source_text"])

        with sqlite3.connect(self.path) as db:
            columns = {row[1] for row in db.execute(
                "PRAGMA table_info(translation_jobs)")}
            tables = {row[0] for row in db.execute(
                "SELECT name FROM sqlite_master WHERE type='table'")}
        self.assertFalse(columns & {"source_text", "target_text", "audio"})
        self.assertTrue(all(name.startswith("translation_") for name in tables))
        self.assertFalse({"completions", "creations", "state", "originals"} &
                         tables)
        data = self.path.read_bytes()
        self.assertNotIn(b"Synthetic English source.", data)
        self.assertNotIn(b"RIFF", data)
        if os.name != "nt":
            self.assertEqual(self.path.stat().st_mode & 0o077, 0)

    def test_upload_identity_and_conflict(self):
        admitted = self.jobs.admit(self.body)
        operation = admitted["operation_id"]
        self.assert_code("AUDIO_CONFLICT",
                         lambda: self.jobs.upload(operation, wave(9602)))
        changed = bytearray(self.raw)
        changed[44] ^= 1
        self.assert_code("AUDIO_CONFLICT",
                         lambda: self.jobs.upload(operation, bytes(changed)))
        self.assert_code("NOT_FOUND",
                         lambda: self.jobs.upload("0" * 32, self.raw))
        self.assert_code("AUDIO_LIMIT",
                         lambda: self.jobs.upload(operation, b"bad"))
        self.assertEqual((self.provider.asr_calls, self.provider.mt_calls), (0, 0))
        self.assertEqual(self.jobs.status(admitted["job_id"])["state"], "ADMITTED")

    def test_cancel_late_callback_and_generation(self):
        admitted = self.jobs.admit(self.body)
        operation = admitted["operation_id"]
        cancelled = self.jobs.cancel(operation)
        self.assertEqual(cancelled["state"], "CANCELLED")
        self.assertEqual(self.jobs.upload(operation, self.raw)["state"],
                         "CANCELLED")
        self.assertEqual(self.jobs.cancel(operation)["state"], "CANCELLED")
        self.assertEqual((self.provider.asr_calls, self.provider.mt_calls), (0, 0))

        second = self.jobs.admit(request(2, self.raw))
        late = request(2, self.raw)
        self.provider.asr_hook = lambda: self.jobs.cancel(second["operation_id"])
        view = self.jobs.upload(second["operation_id"], self.raw)
        self.provider.asr_hook = None
        self.assertEqual(view["state"], "CANCELLED")
        self.assertEqual((self.provider.asr_calls, self.provider.mt_calls), (1, 0))
        self.assertNotIn("source_text", view)

        third = self.jobs.admit(request(3, self.raw))
        row = self.store.row(third["operation_id"])
        generation = row["generation"]
        self.store.claim(third["operation_id"], "ASR", generation)
        self.store.cancel(third["operation_id"], generation)
        source_hash = hashlib.sha256(b"source").hexdigest()
        self.assert_code("GENERATION_MISMATCH",
                         lambda: self.store.complete_asr(
                             third["operation_id"], generation, source_hash, 6))
        self.assertEqual(self.jobs.status(third["job_id"])["state"], "CANCELLED")
        self.assertEqual(late["operation_id"], second["operation_id"])

    def test_generation_stage_order_and_identity(self):
        admitted = self.jobs.admit(self.body)
        operation = admitted["operation_id"]
        generation = admitted["generation"]
        self.assert_code("STAGE_ORDER",
                         lambda: self.store.claim(operation, "MT", generation))
        self.assert_code("GENERATION_MISMATCH",
                         lambda: self.store.claim(operation, "ASR",
                                                  generation + 1))
        self.assertEqual(self.store.claim(operation, "ASR", generation)["stage"],
                         "ASR")
        self.assert_code("ALREADY_CLAIMED",
                         lambda: self.store.claim(operation, "ASR", generation))
        source_hash = hashlib.sha256(b"source").hexdigest()
        self.assert_code("GENERATION_MISMATCH",
                         lambda: self.store.complete_asr(operation,
                                                         generation + 1,
                                                         source_hash, 6))
        row = self.store.complete_asr(operation, generation, source_hash, 6)
        self.assertEqual(row["stage"], "ASR_DONE")
        self.assertEqual(self.store.claim(operation, "MT", generation)["stage"],
                         "MT")
        self.assert_code("ALREADY_CLAIMED",
                         lambda: self.store.claim(operation, "MT", generation))
        target_hash = hashlib.sha256(b"target").hexdigest()
        self.assertEqual(self.store.complete_mt(operation, generation,
                                                target_hash, 6)["state"],
                         "DONE")
        self.assert_code("STATE",
                         lambda: self.store.complete_mt(operation, generation,
                                                        target_hash, 6))
        self.assertEqual(self.jobs.status(admitted["job_id"])["state"],
                         "TEXT_LOST")
        self.assert_code("NOT_FOUND",
                         lambda: self.jobs.status("0" * 32))
        self.assert_code("NOT_FOUND",
                         lambda: self.jobs.cancel_job("0" * 32))
        self.assert_code("INVALID_JOB", lambda: validate_job_id("bad"))

    def test_restart_after_claim_is_unknown(self):
        admitted = self.jobs.admit(self.body)
        operation = admitted["operation_id"]
        self.store.claim(operation, "ASR", admitted["generation"])
        second_provider = SyntheticProvider()
        restarted_store = TranslationStore(self.path,
                                           clock=lambda: self.clock_value)
        restarted = TranslationJobs(restarted_store, second_provider,
                                    clock=lambda: self.clock_value)
        view = restarted.status(admitted["job_id"])
        self.assertEqual(view["state"], "UNKNOWN")
        self.assertEqual(restarted.upload(operation, self.raw)["state"], "UNKNOWN")
        self.assertEqual((second_provider.asr_calls, second_provider.mt_calls),
                         (0, 0))
        self.assertEqual(restarted_store.row(operation)["state"], "UNKNOWN")

    def test_restart_done_is_text_lost_and_expiry(self):
        admitted = self.jobs.admit(self.body)
        self.assertEqual(self.jobs.upload(admitted["operation_id"],
                                          self.raw)["state"], "DONE")
        second_provider = SyntheticProvider()
        restarted_store = TranslationStore(self.path,
                                           clock=lambda: self.clock_value)
        restarted = TranslationJobs(restarted_store, second_provider,
                                    clock=lambda: self.clock_value)
        view = restarted.status(admitted["job_id"])
        self.assertEqual(view["state"], "TEXT_LOST")
        self.assertTrue(view["text_lost"])
        self.assertNotIn("target_text", view)
        self.assertEqual(restarted.upload(admitted["operation_id"],
                                          self.raw)["state"], "TEXT_LOST")
        self.assertEqual((second_provider.asr_calls, second_provider.mt_calls),
                         (0, 0))

        third = self.jobs.admit(request(2, self.raw))
        self.assertEqual(self.jobs.upload(third["operation_id"], self.raw)["state"],
                         "DONE")
        self.clock_value += 120
        self.assertEqual(self.jobs.status(third["job_id"])["state"],
                         "TEXT_LOST")

    def test_cancel_between_asr_and_remember_is_bounded(self):
        path = Path(self.tmp.name) / "race.sqlite3"
        store = TranslationStore(path, clock=lambda: self.clock_value)
        provider = SyntheticProvider()
        reached = threading.Event()
        release = threading.Event()

        class PausedJobs(TranslationJobs):
            def _remember(self, row, source, target, generation):
                if target is None:
                    reached.set()
                    if not release.wait(5):
                        raise RuntimeError("review scheduling timeout")
                return super()._remember(row, source, target, generation)

        jobs = PausedJobs(store, provider, clock=lambda: self.clock_value)
        admitted = jobs.admit(self.body)
        result = {}

        def worker():
            try:
                result["state"] = jobs.upload(admitted["operation_id"],
                                              self.raw)["state"]
            except TranslationError as exc:
                result["error"] = exc.code

        thread = threading.Thread(target=worker)
        thread.start()
        self.assertTrue(reached.wait(5))
        self.assertEqual(jobs.cancel(admitted["operation_id"],
                                     admitted["generation"])["state"],
                         "CANCELLED")
        release.set()
        thread.join(5)
        self.assertFalse(thread.is_alive())
        self.assertEqual(result, {"state": "CANCELLED"})
        self.assertEqual(store.row(admitted["operation_id"])["state"],
                         "CANCELLED")
        self.assertEqual(len(jobs._texts), 0)
        self.assertEqual((provider.asr_calls, provider.mt_calls), (1, 0))

    def test_mt_late_callback_discards_both_texts(self):
        admitted = self.jobs.admit(self.body)
        self.provider.mt_hook = lambda: self.jobs.cancel(admitted["operation_id"])
        view = self.jobs.upload(admitted["operation_id"], self.raw)
        self.provider.mt_hook = None
        self.assertEqual(view["state"], "CANCELLED")
        self.assertNotIn("source_text", view)
        self.assertNotIn(admitted["job_id"], self.jobs._texts)
        self.assertEqual((self.provider.asr_calls, self.provider.mt_calls), (1, 1))

    def test_cross_job_cleanup_sweeps_expired_text(self):
        first = self.jobs.admit(self.body)
        self.assertEqual(self.jobs.upload(first["operation_id"],
                                          self.raw)["state"], "DONE")
        self.assertIn(first["job_id"], self.jobs._texts)
        self.clock_value += 900
        second = self.jobs.admit(request(2, self.raw))
        self.assertNotIn(first["job_id"], self.jobs._texts)
        self.assertEqual(self.jobs.status(first["job_id"])["state"], "TEXT_LOST")
        self.assertEqual(self.store.row(first["operation_id"])["state"], "DONE")
        self.assertEqual(self.jobs.cleanup(), 0)
        self.assertEqual(self.jobs.status(second["job_id"])["state"], "ADMITTED")

        third = self.jobs.admit(request(3, self.raw))
        self.assertEqual(self.jobs.upload(third["operation_id"],
                                          self.raw)["state"], "DONE")
        self.assertIn(third["job_id"], self.jobs._texts)
        self.clock_value += 121
        self.assertEqual(self.jobs.cleanup(), 1)
        self.assertNotIn(third["job_id"], self.jobs._texts)

    def test_provider_failure_preserves_source(self):
        admitted = self.jobs.admit(self.body)

        def fail():
            raise TranslationError("SYNTH_UNAVAILABLE")

        self.provider.mt_hook = fail
        view = self.jobs.upload(admitted["operation_id"], self.raw)
        self.assertEqual(view["state"], "FAILED")
        self.assertEqual(view["error"], "SYNTH_UNAVAILABLE")
        self.assertEqual(view["source_text"], "Synthetic English source.")
        self.assertNotIn("target_text", view)
        again = self.jobs.upload(admitted["operation_id"], self.raw)
        self.assertEqual(again["state"], "FAILED")
        self.assertEqual((self.provider.asr_calls, self.provider.mt_calls), (1, 1))


class CapacityTests(Fixture):
    def test_capacity_rejects_without_eviction(self):
        path = Path(self.tmp.name) / "small.sqlite3"
        store = TranslationStore(path, clock=lambda: self.clock_value,
                                 capacity=2)
        jobs = TranslationJobs(store, self.provider, clock=lambda: self.clock_value)
        first = jobs.admit(request(1, self.raw))
        second = jobs.admit(request(2, self.raw))
        self.assert_code("BUSY", lambda: jobs.admit(request(3, self.raw)))
        self.assertEqual(store.count(), 2)
        replay = jobs.admit(request(1, self.raw))
        self.assertEqual(replay["job_id"], first["job_id"])
        self.assertEqual(store.row(second["operation_id"])["job_id"],
                         second["job_id"])
        self.assertIsNone(store.row(request(3, self.raw)["operation_id"]))


MISSING = object()


class ApiTests(Fixture):
    def setUp(self):
        super().setUp()
        self.token = "t" * 48
        self.tasks = ("r" * 32, "w" * 32, "a" * 32)
        self.api = TranslationAPI(self.jobs, self.token, self.tasks)

    def call(self, method, path, body=None, token=MISSING, content_type=None):
        raw = b"" if body is None else json.dumps(body).encode("utf-8")
        if content_type is None:
            content_type = "application/json"
        if token is MISSING:
            token = self.token
        environ = {
            "REQUEST_METHOD": method,
            "PATH_INFO": path,
            "HTTP_AUTHORIZATION": "" if token is None else "Bearer " + token,
            "CONTENT_LENGTH": str(len(raw)),
            "CONTENT_TYPE": content_type,
            "wsgi.input": io.BytesIO(raw),
        }
        status = []
        data = b"".join(self.api(environ, lambda code, headers: status.append(code)))
        return status[0], json.loads(data), data

    def test_task_tokens_are_rejected_and_translation_token_works(self):
        for token in (None, "", *self.tasks):
            status, value, _ = self.call("POST", "/suite/v1/translations",
                                         self.body, token=token)
            self.assertEqual(status, "401 Unauthorized")
            self.assertEqual(value["error"], "UNAUTHORIZED")
        self.assert_code("INVALID_CONFIG",
                         lambda: TranslationAPI(self.jobs, self.tasks[0],
                                                self.tasks))
        status, value, _ = self.call("POST", "/suite/v1/translations", self.body)
        self.assertEqual(status, "202 Accepted")
        job_id = value["job_id"]
        status, value, raw = self.call("GET",
                                       "/suite/v1/translations/" + job_id)
        self.assertEqual((status, value["state"]), ("200 OK", "ADMITTED"))
        self.assertNotIn(self.token.encode(), raw)
        status, value, _ = self.call("DELETE",
                                     "/suite/v1/translations/" + job_id)
        self.assertEqual((status, value["state"]), ("200 OK", "CANCELLED"))
        status, value, _ = self.call("DELETE",
                                     "/suite/v1/translations/" + job_id)
        self.assertEqual((status, value["state"]), ("200 OK", "CANCELLED"))
        for token in self.tasks:
            status, _, _ = self.call("GET", "/suite/v1/translations/" + job_id,
                                     token=token)
            self.assertEqual(status, "401 Unauthorized")

    def test_replay_conflict_and_request_errors(self):
        status, first, _ = self.call("POST", "/suite/v1/translations", self.body)
        self.assertEqual(status, "202 Accepted")
        status, replay, _ = self.call("POST", "/suite/v1/translations", self.body)
        self.assertEqual((status, replay["job_id"]), ("202 Accepted", first["job_id"]))
        changed = dict(self.body, source_lang="zh-CN", target_lang="en")
        status, value, _ = self.call("POST", "/suite/v1/translations", changed)
        self.assertEqual((status, value["error"]), ("409 Conflict",
                                                    "OPERATION_CONFLICT"))
        status, value, _ = self.call("POST", "/suite/v1/translations",
                                     self.body, content_type="text/plain")
        self.assertEqual((status, value["error"]), ("400 Bad Request",
                                                    "INVALID_REQUEST"))
        environ = {
            "REQUEST_METHOD": "POST",
            "PATH_INFO": "/suite/v1/translations",
            "HTTP_AUTHORIZATION": "Bearer " + self.token,
            "CONTENT_LENGTH": "5000",
            "CONTENT_TYPE": "application/json",
            "wsgi.input": io.BytesIO(b"{}"),
        }
        status = []
        raw = b"".join(self.api(environ, lambda code, headers: status.append(code)))
        self.assertEqual((status[0], json.loads(raw)["error"]),
                         ("400 Bad Request", "LIMIT_EXCEEDED"))
        status, value, _ = self.call("GET", "/suite/v1/translations/" + "0" * 32)
        self.assertEqual((status, value["error"]), ("404 Not Found", "NOT_FOUND"))
        status, value, _ = self.call("GET", "/suite/v1/translations/not-a-job")
        self.assertEqual((status, value["error"]), ("404 Not Found", "NOT_FOUND"))


if __name__ == "__main__":
    unittest.main()
