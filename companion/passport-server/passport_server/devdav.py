"""Minimal WebDAV server for local tests and demos (NOT production).

Implements MKCOL, PUT, GET, PROPFIND (depth 0/1) as a simple file-backed
remote for exercising the adapter locally. Real WebDAV acceptance is a
import html
separate, NOT VERIFIED item until a real server is used.
"""
from __future__ import annotations

import base64
import os
import threading
import urllib.parse

TEST_DAV_USER = "passport"
TEST_DAV_PASSWORD = "test-dav-password"
_AUTH = "Basic " + base64.b64encode(
    f"{TEST_DAV_USER}:{TEST_DAV_PASSWORD}".encode()).decode()
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def make_dav_handler(root: str):
    class DavHandler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def _path(self) -> str:
            rel = urllib.parse.urlsplit(self.path).path.lstrip("/")
            path = os.path.realpath(os.path.join(root, rel))
            if os.path.commonpath([os.path.realpath(root), path]) != os.path.realpath(root):
                return ""
            return path

        def _respond(self, status: int, body: bytes = b"", ctype="text/xml") -> None:
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _xml_ok(self, path: str, depth: str) -> None:
            entries = []
            title = "/" + os.path.relpath(path, root).replace(os.sep, "/")
            if os.path.exists(path):
                entries.append((title if title == "/" else title, True, 0))
            if depth == "1" and os.path.isdir(path):
                for name in sorted(os.listdir(path)):
                    full = os.path.join(path, name)
                    # hrefs: URL-encoded, relative path from root
                    href = "/" + os.path.relpath(full, root).replace(os.sep, "/")
                    entries.append((href, os.path.isdir(full),
                                    os.path.getsize(full) if os.path.isfile(full) else 0))
            body = ("<?xml version=\"1.0\"?><D:multistatus"
                    " xmlns:D=\"DAV:\">" + "".join(
                        f"<D:response><D:href>{html.escape(h)}</D:href>"
                        "<D:propstat><D:prop>"
                        f"<D:resourcetype><D:collection/></D:resourcetype></D:prop>"
                        "<D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>"
                        if isdir else
                        f"<D:response><D:href>{html.escape(h)}</D:href>"
                        "<D:propstat><D:prop></D:prop>"
                        "<D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>"
                        for h, isdir, _ in entries).encode() + "</D:multistatus>")
            self._respond(207, body)

        def do_PROPFIND(self):  # noqa: N802
            if self.headers.get("Authorization", "") != _AUTH:
                self._respond(401)
                return
            path = self._path()
            if not path or not os.path.exists(path):
                self._respond(404)
                return
            self._xml_ok(path, self.headers.get("Depth", "0"))

        def _auth(self) -> bool:
            return self.headers.get("Authorization", "") == _AUTH

        def do_GET(self):  # noqa: N802
            path = self._path()
            if not path or not os.path.isfile(path):
                self._respond(404)
                return
            with open(path, "rb") as handle:
                body = handle.read()
            self._respond(200, body, "application/octet-stream")

        def do_PUT(self):  # noqa: N802
            path = self._path()
            if not path:
                self._respond(400)
                return
            length = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(length)
            existed = os.path.exists(path)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as handle:
                handle.write(body)
            self._respond(204 if existed else 201)

        def do_MKCOL(self):  # noqa: N802
            path = self._path()
            if not path:
                self._respond(400)
                return
            if os.path.exists(path):
                self._respond(405)
            else:
                os.makedirs(path, exist_ok=True)
                self._respond(201)

        def log_message(self, fmt, *args):  # noqa: A003
            pass

    return DavHandler


def start_test_dav(root: str, port: int = 0) -> tuple[ThreadingHTTPServer, str]:
    server = ThreadingHTTPServer(("127.0.0.1", port), make_dav_handler(root))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, f"http://127.0.0.1:{server.server_port}/dav/"
