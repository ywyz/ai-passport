**English** · [简体中文](README.zh_CN.md)

# Passport companion server (D1a)

Local single-user test server for the travel D1 contract `passport-travel-v1-draft` (docs/assets/passport-travel-contract.md).
Zero runtime dependencies beyond the Python 3.11+ standard library; SQLite is
the working database, WebDAV holds immutable resources and complete snapshots
only. A local test instance is allowed; production deployment is a separate
authorization and would put a TLS-terminating reverse proxy in front.

## Run a local test instance

```bash
cd companion/passport-server
python3 -m passport_server --data-dir ./passport-data --port 8642
# first run prints the admin password once; store it privately
```

Website: `http://127.0.0.1:8642/` (login → dashboard with pack publication,
device bindings, records/backup states, restore). Device API: `/api/v1/...`
as in the contract. A local WebDAV fake for tests only:
`passport_server.devdav` (protocol adapter `passport_server.webdav` works
against real servers too).

## Tests

```bash
cd companion/passport-server && python3 -m unittest discover -s tests
```

Also runs inside `./tools/validate.sh --static` as the companion gate.

## Pinned versions

| Dependency | Pin | Notes |
| --- | --- | --- |
| Python | ≥ 3.11 (3.14 verified) | stdlib only: sqlite3, http.server, http.client, hashlib |
| SQLite | the sqlite3 module's bundled version | WAL + `synchronous=FULL` |
| lv_font_conv | 1.5.3 | firmware font tool (Node ≥ 18), see `assets/fonts/README.md` |
| Noto Sans CJK SC | OFL 1.1 | font source, SHA-256 recorded |

## Security posture (D1)

- Single user; admin bootstrap once per database, controlled by the operator
  (no public unauthenticated bootstrap; post-restore bootstrap resets auth).
- Website sessions: HttpOnly cookie + CSRF token for every POST; password
  hashing PBKDF2-SHA-256 (100k iterations, per-database salt).
- Device tokens: delivered once at confirmation; the server stores digests
  only; revocation deletes any recovery material and rebinding is required.
- Binding codes are 6 chars of a 31-symbol alphabet, 5-minute TTL, 5 wrong
  attempts expire the session; wrong-code attempts rate limited per code.
- TLS: certificate/hostname/validity checks are never disabled. For local
  runs HTTP is for loopback testing; production device traffic goes through
  an HTTPS origin whose certificate chain/hostname is verified by the device.
- Passwords/binding codes/tokens never appear in logs, snapshots, tests or
  the repository.

## Restore behavior

Snapshots are validated in an isolated staging database first; an empty
server confirms and swaps in the restored database, which (1) creates a new
`server_epoch`, (2) marks stored devices revoked (rebind required), (3)
requires a fresh admin bootstrap, and (4) re-downloads referenced resources.
`complete.json` is the final marker; a snapshot without it is invisible to
restore. Live SQLite is never placed on a remote mount.

## Known D1 boundaries

- Real-WebDAV acceptance (A06 E-portion) needs an actual server; mock tests
  use the in-repo fake and remain separate evidence.
- OTA, multi-user, quotas dashboards, railway/AI adapters are later stages.
- OTP is a reserved boundary only (no codes, no real seeds).
