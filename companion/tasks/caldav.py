"""Same-origin TLS CalDAV transport and bounded discovery/REPORT queries."""

import base64
from http.client import HTTPException
import socket
import ssl
import time
from urllib.error import HTTPError, URLError
from urllib.parse import unquote, urljoin, urlsplit, urlunsplit
from urllib.request import HTTPRedirectHandler, HTTPSHandler, Request, build_opener
from xml.etree.ElementTree import Element, ParseError, SubElement, tostring

from defusedxml.ElementTree import fromstring
from defusedxml.common import DefusedXmlException

from .common import (MAX_COLLECTION_BYTES, MAX_COLLECTIONS, MAX_DAV_BYTES,
                     MAX_ITEMS, MAX_ORIGINAL_BYTES, TaskError, bounded, stable_id,
                     text_limit)
from .model import identity, parse_resource, sort_key

D = "{DAV:}"
C = "{urn:ietf:params:xml:ns:caldav}"


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise TaskError("REDIRECT_REJECTED")


class Transport:
    def __init__(self, base_url, username, password):
        self.base = base_url.rstrip("/") + "/"
        origin = urlsplit(self.base)
        if (any(ord(c) <= 32 for c in base_url) or origin.scheme != "https" or not origin.hostname or origin.username
                or origin.password or origin.query or origin.fragment):
            raise TaskError("INVALID_CONFIG")
        self.origin = (origin.scheme, origin.netloc)
        self.dav_path = origin.path + "remote.php/dav/"
        self.dav_url = urlunsplit((*self.origin, self.dav_path, "", ""))
        if not username or ":" in username or not password:
            raise TaskError("INVALID_CONFIG")
        self.authorization = "Basic " + base64.b64encode(
            (username + ":" + password).encode("utf-8")).decode("ascii")
        self.opener = build_opener(NoRedirect(), HTTPSHandler(context=ssl.create_default_context()))

    def resolve(self, href, relative_to=None):
        if not isinstance(href, str) or any(ord(c) < 32 for c in href) or "\\" in href:
            raise TaskError("UNSAFE_HREF")
        full = urlsplit(urljoin(relative_to or self.dav_url, href))
        decoded = unquote(full.path)
        if ((full.scheme, full.netloc) != self.origin or full.query or full.fragment
                or not full.path.startswith(self.dav_path)
                or not decoded.startswith(self.dav_path)
                or "\\" in decoded or "%" in decoded or any(ord(c) < 32 for c in decoded)
                or any(p in {".", ".."} for p in decoded.split("/"))):
            raise TaskError("UNSAFE_HREF")
        return urlunsplit(full)

    def request(self, method, href, body, depth):
        if method not in {"PROPFIND", "REPORT"}:
            raise TaskError("READ_ONLY")
        request = Request(self.resolve(href), data=body, method=method, headers={
            "Authorization": self.authorization, "Depth": str(depth),
            "Content-Type": "application/xml; charset=utf-8", "Accept": "application/xml",
            "Accept-Encoding": "identity", "User-Agent": "AI-Passport-Tasks/1",
        })
        try:
            with self.opener.open(request, timeout=15) as response:
                if response.status != 207:
                    raise TaskError("DAV_STATUS")
                if response.headers.get("Content-Encoding", "identity") != "identity":
                    raise TaskError("DAV_ENCODING")
                deadline = time.monotonic() + 60
                raw = bytearray()
                while True:
                    chunk = response.read1(min(65536, MAX_DAV_BYTES + 1 - len(raw)))
                    if time.monotonic() > deadline:
                        raise TaskError("UNAVAILABLE")
                    if not chunk:
                        return bytes(raw)
                    raw.extend(chunk)
                    if len(raw) > MAX_DAV_BYTES:
                        raise TaskError("LIMIT_EXCEEDED")
        except HTTPError as exc:
            exc.close()
            if exc.code in (401, 403):
                raise TaskError("AUTH_FAILED") from exc
            if 300 <= exc.code < 400:
                raise TaskError("REDIRECT_REJECTED") from exc
            raise TaskError("DAV_STATUS") from exc
        except (URLError, TimeoutError, socket.timeout, OSError, HTTPException) as exc:
            raise TaskError("UNAVAILABLE") from exc

    def get_resource(self, href, *, allow_missing=False):
        # Ordinary authenticated GETs can still be served stale by an edge
        # cache, even with no-cache headers. Read ETag and data together through
        # CalDAV REPORT before and after every conditional completion PUT.
        url=self.resolve(href)
        collection=url.rsplit('/',1)[0]+'/'
        query=Element(C+'calendar-multiget'); prop=SubElement(query,D+'prop')
        SubElement(prop,D+'getetag'); SubElement(prop,C+'calendar-data')
        SubElement(query,D+'href').text=urlsplit(url).path
        rows=responses(self.request('REPORT',collection,tostring(query,encoding='utf-8',xml_declaration=True),0),missing_resource=True)
        # This Nextcloud instance returns an empty multistatus for a new URI.
        # Only a creation probe may interpret that as an absence hint. The
        # subsequent If-None-Match:* PUT is authoritative; all post-write and
        # completion reads still require exactly one matching resource.
        if not rows and allow_missing:
            raise TaskError('NOT_FOUND')
        if len(rows)!=1 or self.resolve(rows[0][0],collection)!=url:
            raise TaskError('INVALID_DAV')
        props=rows[0][1]; data=props.get(C+'calendar-data'); tag=props.get(D+'getetag')
        if data is None or not data.text or tag is None or not tag.text:
            raise TaskError('INVALID_DAV')
        # XML normalizes literal CRLF. Restore canonical iCalendar line endings
        # so submitted hashes remain comparable after a lost PUT response.
        text=data.text.replace('\r\n','\n')
        if '\r' in text: raise TaskError('INVALID_CALENDAR')
        raw=text.replace('\n','\r\n').encode('utf-8')
        if len(raw)>256*1024: raise TaskError('LIMIT_EXCEEDED')
        return raw,tag.text

    def put_existing(self, href, body, etag):
        from .completion import strong_etag
        if not strong_etag(etag) or not isinstance(body, bytes) or len(body) > 256 * 1024:
            raise TaskError("INVALID_OPERATION")
        return self._put_calendar(href, body, etag)

    def put_new(self, href, body):
        if not isinstance(body, bytes) or not 0<len(body)<=4096:
            raise TaskError("INVALID_OPERATION")
        return self._put_calendar(href, body, None)

    def _put_calendar(self, href, body, etag):
        headers={"Authorization": self.authorization, "Accept": "text/calendar",
                 "Accept-Encoding": "identity", "User-Agent": "AI-Passport-Tasks/1"}
        headers.update({"If-Match": etag} if etag is not None else {"If-None-Match":"*"})
        headers["Content-Type"]="text/calendar; charset=utf-8"
        request=Request(self.resolve(href), data=body, method="PUT", headers=headers)
        try:
            with self.opener.open(request, timeout=15) as response:
                if response.status not in ({200,204} if etag is not None else {201}):
                    raise TaskError("DAV_STATUS")
                if response.headers.get("Content-Encoding", "identity") != "identity":
                    raise TaskError("DAV_ENCODING")
                raw=bytearray(); deadline=time.monotonic()+60
                limit=4096
                while True:
                    block=response.read1(min(16384, limit+1-len(raw)))
                    if time.monotonic()>deadline: raise TaskError("UNAVAILABLE")
                    if not block: break
                    raw.extend(block)
                    if len(raw)>limit: raise TaskError("LIMIT_EXCEEDED")
                return bytes(raw), response.headers.get("ETag", "")
        except HTTPError as error:
            error.close()
            code={401:"AUTH_FAILED",403:"AUTH_FAILED",404:"NOT_FOUND",412:"CONFLICT"}.get(error.code,"DAV_STATUS")
            if 300<=error.code<400: code="REDIRECT_REJECTED"
            raise TaskError(code) from None
        except (URLError, TimeoutError, socket.timeout, OSError, HTTPException):
            raise TaskError("UNAVAILABLE") from None


