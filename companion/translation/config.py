"""Private translation-service configuration and secure provider-key read.

The file contains paths and capability tokens, never an inline provider key.
Configuration and key files must be private; errors expose only fixed codes.
"""

import os
from pathlib import Path
import re

from .common import (MAX_METADATA_ROWS, TEXT_TTL_SECONDS, TranslationError,
                     strict_json_loads, token_valid)

REQUIRED = {"schema", "translation_token", "database", "provider_key_file",
            "daily_asr_seconds", "daily_mt_jobs"}
OPTIONAL = {"forbidden_tokens", "text_ttl", "audio_ttl", "queue_size", "cleanup_interval",
            "asr_timeout", "mt_timeout", "capacity"}
KEY_PATTERN = re.compile(r"[!-~]{16,256}\Z")
MAX_CONFIG_BYTES = 8192
MAX_KEY_FILE_BYTES = 2048
TASKS_STATE_DIR = Path("/var/lib/passport-tasks")


def _inside_tasks_state(path):
    try:
        Path(path).resolve().relative_to(TASKS_STATE_DIR)
        return True
    except (OSError, ValueError):
        return False


def _private_bytes(path, limit):
    path = Path(path)
    try:
        if path.is_symlink() or not path.is_file():
            raise TranslationError("INVALID_CONFIG")
        if os.name != "nt" and path.stat().st_mode & 0o077:
            raise TranslationError("PRIVATE_FILE_REQUIRED")
        if path.stat().st_size > limit:
            raise TranslationError("INVALID_CONFIG")
        return path.read_bytes()
    except TranslationError:
        raise
    except OSError:
        raise TranslationError("INVALID_CONFIG") from None


def read_config(path):
    value = strict_json_loads(_private_bytes(path, MAX_CONFIG_BYTES),
                              "INVALID_CONFIG")
    if type(value) is not dict or not REQUIRED <= set(value) <= REQUIRED | OPTIONAL:
        raise TranslationError("INVALID_CONFIG")
    if type(value["schema"]) is not int or value["schema"] != 1:
        raise TranslationError("INVALID_CONFIG")
    if not token_valid(value["translation_token"]):
        raise TranslationError("INVALID_CONFIG")
    database = value["database"]
    provider_file = value["provider_key_file"]
    for item in (database, provider_file):
        if type(item) is not str or not 1 <= len(item) <= 4096:
            raise TranslationError("INVALID_CONFIG")
        if not Path(item).is_absolute():
            raise TranslationError("INVALID_CONFIG")
    if database == provider_file:
        raise TranslationError("INVALID_CONFIG")
    if _inside_tasks_state(database):
        raise TranslationError("INVALID_CONFIG")
    if (type(value["daily_asr_seconds"]) is not int
            or not 1 <= value["daily_asr_seconds"] <= 86400):
        raise TranslationError("INVALID_CONFIG")
    if (type(value["daily_mt_jobs"]) is not int
            or not 1 <= value["daily_mt_jobs"] <= 100000):
        raise TranslationError("INVALID_CONFIG")
    forbidden = value.get("forbidden_tokens", [])
    if type(forbidden) is not list or len(forbidden) > 8:
        raise TranslationError("INVALID_CONFIG")
    if any(type(token) is not str for token in forbidden):
        raise TranslationError("INVALID_CONFIG")
    if len(set(forbidden)) != len(forbidden):
        raise TranslationError("INVALID_CONFIG")
    for token in forbidden:
        if not token_valid(token) or token == value["translation_token"]:
            raise TranslationError("INVALID_CONFIG")
    text_ttl = value.get("text_ttl", TEXT_TTL_SECONDS)
    if type(text_ttl) is not int or not 1 <= text_ttl <= 3600:
        raise TranslationError("INVALID_CONFIG")
    audio_ttl = value.get("audio_ttl", 300)
    if type(audio_ttl) is not int or not 1 <= audio_ttl <= 600:
        raise TranslationError("INVALID_CONFIG")
    queue_size = value.get("queue_size", 8)
    if type(queue_size) is not int or not 1 <= queue_size <= 32:
        raise TranslationError("INVALID_CONFIG")
    cleanup_interval = value.get("cleanup_interval", 1.0)
    if (type(cleanup_interval) not in (int, float)
            or not 0.01 <= cleanup_interval <= 60):
        raise TranslationError("INVALID_CONFIG")
    timeouts = []
    for name, default in (("asr_timeout", 20.0), ("mt_timeout", 15.0)):
        timeout = value.get(name, default)
        if type(timeout) not in (int, float) or not 1 <= timeout <= 60:
            raise TranslationError("INVALID_CONFIG")
        timeouts.append(float(timeout))
    capacity = value.get("capacity", MAX_METADATA_ROWS)
    if type(capacity) is not int or not 1 <= capacity <= MAX_METADATA_ROWS:
        raise TranslationError("INVALID_CONFIG")
    return {
        "schema": 1,
        "translation_token": value["translation_token"],
        "forbidden_tokens": tuple(forbidden),
        "database": database,
        "provider_key_file": provider_file,
        "daily_asr_seconds": value["daily_asr_seconds"],
        "daily_mt_jobs": value["daily_mt_jobs"],
        "text_ttl": text_ttl,
        "audio_ttl": audio_ttl,
        "queue_size": queue_size,
        "cleanup_interval": float(cleanup_interval),
        "asr_timeout": timeouts[0],
        "mt_timeout": timeouts[1],
        "capacity": capacity,
    }


def load_api_key(config):
    """Read only api_key from the referenced private file; never return JSON."""
    value = strict_json_loads(
        _private_bytes(config["provider_key_file"], MAX_KEY_FILE_BYTES),
        "INVALID_CONFIG")
    if type(value) is not dict:
        raise TranslationError("INVALID_CONFIG")
    key = value.get("api_key")
    if type(key) is not str or not KEY_PATTERN.fullmatch(key):
        raise TranslationError("INVALID_CONFIG")
    return key
