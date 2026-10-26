#!/usr/bin/env python3
"""Prepare private, encrypted USB provisioning images; never flash anything.

Run using the activated ESP-IDF Python. Secrets are read from a private file or
hidden prompts, never CLI values. These images are excluded from firmware archives.
"""
import argparse
import csv
import getpass
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import struct
import subprocess
import sys
import zlib
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener


def validate(origin, token, completion_token=None):
    parsed = urlsplit(origin)
    labels = parsed.netloc.split(".")
    if (parsed.scheme != "https" or len(origin) > 192 or parsed.path or parsed.query
            or parsed.fragment or not labels or any(not re.fullmatch(
                r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?", label) for label in labels)
            or not re.fullmatch(r"[A-Za-z0-9_-]{32,128}", token)):
        raise ValueError("Invalid HTTPS origin or read token")
    if completion_token is not None and (completion_token==token or not re.fullmatch(r"[A-Za-z0-9_-]{32,128}",completion_token)):
        raise ValueError("Distinct completion token required")


def private_directory(path):
    path.mkdir(mode=0o700, parents=True, exist_ok=False)
    if os.name == "nt":
        # Use SIDs so ACL setup also works on localized Windows installations.
        identity = subprocess.run(["whoami", "/user", "/fo", "csv", "/nh"],
                                  capture_output=True, text=True, check=True).stdout
        sid = next(csv.reader([identity.strip()]))[1]
        subprocess.run(["icacls", str(path), "/inheritance:r", "/grant:r",
                        f"*{sid}:(OI)(CI)F", "*S-1-5-18:(OI)(CI)F",
                        "*S-1-5-32-544:(OI)(CI)F"], check=True, capture_output=True)


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def probe(origin, token):
    request = Request(origin + "/suite/v1/tasks/snapshot", headers={
        "Authorization": "Bearer " + token, "Accept": "application/json"})
    with build_opener(NoRedirect()).open(request, timeout=15) as response:
        raw = response.read(32769)
        if response.status != 200 or len(raw) > 32768:
            raise ValueError("Task API verification failed")
        page = json.loads(raw)
        if page.get("schema") != 1 or page.get("read_only") is not True:
            raise ValueError("Task API is not the expected read-only service")
        return {"https_probe": "PASS", "read_only": True}


def prepare(origin, token, output, completion_token=None):
    validate(origin, token,completion_token)
    private_directory(output)
    source = output / "input.csv"
    keypath = output / "taskkeys.bin"
    image = output / "taskcfg.bin"
    try:
        keys = secrets.token_bytes(64)
        key_data = keys + struct.pack("<I", zlib.crc32(keys, 0xffffffff)) + b"\xff" * (4096-68)
        keypath.write_bytes(key_data)
        with source.open("x", encoding="utf-8", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerows([("key", "type", "encoding", "value"),
                ("tasks", "namespace", "", ""), ("schema", "data", "u32", "1"),
                ("origin", "data", "string", origin), ("token", "data", "string", token)])
            if completion_token:
                writer.writerow(("write_token","data","string",completion_token))
        for path in (source, keypath):
            path.chmod(0o600)
        subprocess.run([sys.executable, "-m", "esp_idf_nvs_partition_gen", "encrypt",
                        str(source), str(image), "0x3000", "--inputkey", str(keypath)],
                       check=True, capture_output=True)
        if image.stat().st_size != 0x3000 or token.encode() in image.read_bytes() or (completion_token and completion_token.encode() in image.read_bytes()):
            raise ValueError("Encrypted image verification failed")
        image.chmod(0o600)
        receipt = {"schema": 1, "taskcfg": {"offset": "0x5c0000", "size": 12288},
                   "taskkeys": {"offset": "0x5c3000", "size": 4096},
                   "flashed": False, "hardware_flash_encryption": False,
                   "completion_configured":bool(completion_token),
                   "files": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                             for p in (image, keypath)}}
        (output / "manifest.json").write_text(json.dumps(receipt, indent=2) + "\n")
        return receipt
    finally:
        source.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--origin", required=True, help="HTTPS service origin only")
    parser.add_argument("--companion-config", type=Path, help="Private file; copy device and optional completion tokens only")
    parser.add_argument("--output", required=True, type=Path, help="New private directory")
    parser.add_argument("--probe", action="store_true", help="Verify HTTPS read before packaging")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    try:
        relative = output.relative_to(root)
    except ValueError:
        relative = None
    if relative is not None:
        ignored = subprocess.run(["git", "check-ignore", "--quiet", str(relative / "probe")], cwd=root)
        if ignored.returncode:
            raise ValueError("Provisioning images must stay outside tracked sources")
    if args.companion_config:
        path = args.companion_config
        if path.is_symlink() or (os.name != "nt" and path.stat().st_mode & 0o077):
            raise ValueError("Private config permissions required")
        config = json.loads(path.read_text(encoding="utf-8"))
        token = config["device_token"]
        if token == config.get("admin_token"):
            raise ValueError("Distinct device read token required")
        completion_token=config.get("completion_token")
        if completion_token and completion_token==config.get("admin_token"):
            raise ValueError("Distinct completion token required")
    else:
        token = getpass.getpass("Device READ token (hidden): ")
        completion_token=None
    validate(args.origin, token,completion_token)
    if args.probe:
        probe(args.origin, token)
    prepare(args.origin, token, output,completion_token)
    print("Private USB configuration prepared and verified; no device was written.")


if __name__ == "__main__":
    try:
        main()
    except Exception:
        print("Device configuration failed; review private input, permissions, API and IDF environment.", file=sys.stderr)
        sys.exit(1)
