#!/usr/bin/env python3
"""Send one translation configuration frame over native USB Serial/JTAG.

The origin and capability token are read from a separate private JSON file or
hidden prompts, never from command-line arguments. This tool never flashes,
erases, generates NVS images/keys, or reads credentials back. It prints no
hostname, token, or raw serial data.
"""
import argparse
import getpass
import json
import os
from pathlib import Path
import re
import secrets
import sys
import time
from urllib.parse import urlsplit

STATUSES = {"OK", "INVALID", "BUSY", "STORAGE", "UNVERIFIED"}
MAX_FRAME = 1024
MAX_ACK = 2048
_ANSI_CSI = re.compile(rb"\x1b\[[0-?]*[ -/]*[@-~]")


def validate(origin, token):
    parsed = urlsplit(origin)
    labels = parsed.netloc.split(".")
    if (parsed.scheme != "https" or len(origin) > 192 or parsed.path or parsed.query
            or parsed.fragment or parsed.username or parsed.password
            or not labels or any(not re.fullmatch(
                r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?", label) for label in labels)
            or not re.fullmatch(r"[A-Za-z0-9_-]{32,128}", token)):
        raise ValueError("Invalid HTTPS origin or capability token")


def load_private_config(path):
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        raise ValueError("Private configuration file required")
    if os.name != "nt" and path.stat().st_mode & 0o077:
        raise ValueError("Private configuration permissions required")
    config = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(config, dict) or set(config) != {"origin", "token"}:
        raise ValueError("Private configuration fields are invalid")
    validate(config["origin"], config["token"])
    return config["origin"], config["token"]


def build_frame(origin, token, request_id=None):
    validate(origin, token)
    request_id = request_id or secrets.token_hex(16)
    if not re.fullmatch(r"[0-9a-f]{32}", request_id):
        raise ValueError("Request ID must be 32 lowercase hex characters")
    frame = json.dumps({
        "schema": 1,
        "operation": "set_translation_config",
        "request_id": request_id,
        "origin": origin,
        "token": token,
    }, separators=(",", ":"), ensure_ascii=True) + "\n"
    if len(frame.encode("utf-8")) > MAX_FRAME:
        raise ValueError("Provisioning frame exceeds the device limit")
    return frame.encode("utf-8"), request_id


def parse_ack(raw, request_id):
    if not raw or len(raw) > MAX_ACK:
        raise ValueError("Acknowledgement is missing or oversized")
    try:
        ack = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError("Acknowledgement is not strict JSON") from error
    if (not isinstance(ack, dict) or set(ack) != {"request_id", "status", "verified"}
            or ack.get("request_id") != request_id
            or ack.get("status") not in STATUSES or not isinstance(ack.get("verified"), bool)):
        raise ValueError("Acknowledgement does not match the request")
    if ack["status"] == "OK" and ack["verified"] is not True:
        raise ValueError("Unverified success acknowledgement rejected")
    return ack["status"], ack["verified"]


def _console_prefix_end(line):
    """Return the offset of a standalone ACK after a recognized console prefix.

    Only printable console text and complete ANSI CSI escapes are accepted.
    JSON structural braces/brackets or quotes in the prefix are not skipped, so
    a nested or truncated object/array cannot be mistaken for a standalone ACK.
    """
    at = 0
    while at < len(line):
        byte = line[at]
        if byte == 0x1b:
            match = _ANSI_CSI.match(line, at)
            if match is None:
                raise ValueError("Unrecognized console escape")
            at = match.end()
            continue
        if byte in b'{}[]"':
            return at
        if byte < 0x20 or byte == 0x7f:
            raise ValueError("Control byte in console prefix")
        at += 1
    return at


def _strict_ack(line, request_id):
    """Return (status, verified) for a matching ack, None for unrelated lines.

    Startup, Tasks and weather logs share the console. The raw ACK is written
    through usb_serial_jtag_write_bytes and does not share the VFS log lock, so
    an exact ACK may follow a partial or ANSI console prefix on the same line.
    The prefix scanner strips only recognized ANSI CSI escapes and printable
    console text; the whole remaining line must be one exact ACK object. Nested
    or truncated JSON envelopes, concatenated objects, extra keys, duplicates,
    trailing data, wrong IDs, and control bytes inside JSON are rejected.
    """
    marker = request_id.encode("ascii")
    if not line:
        return None
    try:
        start = _console_prefix_end(line)
    except ValueError:
        if marker in line:
            raise ValueError("Malformed console prefix before acknowledgement")
        return None
    if start >= len(line) or line[start:start + 1] != b"{":
        if marker in line:
            raise ValueError("Acknowledgement is not a standalone object")
        return None
    prefix = line[:start]
    if marker in prefix:
        raise ValueError("Multiple acknowledgement objects")

    def no_duplicates(pairs):
        seen = set()
        for key, value in pairs:
            if key in seen:
                raise ValueError("Duplicate acknowledgement key")
            seen.add(key)
        return dict(pairs)

    try:
        ack = json.loads(line[start:].decode("utf-8"),
                         object_pairs_hook=no_duplicates)
    except (UnicodeDecodeError, json.JSONDecodeError, ValueError):
        if marker in line:
            raise ValueError("Malformed acknowledgement for this request")
        return None
    if not isinstance(ack, dict):
        if marker in line:
            raise ValueError("Acknowledgement is not a standalone object")
        return None
    if ack.get("request_id") != request_id:
        if marker in line:
            raise ValueError("Acknowledgement is not a standalone object")
        return None
    if (set(ack) != {"request_id", "status", "verified"}
            or ack.get("status") not in STATUSES
            or not isinstance(ack.get("verified"), bool)
            or (ack["status"] == "OK" and ack["verified"] is not True)
            or (ack["status"] != "OK" and ack["verified"] is not False)):
        raise ValueError("Acknowledgement does not match the request")
    return ack["status"], ack["verified"]

def configure(port, frame, request_id, timeout=15.0, stage_state=None):
    def mark(name):
        if stage_state is not None:
            stage_state["name"] = name

    try:
        import serial
    except ImportError as error:
        raise RuntimeError("pyserial is required for device provisioning") from error
    mark("connect")
    write_timeout = None if urlsplit(port).scheme.lower() == "rfc2217" else 2.0
    connection = serial.serial_for_url(port, baudrate=115200, timeout=0.25,
                                       write_timeout=write_timeout, do_not_open=True)
    # Native USB CDC does not need modem-line resets; keep both lines inactive
    # unless the operator changes this deliberately.
    connection.dsrdtr = False
    connection.rtscts = False
    connection.xonxoff = False
    connection.dtr = False
    connection.rts = False
    mark("open")
    connection.open()
    try:
        mark("write")
        connection.reset_input_buffer()
        connection.write(frame)
        connection.flush()
        mark("ack")
        deadline = time.monotonic() + timeout
        buffered = bytearray()
        total = 0
        discarding = False
        while time.monotonic() < deadline:
            chunk = connection.read(1)
            if not chunk:
                continue
            total += len(chunk)
            if total > MAX_ACK * 32:
                raise ValueError("Too much unrelated serial data before the acknowledgement")
            for byte in chunk:
                if byte == 0x0a:
                    if discarding:
                        discarding = False
                        buffered.clear()
                        continue
                    line = bytes(buffered)
                    buffered.clear()
                    result = _strict_ack(line, request_id)
                    if result is not None:
                        return result
                    continue
                if discarding:
                    continue
                if len(buffered) >= MAX_ACK:
                    # Drop an oversized log line without letting it extend the
                    # deadline; a real ack is far smaller than this bound.
                    discarding = True
                    buffered.clear()
                    continue
                buffered.append(byte)
        raise TimeoutError("No matching acknowledgement before the deadline")
    finally:
        connection.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True,
                        help="Serial port or rfc2217:// URL for native USB Serial/JTAG")
    parser.add_argument("--config", type=Path,
                        help="Private JSON file with origin and token (never CLI credentials)")
    parser.add_argument("--timeout", type=float, default=15.0,
                        help="Bounded acknowledgement wait in seconds (1-55)")
    parser.add_argument("--print-request-id", action="store_true",
                        help="Print only the request ID; never the token or origin")
    args = parser.parse_args()
    if not 1.0 <= args.timeout <= 55.0:
        parser.error("--timeout must be between 1 and 55 seconds")
    stage_state = {"name": "config"}
    try:
        if args.config:
            origin, token = load_private_config(args.config)
        else:
            origin = input("Translation HTTPS origin: ").strip()
            token = getpass.getpass("Translation capability token (hidden): ")
        validate(origin, token)
        stage_state["name"] = "frame"
        frame, request_id = build_frame(origin, token)
        if args.print_request_id:
            print(f"request_id={request_id}")
        stage_state["name"] = "transport"
        status, verified = configure(args.port, frame, request_id,
                                     args.timeout, stage_state)
        print(f"Translation configuration status={status} verified={str(verified).lower()}; no firmware was flashed.")
        return 0 if status == "OK" and verified else 1
    except Exception as error:
        print(f"Translation provisioning failed; stage={stage_state['name']}; "
              f"exception={type(error).__name__}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
