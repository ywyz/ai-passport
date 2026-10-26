"""Start the isolated translation service on loopback for local inspection."""

import argparse
import sys

from .common import TranslationError
from .server import serve
from .wsgi import build


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True,
                        help="Private JSON file outside tracked sources")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8788)
    args = parser.parse_args(argv)
    if args.host not in {"127.0.0.1", "::1"} or not 1 <= args.port <= 65535:
        print("INVALID_CONFIG", file=sys.stderr)
        return 1
    try:
        app = build(args.config)
    except TranslationError as exc:
        print(exc.code, file=sys.stderr)
        return 1
    except Exception:
        print("LOCAL_CONFIG_OR_STORAGE_ERROR", file=sys.stderr)
        return 1
    try:
        print("Translation service listening on loopback; stop with Ctrl+C.",
              flush=True)
        serve(app, args.host, args.port)
    except KeyboardInterrupt:
        pass
    finally:
        app.jobs.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
