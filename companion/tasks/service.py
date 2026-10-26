"""Refresh orchestration plus the cached device-facing WSGI API."""

import hmac
import json
import re
import threading
from urllib.parse import parse_qs

from .common import TaskError, packed


def discover(client, store):
    if store.state()["auth_halted"]:
        raise TaskError("AUTH_HALTED")
    try:
        collections = client.discover()
        store.set_collections(collections)
        return collections
    except TaskError as exc:
        store.failure(exc.code)
        raise


def refresh(client, store):
    state = store.state()
    if state["selection_revision"] == 0:
        raise TaskError("SELECTION_REQUIRED")
    try:
        collections = discover(client, store)
        selected = set(json.loads(state["selection"]))
        if not selected <= {c["id"] for c in collections}:
            raise TaskError("COLLECTION_MISSING")
        records, originals = client.fetch([c for c in collections if c["id"] in selected])
        return store.publish(records, originals, state["selection_revision"])
    except TaskError as exc:
        store.failure(exc.code)
        raise


class API:
    """Cache reads only for device tokens; selection requires a separate admin token.

    Network refresh is an explicit CLI job, outside HTTP/LVGL threads. This WSGI
    callable requires a production TLS reverse proxy before external deployment.
    """

    def __init__(self, store, device_token, admin_token, completion_token=None, speech=None):
        if (len(device_token) < 32 or len(admin_token) < 32 or device_token == admin_token
                or not device_token.isascii() or not admin_token.isascii()):
            raise TaskError("INVALID_CONFIG")
        self.store = store
        self.device_token = device_token.encode()
        self.admin_token = admin_token.encode()
        if completion_token is not None and (not isinstance(completion_token,str) or
                not re.fullmatch(r"[A-Za-z0-9_-]{32,128}",completion_token) or completion_token in {device_token,admin_token}):
            raise TaskError("INVALID_CONFIG")
        self.completion_token=completion_token.encode() if completion_token else None
        from .creation import CreationStore
        self.creations=CreationStore(store)
        self.speech=speech
        self.speech_lock=threading.Lock()

    def __call__(self, environ, start_response):
        try:
            authorization = environ.get("HTTP_AUTHORIZATION", "").encode("utf-8")
            is_admin = hmac.compare_digest(authorization, b"Bearer " + self.admin_token)
            is_device = hmac.compare_digest(authorization, b"Bearer " + self.device_token)
            is_writer=bool(self.completion_token and hmac.compare_digest(authorization,b"Bearer "+self.completion_token))
            if not (is_admin or is_device or is_writer):
                return self._respond(start_response, "401 Unauthorized", {"error": "UNAUTHORIZED"})
            method, path = environ.get("REQUEST_METHOD"), environ.get("PATH_INFO")
            if path=="/suite/v1/task-voice" and method=="GET":
                if not is_writer: return self._respond(start_response,"403 Forbidden",{"error":"READ_ONLY"})
                target=self.creations.target()
                return self._respond(start_response,"200 OK",{"schema":1,"ready":self.speech is not None,
                    "collection_id":target["id"],"collection_name":target["name"],"max_seconds":12})
            elif path=="/suite/v1/task-transcriptions" and method=="POST":
                if not is_writer: return self._respond(start_response,"403 Forbidden",{"error":"READ_ONLY"})
                from .speech import MAX_WAV,wav_seconds
                if self.speech is None: raise TaskError("SPEECH_NOT_CONFIGURED")
                target=self.creations.target()
                size=int(environ.get("CONTENT_LENGTH") or "0")
                if environ.get("CONTENT_TYPE")!="audio/wav" or not 44<size<=MAX_WAV:
                    raise TaskError("INVALID_AUDIO")
                if not self.speech_lock.acquire(blocking=False): raise TaskError("SPEECH_BUSY")
                try:
                    raw=environ["wsgi.input"].read(size)
                    if len(raw)!=size: raise TaskError("INVALID_AUDIO")
                    seconds=wav_seconds(raw)
                    self.creations.charge(seconds,self.speech.daily_seconds)
                    transcript=self.speech.transcribe(raw)
                    # A selection change during ASR never silently retargets creation.
                    if self.creations.target()["id"]!=target["id"]: raise TaskError("CREATION_TARGET_REQUIRED")
                    return self._respond(start_response,"200 OK",{"schema":1,"summary":transcript,
                        "collection_id":target["id"],"collection_name":target["name"]})
                finally: self.speech_lock.release()
            elif path=="/suite/v1/task-creations" and method=="POST":
                if not is_writer: return self._respond(start_response,"403 Forbidden",{"error":"READ_ONLY"})
                size=int(environ.get("CONTENT_LENGTH") or "0")
                if not 0<size<=4096: raise TaskError("LIMIT_EXCEEDED")
                raw=environ["wsgi.input"].read(size)
                if len(raw)!=size: raise TaskError("INVALID_REQUEST")
                body=json.loads(raw)
                if not isinstance(body,dict): raise TaskError("INVALID_OPERATION")
                retry=body.pop("retry",False)
                if type(retry) is not bool: raise TaskError("INVALID_OPERATION")
                return self._respond(start_response,"202 Accepted",self.creations.accept(body,retry))
            elif path=="/suite/v1/task-completions" and method=="POST":
                if not is_writer: return self._respond(start_response,"403 Forbidden",{"error":"READ_ONLY"})
                size=int(environ.get("CONTENT_LENGTH") or "0")
                if not 0<size<=2048: raise TaskError("LIMIT_EXCEEDED")
                raw=environ["wsgi.input"].read(size)
                if len(raw)!=size: raise TaskError("INVALID_REQUEST")
                body=json.loads(raw)
                if not isinstance(body,dict): raise TaskError("INVALID_OPERATION")
                retry=body.pop("retry",False)
                if type(retry) is not bool: raise TaskError("INVALID_OPERATION")
                return self._respond(start_response,"202 Accepted",self.store.accept_completion(body,retry))
            elif method=="GET" and re.fullmatch(r"/suite/v1/operations/[0-9a-f]{32}",path or ""):
                if not is_writer: return self._respond(start_response,"403 Forbidden",{"error":"READ_ONLY"})
                result=self.store.operation(path.rsplit("/",1)[1])
            elif is_writer:
                return self._respond(start_response,"403 Forbidden",{"error":"TASK_WRITE_SCOPE_ONLY"})
            elif method == "GET" and path == "/suite/v1/task-collections":
                result = self.store.collections()
            elif method == "GET" and path == "/suite/v1/tasks/snapshot":
                query = parse_qs(environ.get("QUERY_STRING", ""), max_num_fields=3,
                                 strict_parsing=True)
                if any(k not in {"revision", "offset"} or len(v) != 1 for k, v in query.items()):
                    raise TaskError("INVALID_CURSOR")
                result = self.store.page(query.get("revision", [None])[0],
                                         int(query.get("offset", ["0"])[0]))
            elif method == "PUT" and path == "/suite/v1/task-selection":
                if not is_admin:
                    return self._respond(start_response, "403 Forbidden", {"error": "READ_ONLY"})
                size = int(environ.get("CONTENT_LENGTH") or "0")
                if size <= 0 or size > 4096:
                    raise TaskError("LIMIT_EXCEEDED")
                raw = environ["wsgi.input"].read(size)
                if len(raw) != size:
                    raise TaskError("INVALID_REQUEST")
                body = json.loads(raw)
                if not isinstance(body, dict) or set(body) != {"collection_ids", "revision"}:
                    raise TaskError("INVALID_SELECTION")
                result = {"selection_revision": self.store.select(body["collection_ids"], body["revision"])}
            else:
                return self._respond(start_response, "404 Not Found", {"error": "NOT_FOUND"})
            return self._respond(start_response, "200 OK", result)
        except TaskError as exc:
            status = "404 Not Found" if exc.code=="NOT_FOUND" else "409 Conflict" if exc.code in {"REVISION_CONFLICT", "SNAPSHOT_NOT_READY","CONFLICT","OPERATION_CONFLICT"} else "400 Bad Request"
            return self._respond(start_response, status, {"error": exc.code})
        except (ValueError, UnicodeError, KeyError, TypeError):
            return self._respond(start_response, "400 Bad Request", {"error": "INVALID_REQUEST"})
        except Exception:
            # Do not leak task content, local paths, credentials, or DB errors.
            return self._respond(start_response, "503 Service Unavailable", {"error": "UNAVAILABLE"})

    @staticmethod
    def _respond(start_response, status, body):
        data = packed(body)
        start_response(status, [("Content-Type", "application/json; charset=utf-8"),
                                ("Content-Length", str(len(data))), ("Cache-Control", "no-store"),
                                ("X-Content-Type-Options", "nosniff")])
        return [data]
