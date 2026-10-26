"""Private local configuration, CalDAV read jobs, and loopback API preview."""

import argparse
import getpass
import json
import os
from pathlib import Path
import secrets
import sqlite3
import sys
from wsgiref.simple_server import make_server, WSGIRequestHandler
from zoneinfo import ZoneInfo

from .caldav import CalDAV, Transport
from .common import TaskError, stable_id
from .service import API, discover, refresh
from .store import Store


def read_config(path):
    path = Path(path)
    if path.is_symlink() or (os.name != "nt" and path.stat().st_mode & 0o077):
        raise TaskError("PRIVATE_FILE_REQUIRED")
    if path.stat().st_size > 8192:
        raise TaskError("INVALID_CONFIG")
    value = json.loads(path.read_text(encoding="utf-8"))
    required={
        "base_url", "username", "app_password", "display_zone", "device_token", "admin_token"
    }
    if not isinstance(value, dict) or set(value) not in (required,required|{"completion_token"}) or any(not isinstance(v, str) for v in value.values()):
        raise TaskError("INVALID_CONFIG")
    ZoneInfo(value["display_zone"])
    return value


def configure(path):
    """No secret command-line arguments, echo, or accidental overwrite."""
    path = Path(path)
    if path.exists():
        raise TaskError("CONFIG_EXISTS")
    base = input("Nextcloud HTTPS URL: ").strip()
    user = getpass.getpass("Nextcloud login name (hidden): ").strip()
    password = getpass.getpass("Nextcloud app password (hidden): ")
    Transport(base, user, password)  # Validate before saving; no request.
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    value = {"base_url": base, "username": user, "app_password": password,
             "display_zone": "Asia/Shanghai", "device_token": secrets.token_urlsafe(32),
             "admin_token": secrets.token_urlsafe(32)}
    with open(path, "x", encoding="utf-8", opener=lambda p, f: os.open(p, f, 0o600)) as output:
        json.dump(value, output, ensure_ascii=False, indent=2)


class QuietHandler(WSGIRequestHandler):
    def log_message(self, format, *args):
        pass  # Access URLs can be private; do not record them.


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, help="Private JSON file, outside tracked sources")
    parser.add_argument("--database", help="Private SQLite cache (required except configure)")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("configure")
    sub.add_parser("discover")
    sub.add_parser("collections")
    select = sub.add_parser("select")
    select.add_argument("ids", nargs="*")
    select.add_argument("--revision", required=True, type=int)
    sub.add_parser("refresh")
    sub.add_parser("resume-auth")
    sub.add_parser("status")
    serve = sub.add_parser("preview")
    serve.add_argument("--port", type=int, default=8787)
    args = parser.parse_args(argv)
    try:
        if args.command == "configure":
            configure(args.config)
            print("Private configuration created.")
            return 0
        if not args.database:
            raise TaskError("DATABASE_REQUIRED")
        config = read_config(args.config)
        transport = Transport(config["base_url"], config["username"], config["app_password"])
        account = stable_id(transport.dav_url, config["username"])
        store = Store(args.database, account)
        client = CalDAV(transport, account, config["display_zone"])
        if args.command == "discover":
            print(json.dumps({"collections": len(discover(client, store))}))
        elif args.command == "collections":
            # An explicit local inspection command; never call it in shared logs.
            print(json.dumps(store.collections(), ensure_ascii=False, indent=2))
        elif args.command == "select":
            print(json.dumps({"selection_revision": store.select(args.ids, args.revision)}))
        elif args.command == "refresh":
            print(json.dumps({"snapshot_revision": refresh(client, store)}))
        elif args.command == "resume-auth":
            store.resume_auth()
            print("Authentication retries explicitly re-enabled.")
        elif args.command == "preview":
            api = API(store, config["device_token"], config["admin_token"])
            with make_server("127.0.0.1", args.port, api, handler_class=QuietHandler) as server:
                print("Loopback API preview started; stop with Ctrl+C.", flush=True)
                server.serve_forever()
        else:
            state = store.state()
            print(json.dumps({"collection_count": len(json.loads(state["collections"])),
                              "selected_count": len(json.loads(state["selection"])),
                              "selection_revision": state["selection_revision"],
                              "snapshot_revision": state["revision"], "downloaded_at": state["downloaded"],
                              "last_error": state["last_error"], "auth_halted": bool(state["auth_halted"])}))
        return 0
    except TaskError as exc:
        print(exc.code, file=sys.stderr)
        return 1
    except (OSError, ValueError, KeyError, sqlite3.Error):
        print("LOCAL_CONFIG_OR_STORAGE_ERROR", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
