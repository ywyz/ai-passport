"""Bounded MiMo ASR. Provider credentials and audio never enter device config."""
import base64
import json
import math
import os
from pathlib import Path
import re
import socket
import ssl
import struct
import time
from http.client import HTTPException
from urllib.error import HTTPError, URLError
from urllib.request import HTTPSHandler, Request, build_opener

from .caldav import NoRedirect
from .common import TaskError, packed

MAX_PCM = 12 * 16000 * 2
MAX_WAV = MAX_PCM + 44
MAX_TITLE = 512
ENDPOINT = "https://api.xiaomimimo.com/v1/chat/completions"


def title(value):
    if (not isinstance(value, str) or not value.strip() or value != value.strip()
            or len(value.encode("utf-8")) > MAX_TITLE
            or any(ord(c) < 32 or 127 <= ord(c) < 160 for c in value)):
        raise TaskError("INVALID_TITLE")
    return value


def wav_seconds(raw):
    # The device produces exactly this PCM envelope. Reject extra chunks,
    # compressed audio, trailing bytes and inconsistent sizes before billing.
    if not isinstance(raw, bytes) or not 44 + 1600 <= len(raw) <= MAX_WAV:
        raise TaskError("INVALID_AUDIO")
    try:
        fields = struct.unpack("<4sI4s4sIHHIIHH4sI", raw[:44])
    except struct.error:
        raise TaskError("INVALID_AUDIO") from None
    if fields != (b"RIFF", len(raw)-8, b"WAVE", b"fmt ", 16, 1, 1,
                  16000, 32000, 2, 16, b"data", len(raw)-44) or (len(raw)-44) % 2:
        raise TaskError("INVALID_AUDIO")
    return math.ceil((len(raw)-44)/32000)


class MiMo:
    def __init__(self, api_key, daily_seconds=300):
        if (not isinstance(api_key, str) or not re.fullmatch(r"[!-~]{16,256}", api_key)
                or type(daily_seconds) is not int or not 1 <= daily_seconds <= 3600):
            raise TaskError("INVALID_CONFIG")
        self.api_key, self.daily_seconds = api_key, daily_seconds
        self.opener = build_opener(NoRedirect(), HTTPSHandler(context=ssl.create_default_context()))

    @classmethod
    def from_file(cls, filename):
        path = Path(filename)
        if path.is_symlink() or (os.name != "nt" and path.stat().st_mode & 0o077):
            raise TaskError("PRIVATE_FILE_REQUIRED")
        if path.stat().st_size > 2048:
            raise TaskError("INVALID_CONFIG")
        value = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(value, dict) or set(value) != {"api_key", "daily_seconds"}:
            raise TaskError("INVALID_CONFIG")
        return cls(**value)

    def transcribe(self, raw):
        wav_seconds(raw)
        body = packed({"model":"mimo-v2.5-asr", "stream":False,
                       "messages":[{"role":"user", "content":[{"type":"input_audio",
                           "input_audio":{"data":"data:audio/wav;base64," + base64.b64encode(raw).decode()}}]}],
                       "asr_options":{"language":"zh"}})
        req = Request(ENDPOINT, data=body, method="POST", headers={
            "Authorization":"Bearer " + self.api_key, "Content-Type":"application/json",
            "Accept":"application/json", "Accept-Encoding":"identity"})
        try:
            with self.opener.open(req, timeout=10) as response:
                if response.status != 200 or response.headers.get("Content-Encoding", "identity") != "identity":
                    raise TaskError("SPEECH_UNAVAILABLE")
                result = bytearray(); deadline = time.monotonic() + 25
                while True:
                    block = response.read1(min(4096, 8193-len(result)))
                    if time.monotonic() > deadline: raise TaskError("SPEECH_TIMEOUT")
                    if not block: break
                    result.extend(block)
                    if len(result) > 8192: raise TaskError("LIMIT_EXCEEDED")
                value = json.loads(result)
                choices = value["choices"]
                if len(choices) != 1 or choices[0].get("finish_reason") != "stop":
                    raise TaskError("INVALID_TRANSCRIPT")
                return title(choices[0]["message"]["content"].strip())
        except HTTPError as error:
            error.close()
            raise TaskError("SPEECH_AUTH_FAILED" if error.code in (401,403) else "SPEECH_UNAVAILABLE") from None
        except (URLError, TimeoutError, socket.timeout, OSError, HTTPException):
            raise TaskError("SPEECH_UNAVAILABLE") from None
        except (ValueError, KeyError, TypeError, AttributeError):
            raise TaskError("INVALID_TRANSCRIPT") from None
