"""T1 translation job orchestration with a separate capability and ledger."""

import hmac
import queue as queue_module
import re
import threading
import time

from .common import (MAX_PAYLOAD_BYTES, MAX_WAV_BYTES, TEXT_TTL_SECONDS,
                     TranslationError, canonical, fingerprint, new_job_id,
                     sha256_hex, strict_json_loads, token_valid, validate_job_id,
                     validate_request, validate_text, validate_wav)
from .store import TranslationStore

JOB_PATH = re.compile(r"/suite/v1/translations/([0-9a-f]{32})\Z")
AUDIO_PATH = re.compile(r"/suite/v1/translations/([0-9a-f]{32})/audio\Z")
DEFAULT_DAILY_ASR_SECONDS = 60
DEFAULT_DAILY_MT_JOBS = 6
DEFAULT_QUEUE_SIZE = 8
DEFAULT_AUDIO_TTL_SECONDS = 300


class _AudioRef:
    """A one-slot mutable reference so ASR can release the WAV immediately."""

    __slots__ = ("data",)

    def __init__(self, data):
        self.data = data


class TranslationJobs:
    """Run the local admission/ASR/MT job path over an isolated metadata store."""

    def __init__(self, store, provider, clock=time.time,
                 text_ttl=TEXT_TTL_SECONDS,
                 daily_asr_seconds=DEFAULT_DAILY_ASR_SECONDS,
                 daily_mt_jobs=DEFAULT_DAILY_MT_JOBS,
                 audio_ttl=DEFAULT_AUDIO_TTL_SECONDS):
        if not isinstance(store, TranslationStore):
            raise TranslationError("INVALID_CONFIG")
        if not hasattr(provider, "transcribe") or not hasattr(provider, "translate"):
            raise TranslationError("INVALID_CONFIG")
        if type(text_ttl) is not int or not 1 <= text_ttl <= 3600:
            raise TranslationError("INVALID_CONFIG")
        if (type(daily_asr_seconds) is not int
                or not 1 <= daily_asr_seconds <= 86400):
            raise TranslationError("INVALID_CONFIG")
        if type(daily_mt_jobs) is not int or not 1 <= daily_mt_jobs <= 100000:
            raise TranslationError("INVALID_CONFIG")
        if type(audio_ttl) is not int or not 1 <= audio_ttl <= 600:
            raise TranslationError("INVALID_CONFIG")
        self.store = store
        self.provider = provider
        self.clock = clock
        self.text_ttl = text_ttl
        self.daily_asr_seconds = daily_asr_seconds
        self.daily_mt_jobs = daily_mt_jobs
        self.audio_ttl = audio_ttl
        self._texts = {}
        # Serializes RAM cache updates with short store reads/cancellation.
        # Provider calls always run outside this lock.
        self._text_lock = threading.Lock()
        self._audio = {}
        self._pending = set()
        self._audio_lock = threading.Lock()
        self._stage_lock = threading.Lock()
        self._shutdown = threading.Event()
        self._in_flight = 0
        self._queue = None
        self._worker = None
        self._worker_stop = threading.Event()
        self._scheduler = None
        self._scheduler_stop = threading.Event()
        # A restart may follow a durable provider claim. Never replay it.
        self.store.recover()
        # A lost process also loses queued WAV bytes; expire old unclaimed
        # admissions instead of leaving them ADMITTED forever.
        now = self._now()
        self.store.expire_stale_admissions(now - self.audio_ttl, now)

    def _now(self):
        return int(self.clock())

    def _day(self):
        return self._now() // 86400

    def _remember(self, row, source, target, generation):
        """Refuse to refill a cancelled or generation-invalid text slot."""
        with self._text_lock:
            current = self.store.row(row["operation_id"])
            if (current is None or current["generation"] != generation or
                    current["state"] == "CANCELLED"):
                return False
            if target is None:
                if current["state"] != "RUNNING" or current["stage"] != "ASR_DONE":
                    return False
            elif current["state"] != "DONE":
                return False
            self._texts[row["job_id"]] = (self._now() + self.text_ttl,
                                          source, target)
            return True

    def _text(self, row):
        with self._text_lock:
            entry = self._texts.get(row["job_id"])
            if entry is None:
                return None
            if self._now() >= entry[0]:
                self._texts.pop(row["job_id"], None)
                return None
            return entry[1], entry[2]

    def cleanup(self, now=None):
        """Drop expired RAM text/audio and terminalize unclaimed admissions."""
        current = self._now() if now is None else int(now)
        with self._text_lock:
            expired = [job_id for job_id, entry in self._texts.items()
                       if current >= entry[0]]
            for job_id in expired:
                self._texts.pop(job_id, None)
        stale_jobs = []
        with self._audio_lock:
            entry = None
            for job_id in list(self._audio):
                entry = self._audio.get(job_id)
                if entry is not None and current >= entry[0]:
                    self._audio.pop(job_id, None)
                    self._pending.discard(job_id)
                    stale_jobs.append(job_id)
                entry = None
        for job_id in stale_jobs:
            row = self.store.row_by_job(job_id)
            if row is not None and row["state"] == "ADMITTED":
                self.store.expire_audio(row["operation_id"],
                                        row["generation"], current)
        # Also expire admissions whose RAM audio was lost in an earlier
        # process. Deduplication evidence remains as an EXPIRED row.
        self.store.expire_stale_admissions(current - self.audio_ttl, current)
        return len(expired)

    def _view(self, row):
        if row is None:
            raise TranslationError("NOT_FOUND")
        view = {
            "schema": 1,
            "operation_id": row["operation_id"],
            "job_id": row["job_id"],
            "state": row["state"],
            "source_lang": row["source_lang"],
            "target_lang": row["target_lang"],
            "generation": row["generation"],
            "audio_bytes": row["audio_bytes"],
            "source_bytes": row["source_bytes"],
            "target_bytes": row["target_bytes"],
            "updated_at": row["updated"],
            "error": row["error"],
        }
        if row["state"] == "DONE":
            pair = self._text(row)
            if pair is None or pair[1] is None:
                view["state"] = "TEXT_LOST"
                view["text_lost"] = True
                if pair is not None:
                    view["source_text"] = pair[0]
            else:
                view["source_text"], view["target_text"] = pair
                view["text_lost"] = False
        elif row["state"] == "FAILED":
            pair = self._text(row)
            if pair is not None:
                view["source_text"] = pair[0]
        return view

    def admit(self, body):
        self.cleanup()
        if self._shutdown.is_set():
            raise TranslationError("UNAVAILABLE")
        request = validate_request(body)
        now = self._now()
        row = self.store.admit(request, fingerprint(request), new_job_id(), now)
        return self._view(row)

    def _current(self, operation_id):
        row = self.store.row(operation_id)
        if row is None:
            raise TranslationError("NOT_FOUND")
        return row

    def _fail(self, operation_id, code, generation):
        try:
            return self.store.fail(operation_id, code, generation)
        except TranslationError as exc:
            if exc.code in {"GENERATION_MISMATCH", "STATE"}:
                return self._current(operation_id)
            raise

    def _claim(self, operation_id, stage, generation):
        try:
            return self.store.claim(operation_id, stage, generation), True
        except TranslationError as exc:
            if exc.code in {"ALREADY_CLAIMED", "STATE", "STAGE_ORDER",
                            "GENERATION_MISMATCH"}:
                return self._current(operation_id), False
            raise

    def _complete_asr(self, operation_id, generation, source):
        raw = source.encode("utf-8")
        try:
            self.store.complete_asr(operation_id, generation,
                                    sha256_hex(raw), len(raw))
        except TranslationError as exc:
            if exc.code in {"GENERATION_MISMATCH", "STATE"}:
                return self._current(operation_id)
            raise
        return self._current(operation_id)

    def _complete_mt(self, operation_id, generation, target):
        raw = target.encode("utf-8")
        try:
            self.store.complete_mt(operation_id, generation,
                                   sha256_hex(raw), len(raw))
        except TranslationError as exc:
            if exc.code in {"GENERATION_MISMATCH", "STATE"}:
                return self._current(operation_id)
            raise
        return self._current(operation_id)

    def _reserve_asr(self, seconds):
        self.store.reserve_asr(self._day(), seconds, self.daily_asr_seconds)

    def _reserve_mt(self):
        self.store.reserve_mt(self._day(), self.daily_mt_jobs)

    def _stage_allowed(self):
        with self._stage_lock:
            return not self._shutdown.is_set()

    def _begin_stage(self):
        """Atomically permit a paid call unless shutdown already began."""
        with self._stage_lock:
            if self._shutdown.is_set():
                return False
            self._in_flight += 1
            return True

    def _end_stage(self):
        with self._stage_lock:
            self._in_flight -= 1

    def _execute(self, row, audio, info):
        """Run one paid pipeline with cancellation checks between stages."""
        operation_id = row["operation_id"]
        generation = row["generation"]
        if not self._stage_allowed():
            return self._view(self._current(operation_id))
        row, claimed = self._claim(operation_id, "ASR", generation)
        if not claimed or row["state"] == "CANCELLED":
            return self._view(row)

        try:
            self._reserve_asr(info["seconds"])
        except TranslationError as exc:
            current = self._current(operation_id)
            if current["state"] == "CANCELLED":
                return self._view(current)
            return self._view(self._fail(operation_id, exc.code, generation))
        current = self._current(operation_id)
        if current["state"] == "CANCELLED":
            return self._view(current)
        if not self._begin_stage():
            return self._view(self._current(operation_id))

        reference = None
        raw = None
        try:
            reference = audio if isinstance(audio, _AudioRef) else None
            raw = reference.data if reference is not None else audio
            audio = None
            try:
                source = self.provider.transcribe(raw, row["source_lang"])
            except TranslationError as exc:
                current = self._current(operation_id)
                if current["state"] == "CANCELLED":
                    return self._view(current)
                return self._view(self._fail(operation_id, exc.code,
                                             generation))
            except Exception:
                current = self._current(operation_id)
                if current["state"] == "CANCELLED":
                    return self._view(current)
                return self._view(self._fail(operation_id, "UNAVAILABLE",
                                             generation))
        finally:
            # Release the WAV as soon as ASR returns; MT needs only the text.
            raw = None
            if reference is not None:
                reference.data = None
            reference = None
            audio = None
            self._end_stage()

        current = self._current(operation_id)
        if current["state"] == "CANCELLED":
            return self._view(current)
        try:
            source = validate_text(source)
        except TranslationError as exc:
            return self._view(self._fail(operation_id, exc.code, generation))
        current = self._complete_asr(operation_id, generation, source)
        if current["state"] == "CANCELLED":
            return self._view(current)
        if current["stage"] != "ASR_DONE":
            return self._view(current)
        # Source text may be shown even when MT later fails; it stays in RAM.
        if not self._remember(current, source, None, generation):
            return self._view(self._current(operation_id))

        if not self._stage_allowed():
            return self._view(self._current(operation_id))
        row, claimed = self._claim(operation_id, "MT", generation)
        if not claimed or row["state"] == "CANCELLED":
            return self._view(row)
        if row["stage"] != "MT":
            return self._view(row)

        try:
            self._reserve_mt()
        except TranslationError as exc:
            current = self._current(operation_id)
            if current["state"] == "CANCELLED":
                return self._view(current)
            return self._view(self._fail(operation_id, exc.code, generation))
        current = self._current(operation_id)
        if current["state"] == "CANCELLED":
            return self._view(current)
        if not self._begin_stage():
            return self._view(self._current(operation_id))

        try:
            target = self.provider.translate(source, row["source_lang"],
                                             row["target_lang"])
        except TranslationError as exc:
            current = self._current(operation_id)
            if current["state"] == "CANCELLED":
                return self._view(current)
            return self._view(self._fail(operation_id, exc.code, generation))
        except Exception:
            current = self._current(operation_id)
            if current["state"] == "CANCELLED":
                return self._view(current)
            return self._view(self._fail(operation_id, "UNAVAILABLE", generation))
        finally:
            self._end_stage()

        current = self._current(operation_id)
        if current["state"] == "CANCELLED":
            return self._view(current)
        try:
            target = validate_text(target)
        except TranslationError as exc:
            return self._view(self._fail(operation_id, exc.code, generation))
        current = self._complete_mt(operation_id, generation, target)
        if current["state"] == "CANCELLED":
            return self._view(current)
        if current["state"] == "DONE":
            self._remember(current, source, target, generation)
        return self._view(current)

    def upload(self, operation_id, raw):
        """Synchronous local path used by direct callers and host tests."""
        self.cleanup()
        if self._shutdown.is_set():
            raise TranslationError("UNAVAILABLE")
        row = self._current(operation_id)
        if row["state"] == "CANCELLED":
            return self._view(row)
        info = validate_wav(raw)
        if (len(raw) != row["audio_bytes"] or
                sha256_hex(raw) != row["audio_sha256"]):
            raise TranslationError("AUDIO_CONFLICT")
        # A repeated matching upload, or a terminal row, never repeats ASR.
        if row["state"] != "ADMITTED":
            return self._view(row)
        reference = _AudioRef(raw)
        raw = None
        return self._execute(row, reference, info)

    def submit_audio(self, job_id, raw):
        """Validate once, enqueue to the single worker, acknowledge promptly."""
        self.cleanup()
        if self._shutdown.is_set():
            raise TranslationError("UNAVAILABLE")
        validate_job_id(job_id)
        row = self.store.row_by_job(job_id)
        if row is None:
            raise TranslationError("NOT_FOUND")
        if row["state"] == "CANCELLED":
            raise TranslationError("CANCELLED")
        info = validate_wav(raw)
        if (len(raw) != row["audio_bytes"] or
                sha256_hex(raw) != row["audio_sha256"]):
            raise TranslationError("AUDIO_CONFLICT")
        if row["state"] != "ADMITTED":
            return self._view(row), False
        if self._queue is None:
            return self.upload(row["operation_id"], raw), True
        with self._audio_lock:
            if job_id not in self._pending:
                try:
                    self._queue.put_nowait(
                        (row["operation_id"], job_id, row["generation"]))
                except queue_module.Full:
                    raise TranslationError("BUSY") from None
                self._pending.add(job_id)
                self._audio[job_id] = (self._now() + self.audio_ttl, raw)
        # Record the enqueue time so the admission sweep measures the same
        # audio lifetime as the RAM entry.
        self.store.touch_admitted(row["operation_id"], self._now())
        # ``info`` was validated above; it is intentionally not retained.
        del info
        return self._view(row), True

    def start_worker(self, queue_size=DEFAULT_QUEUE_SIZE):
        if type(queue_size) is not int or not 1 <= queue_size <= 32:
            raise TranslationError("INVALID_CONFIG")
        if self._worker is not None:
            return
        self._shutdown.clear()
        self._queue = queue_module.Queue(maxsize=queue_size)
        self._worker_stop.clear()
        self._worker = threading.Thread(
            target=self._worker_loop, name="translation-worker", daemon=True)
        self._worker.start()

    def _worker_loop(self):
        while not self._worker_stop.is_set():
            try:
                item = self._queue.get(timeout=0.2)
            except queue_module.Empty:
                continue
            if item is None:
                self._queue.task_done()
                break
            try:
                self._process_item(item)
            finally:
                item = None
                self._queue.task_done()

    def _process_item(self, item):
        """Own one job in a short-lived frame so no WAV outlives the job."""
        operation_id, job_id, generation = item
        raw = None
        entry = None
        reference = None
        try:
            with self._audio_lock:
                entry = self._audio.pop(job_id, None)
                if entry is not None:
                    raw = entry[1]
                entry = None
            if raw is None:
                return
            row = self.store.row(operation_id)
            if (row is None or row["generation"] != generation
                    or row["state"] != "ADMITTED"):
                return
            try:
                info = validate_wav(raw)
            except TranslationError:
                return
            reference = _AudioRef(raw)
            raw = None
            self._execute(row, reference, info)
        except Exception:
            try:
                self._fail(operation_id, "UNAVAILABLE", generation)
            except Exception:
                pass
        finally:
            raw = None
            entry = None
            if reference is not None:
                reference.data = None
            reference = None
            with self._audio_lock:
                self._pending.discard(job_id)
                self._audio.pop(job_id, None)

    def stop_worker(self, join_timeout=5.0):
        if self._worker is None:
            return
        self._worker_stop.set()
        try:
            self._queue.put_nowait(None)
        except queue_module.Full:
            pass
        self._worker.join(max(0.0, float(join_timeout)))
        if not self._worker.is_alive():
            self._worker = None
            self._queue = None
            with self._audio_lock:
                self._audio.clear()
                self._pending.clear()

    def start_expiry(self, interval=1.0):
        if (type(interval) not in (int, float)
                or not 0.01 <= interval <= 60):
            raise TranslationError("INVALID_CONFIG")
        if self._scheduler is not None:
            return
        self._scheduler_stop.clear()
        self._scheduler = threading.Thread(
            target=self._expiry_loop, args=(float(interval),),
            name="translation-expiry", daemon=True)
        self._scheduler.start()

    def _expiry_loop(self, interval):
        while not self._scheduler_stop.wait(interval):
            try:
                self.cleanup()
            except Exception:
                pass

    def stop_expiry(self):
        if self._scheduler is None:
            return
        self._scheduler_stop.set()
        self._scheduler.join(5)
        self._scheduler = None

    def stop(self, join_timeout=5.0):
        """Mark pending/active jobs terminal, then bound the worker drain."""
        with self._stage_lock:
            self._shutdown.set()
        self.stop_expiry()
        try:
            self.store.shutdown_cancel(self._now())
        except Exception:
            pass
        with self._text_lock:
            self._texts.clear()
        with self._audio_lock:
            self._audio.clear()
            self._pending.clear()
        self.stop_worker(join_timeout)

    def ready(self):
        """Local readiness only; this performs no provider or quota call."""
        with self._stage_lock:
            if self._shutdown.is_set():
                return False
            return (self._worker is not None and self._worker.is_alive()
                    and self._queue is not None)

    def cancel(self, operation_id, generation=None):
        self.cleanup()
        with self._text_lock:
            row = self.store.cancel(operation_id, generation)
            self._texts.pop(row["job_id"], None)
        with self._audio_lock:
            self._audio.pop(row["job_id"], None)
            self._pending.discard(row["job_id"])
        return self._view(row)

    def cancel_job(self, job_id, generation=None):
        self.cleanup()
        validate_job_id(job_id)
        row = self.store.row_by_job(job_id)
        if row is None:
            raise TranslationError("NOT_FOUND")
        return self.cancel(row["operation_id"], generation)

    def status(self, job_id):
        self.cleanup()
        validate_job_id(job_id)
        row = self.store.row_by_job(job_id)
        if row is None:
            raise TranslationError("NOT_FOUND")
        return self._view(row)

    def status_operation(self, operation_id):
        self.cleanup()
        return self._view(self._current(operation_id))


