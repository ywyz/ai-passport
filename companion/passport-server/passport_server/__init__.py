"""Passport companion server (D1a, passport-travel-v1-draft).

Single-user private server SQLite | WebDAV backup and recovery.
Dependencies: Python 3.11+ standard library (sqlite3/hashlib/http.server/http.client).
Locks presentation in README (local test instance, do not deploy to production).
"""

from .db import Database, DB_SCHEMA_VERSION  # noqa: F401
from .errors import ApiError  # noqa: F401

CONTRACT_NAME = "passport-travel-v1-draft"
CONTRACT_VERSION = 1
