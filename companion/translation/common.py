"""Bounded wire validation shared by the translation API and job runner."""

import hashlib
import json
import re
import struct
import uuid

MAX_TEXT_BYTES = 2048
MAX_TEXT_SCALARS = 512
MIN_WAV_DATA = 9600
MAX_WAV_BYTES = 320044
MAX_PAYLOAD_BYTES = 4096
MAX_METADATA_ROWS = 64
TEXT_TTL_SECONDS = 120
LANGUAGES = ("en", "zh-CN")
DIRECTIONS = (("en", "zh-CN"), ("zh-CN", "en"))
REQUEST_FIELDS = {
    "schema", "operation_id", "source_lang", "target_lang", "format",
    "audio_sha256", "audio_bytes",
}

ID_PATTERN = re.compile(r"[0-9a-f]{32}\Z")
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}\Z")
TOKEN_PATTERN = re.compile(r"[A-Za-z0-9_-]{32,128}\Z")


class TranslationError(Exception):
    """Only a fixed code crosses a public boundary; never text or audio."""

    def __init__(self, code):
        self.code = code
        super().__init__(code)


def canonical(value):
    try:
        return json.dumps(value, ensure_ascii=False, separators=(",", ":"),
                          sort_keys=True, allow_nan=False).encode("utf-8")
    except (TypeError, ValueError):
        raise TranslationError("INVALID_REQUEST") from None


def _unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError("duplicate key")
        value[key] = item
    return value


def _reject_constant(_value):
    raise ValueError("non-finite number")


def strict_json_loads(raw, code="INVALID_REQUEST"):
    """Decode strict UTF-8 JSON, rejecting duplicates and non-finite values."""
    if type(raw) is bytes:
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            raise TranslationError(code) from None
    elif type(raw) is str:
        text = raw
    else:
        raise TranslationError(code)
    try:
        return json.loads(text, object_pairs_hook=_unique_object,
                          parse_constant=_reject_constant)
    except (ValueError, TypeError, RecursionError):
        raise TranslationError(code) from None


def sha256_hex(raw):
    if type(raw) is not bytes:
        raise TranslationError("INVALID_REQUEST")
    return hashlib.sha256(raw).hexdigest()


def validate_text(value):
    if type(value) is not str:
        raise TranslationError("INVALID_TEXT")
    try:
        raw = value.encode("utf-8")
    except UnicodeEncodeError:
        raise TranslationError("INVALID_TEXT") from None
    if not raw or "\x00" in value:
        raise TranslationError("INVALID_TEXT")
    if len(raw) > MAX_TEXT_BYTES or len(value) > MAX_TEXT_SCALARS:
        raise TranslationError("TEXT_LIMIT")
    return value


def validate_direction(source_lang, target_lang):
    if type(source_lang) is not str or type(target_lang) is not str:
        raise TranslationError("INVALID_DIRECTION")
    if (source_lang, target_lang) not in DIRECTIONS:
        raise TranslationError("INVALID_DIRECTION")


def validate_wav(raw):
    if type(raw) is not bytes:
        raise TranslationError("INVALID_AUDIO")
    if len(raw) < 44 + MIN_WAV_DATA or len(raw) > MAX_WAV_BYTES:
        raise TranslationError("AUDIO_LIMIT")
    try:
        fields = struct.unpack("<4sI4s4sIHHIIHH4sI", raw[:44])
    except struct.error:
        raise TranslationError("INVALID_AUDIO") from None
    payload = len(raw) - 44
    expected = (b"RIFF", len(raw) - 8, b"WAVE", b"fmt ", 16, 1, 1,
                16000, 32000, 2, 16, b"data", payload)
    if fields != expected or payload & 1:
        raise TranslationError("INVALID_AUDIO")
    return {"bytes": len(raw), "data_bytes": payload,
            "seconds": (payload + 31999) // 32000}


def validate_request(body):
    if type(body) is not dict or set(body) != REQUEST_FIELDS:
        raise TranslationError("INVALID_REQUEST")
    if type(body["schema"]) is not int or body["schema"] != 1:
        raise TranslationError("INVALID_REQUEST")
    operation_id = body["operation_id"]
    if type(operation_id) is not str or not ID_PATTERN.fullmatch(operation_id):
        raise TranslationError("INVALID_OPERATION")
    validate_direction(body["source_lang"], body["target_lang"])
    if body["format"] != "wav":
        raise TranslationError("INVALID_FORMAT")
    digest = body["audio_sha256"]
    if type(digest) is not str or not SHA256_PATTERN.fullmatch(digest):
        raise TranslationError("INVALID_AUDIO")
    audio_bytes = body["audio_bytes"]
    if (type(audio_bytes) is not int or
            not 44 + MIN_WAV_DATA <= audio_bytes <= MAX_WAV_BYTES):
        raise TranslationError("AUDIO_LIMIT")
    return dict(body)


def fingerprint(request):
    return hashlib.sha256(canonical(request)).hexdigest()


def new_job_id():
    return uuid.uuid4().hex


def validate_job_id(value):
    if type(value) is not str or not ID_PATTERN.fullmatch(value):
        raise TranslationError("INVALID_JOB")
    return value


def token_valid(value):
    return type(value) is str and bool(TOKEN_PATTERN.fullmatch(value))