class TranslationAPI:
    """WSGI surface for admission, one bounded WAV PUT, status and cancel.

    Audio upload returns as soon as the single bounded worker accepts the job.
    Status and cancellation remain serviceable while ASR or MT runs because no
    request-wide lock is held across a provider call. Only the independent
    translation bearer is accepted; task tokens are rejected.
    """

    def __init__(self, jobs, token, forbidden_tokens=()):
        if not isinstance(jobs, TranslationJobs) or not token_valid(token):
            raise TranslationError("INVALID_CONFIG")
        for forbidden in forbidden_tokens:
            if type(forbidden) is not str or token == forbidden:
                raise TranslationError("INVALID_CONFIG")
        self.jobs = jobs
        self._token = token.encode("ascii")

    def __call__(self, environ, start_response):
        try:
            authorization = environ.get("HTTP_AUTHORIZATION", "").encode("utf-8")
            if not hmac.compare_digest(authorization, b"Bearer " + self._token):
                return self._respond(start_response, "401 Unauthorized",
                                     {"error": "UNAUTHORIZED"})
            method = environ.get("REQUEST_METHOD")
            path = environ.get("PATH_INFO")
            if method == "GET" and path == "/suite/v1/translations/ready":
                if not self.jobs.ready():
                    return self._respond(start_response,
                                         "503 Service Unavailable",
                                         {"error": "NOT_READY"})
                return self._respond(start_response, "200 OK",
                                     {"schema": 1, "ready": True,
                                      "provider": "mimo"})
            if method == "POST" and path == "/suite/v1/translations":
                if environ.get("CONTENT_TYPE") != "application/json":
                    raise TranslationError("INVALID_REQUEST")
                raw = self._body(environ)
                try:
                    body = strict_json_loads(raw)
                except TranslationError:
                    raise
                return self._respond(start_response, "202 Accepted",
                                     self.jobs.admit(body))
            match = AUDIO_PATH.fullmatch(path or "")
            if match and method == "PUT":
                raw = self._audio_body(environ)
                view, accepted = self.jobs.submit_audio(match.group(1), raw)
                status = "202 Accepted" if accepted else "200 OK"
                return self._respond(start_response, status, view)
            match = JOB_PATH.fullmatch(path or "")
            if match and method == "GET":
                return self._respond(start_response, "200 OK",
                                     self.jobs.status(match.group(1)))
            if match and method == "DELETE":
                return self._respond(start_response, "200 OK",
                                     self.jobs.cancel_job(match.group(1)))
            return self._respond(start_response, "404 Not Found",
                                 {"error": "NOT_FOUND"})
        except TranslationError as exc:
            status = {
                "NOT_FOUND": "404 Not Found",
                "OPERATION_CONFLICT": "409 Conflict",
                "CONFLICT": "409 Conflict",
                "AUDIO_CONFLICT": "409 Conflict",
                "GENERATION_MISMATCH": "409 Conflict",
                "CANCELLED": "409 Conflict",
                "STATE": "409 Conflict",
                "BUSY": "429 Too Many Requests",
                "QUOTA": "429 Too Many Requests",
                "AUDIO_LIMIT": "413 Payload Too Large",
                "UNAVAILABLE": "503 Service Unavailable",
            }.get(exc.code, "400 Bad Request")
            return self._respond(start_response, status, {"error": exc.code})
        except (ValueError, UnicodeError, KeyError, TypeError):
            return self._respond(start_response, "400 Bad Request",
                                 {"error": "INVALID_REQUEST"})
        except Exception:
            return self._respond(start_response, "503 Service Unavailable",
                                 {"error": "UNAVAILABLE"})

    @staticmethod
    def _body(environ):
        try:
            size = int(environ.get("CONTENT_LENGTH") or "0")
        except (TypeError, ValueError):
            raise TranslationError("INVALID_REQUEST") from None
        if size <= 0 or size > MAX_PAYLOAD_BYTES:
            raise TranslationError("LIMIT_EXCEEDED")
        raw = environ["wsgi.input"].read(size)
        if len(raw) != size:
            raise TranslationError("INVALID_REQUEST")
        return raw

    @staticmethod
    def _audio_body(environ):
        """Stream one canonical WAV body without chunked/compressed ambiguity."""
        if environ.get("HTTP_TRANSFER_ENCODING"):
            raise TranslationError("INVALID_REQUEST")
        encoding = (environ.get("HTTP_CONTENT_ENCODING") or "identity")
        if encoding.strip().lower() != "identity":
            raise TranslationError("INVALID_REQUEST")
        content_type = environ.get("CONTENT_TYPE")
        if (type(content_type) is not str
                or content_type.strip().lower() != "audio/wav"):
            raise TranslationError("INVALID_REQUEST")
        length = environ.get("CONTENT_LENGTH")
        if type(length) is not str:
            raise TranslationError("INVALID_REQUEST")
        if not length.isascii() or not length.isdigit():
            raise TranslationError("INVALID_REQUEST")
        size = int(length)
        if size <= 0:
            raise TranslationError("INVALID_REQUEST")
        if size > MAX_WAV_BYTES:
            raise TranslationError("AUDIO_LIMIT")
        stream = environ.get("wsgi.input")
        if stream is None:
            raise TranslationError("INVALID_REQUEST")
        raw = bytearray()
        while len(raw) < size:
            block = stream.read(min(8192, size - len(raw)))
            if not block:
                raise TranslationError("INVALID_REQUEST")
            raw.extend(block)
        return bytes(raw)

    @staticmethod
    def _respond(start_response, status, body):
        data = canonical(body)
        start_response(status, [("Content-Type", "application/json"),
                                ("Content-Length", str(len(data))),
                                ("Cache-Control", "no-store")])
        return [data]