def propfind(*names):
    root = Element(D + "propfind")
    prop = SubElement(root, D + "prop")
    for name in names:
        SubElement(prop, name)
    return tostring(root, encoding="utf-8", xml_declaration=True)


def responses(raw, *, missing_resource=False):
    if len(raw) > MAX_DAV_BYTES:
        raise TaskError("LIMIT_EXCEEDED")
    try:
        root = fromstring(raw, forbid_dtd=True)
    except (ParseError, ValueError, DefusedXmlException) as exc:
        # XML parser details can contain private task data; report only a code.
        raise TaskError("INVALID_DAV") from exc
    if root.tag != D + "multistatus":
        raise TaskError("INVALID_DAV")
    result = []
    for response in root.findall(D + "response"):
        href = response.findtext(D + "href")
        if not href:
            raise TaskError("INVALID_DAV")
        status = response.findtext(D + "status")
        if status and not _success(status):
            if missing_resource and len(status.split())>=2 and status.split()[1]=='404':
                raise TaskError('NOT_FOUND')
            raise TaskError("DAV_PARTIAL_FAILURE")
        props = {}
        for ps in response.findall(D + "propstat"):
            if _success(ps.findtext(D + "status", "")):
                prop = ps.find(D + "prop")
                if prop is None:
                    raise TaskError("INVALID_DAV")
                for child in prop:
                    if child.tag in props:
                        raise TaskError("INVALID_DAV")
                    props[child.tag] = child
        result.append((href, props))
    return result


def _success(status):
    parts = status.split()
    return len(parts) >= 2 and parts[1] == "200"


