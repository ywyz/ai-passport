"""Stable API error codes per contract section 6.

Initial codes: AUTH_REQUIRED, BINDING_EXPIRED, REVISION_CONFLICT,
SCHEMA_UNSUPPORTED, SIZE_LIMIT, NO_SPACE, HASH_MISMATCH, GLYPH_MISSING,
PROVIDER_UNAVAILABLE, DAV_PENDING, DAV_QUOTA.  Never include passwords,
binding codes or tokens in messages.
"""
from __future__ import annotations

HTTP_STATUS = {
    "AUTH_REQUIRED": 401,
    "BINDING_EXPIRED": 410,
    "REVISION_CONFLICT": 409,
    "SCHEMA_UNSUPPORTED": 400,
    "SIZE_LIMIT": 413,
    "NO_SPACE": 507,
    "HASH_MISMATCH": 400,
    "GLYPH_MISSING": 400,
    "PROVIDER_UNAVAILABLE": 404,
    "DAV_PENDING": 503,
    "DAV_QUOTA": 507,
    "NOT_FOUND": 404,
    "BAD_REQUEST": 400,
    "RATE_LIMITED": 429,
    "FORBIDDEN": 403,
}


class ApiError(Exception):
    def __init__(self, code: str, message: str, retry_after_seconds: int | None = None) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.retry_after_seconds = retry_after_seconds
        self.status = HTTP_STATUS.get(code, 400)

    def to_body(self, request_id: str) -> dict:
        body = {"code": self.code, "user_message": self.message, "request_id": request_id}
        if self.retry_after_seconds is not None:
            body["retry_after_seconds"] = self.retry_after_seconds
        return body
