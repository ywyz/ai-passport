#!/usr/bin/env python3
"""Create a private translation config; never accept or print a provider key."""
import argparse
import json
import os
from pathlib import Path
import re
import secrets
import sys

TASKS_CONFIG_DIR = Path("/etc/passport-tasks")
TASKS_STATE_DIR = Path("/var/lib/passport-tasks")
KEY_PATTERN = re.compile(r"[!-~]{16,256}\Z")
MAX_KEY_BYTES = 2048


def fail(message):
    print(message, file=sys.stderr)
    return 1


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError("duplicate key")
        value[key] = item
    return value


def load_key(path):
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        raise ValueError("INVALID_PROVIDER_KEY_FILE")
    if os.name != "nt" and path.stat().st_mode & 0o077:
        raise ValueError("PRIVATE_FILE_REQUIRED")
    if path.stat().st_size > MAX_KEY_BYTES:
        raise ValueError("INVALID_PROVIDER_KEY_FILE")
    try:
        value = json.loads(path.read_text(encoding="utf-8"),
                           object_pairs_hook=unique_object)
    except (OSError, ValueError):
        raise ValueError("INVALID_PROVIDER_KEY_FILE") from None
    if type(value) is not dict:
        raise ValueError("INVALID_PROVIDER_KEY_FILE")
    key = value.get("api_key")
    if type(key) is not str or not KEY_PATTERN.fullmatch(key):
        raise ValueError("INVALID_PROVIDER_KEY_FILE")
    return key


def inside_tasks(path):
    try:
        Path(path).resolve().relative_to(TASKS_CONFIG_DIR)
        return True
    except (OSError, ValueError):
        return False


def inside_tasks_state(path):
    try:
        Path(path).resolve().relative_to(TASKS_STATE_DIR)
        return True
    except (OSError, ValueError):
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True,
                        help="New private translation config path")
    parser.add_argument("--provider-key-file", required=True,
                        help="Existing private JSON containing api_key")
    parser.add_argument("--database",
                        default="/var/lib/passport-translation/translation.sqlite3")
    parser.add_argument("--daily-asr-seconds", type=int, default=60)
    parser.add_argument("--daily-mt-jobs", type=int, default=6)
    args = parser.parse_args()

    output = Path(args.output)
    provider = Path(args.provider_key_file)
    if not 1 <= args.daily_asr_seconds <= 86400:
        return fail("INVALID_DAILY_ASR_SECONDS")
    if not 1 <= args.daily_mt_jobs <= 100000:
        return fail("INVALID_DAILY_MT_JOBS")
    if not Path(args.database).is_absolute():
        return fail("INVALID_DATABASE")
    if inside_tasks_state(args.database):
        return fail("REFUSING_TASKS_DATABASE")
    if output.resolve() == provider.resolve():
        return fail("CONFIG_OVERWRITES_PROVIDER_KEY")
    if inside_tasks(output):
        return fail("REFUSING_TO_WRITE_TASKS_CONFIG")
    if output.exists() or output.is_symlink():
        return fail("CONFIG_EXISTS")
    try:
        load_key(provider)
    except ValueError as exc:
        return fail(str(exc))

    token = secrets.token_urlsafe(32)
    value = {
        "schema": 1,
        "translation_token": token,
        "database": args.database,
        "provider_key_file": str(provider.resolve()),
        "daily_asr_seconds": args.daily_asr_seconds,
        "daily_mt_jobs": args.daily_mt_jobs,
    }
    output.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    try:
        with open(output, "x", encoding="utf-8",
                  opener=lambda path, flags: os.open(path, flags, 0o600)) as handle:
            json.dump(value, handle, indent=2)
            handle.write("\n")
    except OSError:
        return fail("CONFIG_WRITE_FAILED")
    print("Private translation config created. The provider key was referenced, "
          "not printed or copied.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
