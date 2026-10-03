"""WebDAV persistence adapter (contract section 9).

- Working SQLite stays on a local volume; only immutable resources and
  complete-snapshot material are laid out under the remote DAV root.
- Snapshots publish records.json + manifest.json first and `complete.json`
  LAST as the final completion marker; an upload without that marker is
  never a recoverable snapshot.
- Outages raise DavError with stable error codes; callers own retries/backoff
  (`retry_after_seconds` semantics, 60 s for temporary DAV failures).
"""
from __future__ import annotations

import base64
import http.client
import ssl
import urllib.parse


class DavError(Exception):
    def __init__(self, code: str, message: str, retry_after_seconds: int | None = None) -> None:
        super().__init__(message)
        self.code = code  # DAV_PENDING | DAV_QUOTA | DAV_PROTOCOL
        self.message = message
        self.retry_after_seconds = retry_after_seconds


def dav_quote(path: str) -> str:
    return urllib.parse.quote(path, safe="/._-@()"[:12])


class WebdavClient:
    def __init__(self, base_url: str, username: str, password: str, timeout: float = 30.0) -> None:
        parsed = urllib.parse.urlsplit(base_url)
        if parsed.scheme not in ("http", "https") or not parsed.netloc:
            raise ValueError("dav base_url must be http(s)://host[:port]/")
        self.scheme = parsed.scheme
        self.host = parsed.hostname
        self.port = parsed.port or (443 if parsed.scheme == "https" else 80)
        self.base_path = parsed.path.rstrip("/")
        self.auth = "Basic " + base64.b64encode(
            f"{username}:{password}".encode()).decode()
        self.timeout = timeout
        self._ssl = ssl.create_default_context() if self.scheme == "https" else None

    # -- low level -------------------------------------------------------
    def _request(self, method: str, path: str, body: bytes | None = None,
                 headers: dict | None = None, expect: tuple = (200, 201, 204)) -> http.client.HTTPResponse:
        if self._ssl is not None:
            conn = http.client.HTTPSConnection(self.host, self.port, context=self._ssl,
                                               timeout=self.timeout)
        else:
            conn = http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)
        req_headers = {"Authorization": self.auth, "User-Agent": "passport-server/1"}
        if body is not None:
            req_headers["Content-Length"] = str(len(body))
        req_headers.update(headers or {})
        try:
            conn.request(method, self.base_path.rstrip("/") + "/" + dav_quote(path.lstrip("/")),
                         body=body, headers=req_headers)
            resp = conn.getresponse()
            data = resp.read()
            if resp.status not in expect:
                if resp.status == 403:
                    raise DavError("DAV_QUOTA", "forbidden by server", 60)
                if resp.status in (409, 423, 507):
                    raise DavError("DAV_QUOTA", f"dav {resp.status}", 60)
                raise DavError("DAV_PENDING", f"dav {resp.status}", 60)
            resp.body = data  # type: ignore[attr-defined]
            return resp
        except (http.client.HTTPException, OSError, TimeoutError) as exc:
            raise DavError("DAV_PENDING", f"dav unreachable: {exc}", 60) from exc
        finally:
            conn.close()

    # -- operations ------------------------------------------------------
    def mkdirs(self, path: str) -> None:
        """Create every missing collection segment (405/301 tolerated as exists)."""
        parts = [p for p in path.strip("/").split("/") if p]
        current = ""
        for part in parts:
            current += "/" + part
            self._request("MKCOL", current, expect=(200, 201, 204, 405, 301))

    def put(self, path: str, body: bytes) -> None:
        self._request("PUT", path, body=body)

    def get(self, path: str) -> bytes:
        resp = self._request("GET", path, expect=(200, 204))
        return resp.body  # type: ignore[attr-defined]

    def exists(self, path: str) -> bool:
        try:
            self._request("PROPFIND", path, headers={"Depth": "0"}, expect=(207,))
            return True
        except DavError as err:
            if err.code == "DAV_QUOTA":
                raise
            return False

    def list_dir(self, path: str) -> list[str]:
        resp = self._request("PROPFIND", path, headers={"Depth": "1"}, expect=(207,))
        text = resp.body.decode("utf-8", "replace")  # type: ignore[attr-defined]
        # Minimal XML parsing of hrefs is enough for listing; server is trusted
        # for escape-validity here. Collect next-segment names.
        import re
        import html as _html
        hrefs = re.findall(r"<(?:[a-zA-Z0-9]+:)?href[^>]*>(.*?)</(?:[a-zA-Z0-9]+:)?href>",
                           text, re.S)
        names: list[str] = []
        for href in hrefs:
            href = _html.unescape(href.strip())
            seg = urllib.parse.urlsplit(href).path
            if self.base_path and seg.startswith(self.base_path):
                seg = seg[len(self.base_path):]
            seg = seg.strip("/")
            full = path.strip("/")
            if seg == full:
                continue
            name = seg[len(full):].lstrip("/")
            if name and "/" not in name:
                names.append(name)
        return names