class CalDAV:
    def __init__(self, transport, account_id, display_zone="Asia/Shanghai"):
        self.transport = transport
        self.account_id = account_id
        self.display_zone = display_zone

    def _properties(self, url, *names):
        rows = responses(self.transport.request("PROPFIND", url, propfind(*names), 0))
        if len(rows) != 1 or self.transport.resolve(rows[0][0], url) != url:
            raise TaskError("INVALID_DAV")
        return rows[0][1]

    def _link(self, props, tag, relative_to):
        element = props.get(tag)
        links = [] if element is None else element.findall(D + "href")
        if len(links) != 1 or not links[0].text:
            raise TaskError("INVALID_DAV")
        return self.transport.resolve(links[0].text, relative_to)

    def discover(self):
        dav = self.transport.dav_url
        props = self._properties(dav, D + "current-user-principal")
        principal = self._link(props, D + "current-user-principal", dav)
        props = self._properties(principal, C + "calendar-home-set")
        home = self._link(props, C + "calendar-home-set", principal)
        if not home.endswith("/"):
            raise TaskError("INVALID_DAV")
        rows = responses(self.transport.request("PROPFIND", home, propfind(
            D + "resourcetype", D + "displayname", C + "supported-calendar-component-set",
            D + "owner", D + "current-user-privilege-set"), 1))
        collections = []
        seen = set()
        for href, props in rows:
            types = props.get(D + "resourcetype")
            if types is None:
                raise TaskError("DAV_PARTIAL_FAILURE")
            if types.find(C + "calendar") is None:
                continue
            components = props.get(C + "supported-calendar-component-set")
            if components is None:
                raise TaskError("DAV_PARTIAL_FAILURE")
            if not any(c.get("name") == "VTODO" for c in components):
                continue
            url = self.transport.resolve(href, home)
            if not url.endswith("/") or not url.startswith(home) or url == home or url in seen:
                raise TaskError("INVALID_DAV")
            seen.add(url)
            name = props.get(D + "displayname")
            owner=props.get(D + "owner")
            owner_href=None if owner is None else owner.findtext(D + "href")
            privileges=props.get(D + "current-user-privilege-set")
            writable=bool(owner_href and self.transport.resolve(owner_href,home)==principal and privileges is not None
                          and any(privileges.find(".//"+D+key) is not None for key in ("write", "write-content", "all")))
            creatable=bool(owner_href and self.transport.resolve(owner_href,home)==principal and privileges is not None
                          and any(privileges.find(".//"+D+key) is not None for key in ("write", "bind", "all")))
            collections.append({"id": stable_id(self.account_id, url), "href": url,
                                "name": text_limit("" if name is None else name.text or "", 256),
                                "read_only": True, "completion_allowed": writable,"creation_allowed":creatable})
            if len(collections) > MAX_COLLECTIONS:
                raise TaskError("LIMIT_EXCEEDED")
        collections.sort(key=lambda c: (c["name"].casefold(), c["id"]))
        bounded(collections, MAX_COLLECTION_BYTES - MAX_COLLECTIONS * 68 - 256)
        return collections

    def fetch(self, collections):
        root = Element(C + "calendar-query")
        prop = SubElement(root, D + "prop")
        SubElement(prop, D + "getetag")
        SubElement(prop, C + "calendar-data")
        filt = SubElement(root, C + "filter")
        cal = SubElement(filt, C + "comp-filter", {"name": "VCALENDAR"})
        SubElement(cal, C + "comp-filter", {"name": "VTODO"})
        records, originals, seen, seen_urls = [], [], set(), set()
        original_bytes = 0
        for collection in collections:
            rows = responses(self.transport.request("REPORT", collection["href"],
                                                     tostring(root, encoding="utf-8"), 1))
            for href, props in rows:
                url = self.transport.resolve(href, collection["href"])
                if not url.startswith(collection["href"]) or url == collection["href"]:
                    raise TaskError("UNSAFE_HREF")
                if url in seen_urls:
                    raise TaskError("DUPLICATE_RESOURCE")
                seen_urls.add(url)
                etag = props.get(D + "getetag")
                data = props.get(C + "calendar-data")
                if etag is None or not etag.text or data is None or not data.text:
                    raise TaskError("DAV_PARTIAL_FAILURE")
                raw = data.text.encode("utf-8")
                original_bytes += len(raw)
                if original_bytes > MAX_ORIGINAL_BYTES:
                    raise TaskError("LIMIT_EXCEEDED")
                items = parse_resource(raw, account_id=self.account_id,
                                       collection_id=collection["id"], href=url,
                                       etag=etag.text, display_zone=self.display_zone)
                for item in items:
                    from .completion import strong_etag
                    item["completion_allowed"]=bool(collection.get("completion_allowed") and not item["recurring"]
                                                    and not item["has_parent"] and strong_etag(item["etag"]) and item["status"] in {"NEEDS-ACTION","IN-PROCESS"})
                    key = identity(item)
                    if key in seen:
                        raise TaskError("DUPLICATE_IDENTITY")
                    seen.add(key)
                    records.append(item)
                    if len(records) > MAX_ITEMS:
                        raise TaskError("LIMIT_EXCEEDED")
                originals.append((url, etag.text, raw))
        records.sort(key=sort_key)
        return records, originals
