"""Local test instance entrypoint (development only).

    python -m passport_server --data-dir ./passport-data --port 8642 \
        [--admin-password ENV:PASSPORT_ADMIN_PASSWORD]

- Local single-user test instance, plain HTTP on localhost.
- Do NOT deploy to production; production deployment is separately
  authorized and would place a TLS-terminating proxy (device verifies
  hostname and chain against a privately installed CA) in front.
- Never finds admin passwords, DAV passwords, binding codes or tokens
  outside environment / config files described in the README.
"""
from __future__ import annotations

import argparse
import os
import sys

from .server import Handler, Main
from http.server import ThreadingHTTPServer


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Passport test server (D1a)")
    parser.add_argument("--data-dir", default="./passport-data")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8642)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)

    os.makedirs(args.data_dir, exist_ok=True)
    app = Main(args.data_dir, listen=args.host, port=args.port, quiet=args.quiet)
    app.core.sweep_expired()
    app.load_dav_config()
    if app.core.bootstrap_required():
        password = os.environ.get("PASSPORT_ADMIN_PASSWORD", "")
        if password:
            try:
                app.core.reset_admin_auth(password)
            except Exception:
                app.core.bootstrap_admin(password)
            print("admin credentials initialized (from environment)", flush=True)
        else:
            # Generate once, print to the terminal, never to persistent logs.
            import secrets as _secrets
            generated = _secrets.token_urlsafe(12)
            app.core.bootstrap_admin(generated)
            print("Admin password (store privately, shown once):", generated,
                  flush=True)
    print(f"passport server listening on http://{args.host}:{args.port}/",
          flush=True)
    Handler.main = app
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        app.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
