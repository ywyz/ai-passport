"""Concurrent loopback WSGI server for the translation preview/service."""

from socketserver import ThreadingMixIn
from wsgiref.simple_server import WSGIRequestHandler, WSGIServer, make_server


class ThreadingWSGIServer(ThreadingMixIn, WSGIServer):
    """One request per thread so status/cancel stay live during ASR/MT."""

    daemon_threads = True
    allow_reuse_address = True
    block_on_close = False


class QuietHandler(WSGIRequestHandler):
    def log_message(self, format, *args):
        pass  # Request paths and bodies are private; never log them.


def serve(app, host="127.0.0.1", port=8788):
    if host not in {"127.0.0.1", "::1"}:
        raise ValueError("loopback host required")
    with make_server(host, port, app, server_class=ThreadingWSGIServer,
                     handler_class=QuietHandler) as server:
        server.serve_forever()
