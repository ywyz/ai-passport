[简体中文](passport-nextcloud.zh_CN.md) · **English**

# Nextcloud tasks companion

This is the server-side implementation of the authorized task increment in
[suite-plan](passport-suite-plan.md#0-current-implementation-and-acceptance).
The implementation lives in `companion/tasks/`, uses Python 3.12 or newer, and
implements CalDAV discovery, explicit list selection, read jobs, and a cached
device API. The subsequent [device increment](passport-device-tasks.md) implements
the list/detail UI, snapshot download, persistent device cache and private USB
token configuration. The service is deployed behind the existing
provisioning website's HTTPS proxy. Current source adds opt-in conditional
completion and conflict replay. The user approved activation and the exact device
flash on 2026-10-20; operator completion, offline cache and normal-reboot replay
subsequently passed. The five-minute server refresh timer is deployed. Current
overall status is summarized in
[suite-plan section 0](passport-suite-plan.md#0-current-implementation-and-acceptance).

## Completion increment (history), 2026-10-20

The user deleted Deck. Source inspection now finds one owner-writable Tasks
collection with 150 source records, two unfinished tasks and no blank active
titles. The live read-only selection/snapshot was updated atomically to remove
the missing Deck choice. No Nextcloud records or credentials changed. The older
two-collection/154-record observations below are historical.

Optional private `completion_token` enables the write API and a background
worker in each WSGI process. It must be a distinct random 32–128-character
URL-safe secret, separate from read and admin tokens. Existing configuration
without it stays read-only. Generate/store it locally in restricted configuration,
never in CLI arguments or chat. The initial upgrade adds only this field.

POST admission persists the canonical intent and receipt in SQLite before
`SERVER_DURABLE`; repeated ID/hash returns the stored receipt. A 64-hex task ID
binds account/collection/UID/recurrence identity. Only selected, freshly verified
owner-writable collections and simple nonrecurring VTODOs with strong ETags are
eligible. Shared lists, subtasks and recurrence stay read-only. The worker checks
discovery/selection, reads the resource through CalDAV REPORT and PUTs with exact `If-Match`, updating
STATUS, PERCENT-COMPLETE, COMPLETED and necessary timestamps while preserving
other lines, folds and alarms. A final REPORT must confirm completion, 100 percent
and UTC completion time. It never creates, deletes or reopens a task.

Changed ETags/412 conflict without overwriting newer edits. Lost-response recovery
requires an exact hash match to the persisted submitted resource and valid
completed fields; ambiguity conflicts. Durable leases recover interrupted workers.
Transient failures retry at 2/5/15/60-second delays with jitter, five attempts
before `RETRY_PAUSED`; explicit retry resets only a paused budget. Authentication
failures halt work until private repair and `resume-auth`. The ledger retains at
most 1,024 receipts without automatic eviction and rejects overflow visibly.

Success updates the active view transactionally, retains completed history and
the whole-source fetch time, and preserves concurrent newer source refreshes.
Ordinary Refresh still downloads committed cache. Twelve synthetic completion tests
supplement 30 reader regressions, covering durability/scopes, property preservation,
conditional races, response loss, restart and bounded retries. Real SA-12 still
requires physical completion, offline reboot/replay and concurrent website edits.

The approved completion bundle `cfb330b4` is now active, and all five COM3 images
of firmware `d5779c7e` verified. Loopback/public HTTPS scopes passed without
admitting any completion intent; configuration preserved the existing credential
values, selected collection, proxy and provisioning files. The baseline ledger
was empty with two active tasks. Startup restored Wi-Fi/time/weather; physical
online completion, offline replay and conflicts remain pending operator feedback.
The [device record](passport-device-tasks.md#completion-candidate-validation-2026-10-20)
holds exact image/ELF identities and scoped observations. Historical read-only
deployment facts follow.

## Deployment facts and boundaries

The 2026-10-20 cached-GET repair supersedes the original completion-read path.
The operator reported other device functions working but completion showing
conflict. Two operation receipts addressed one task; fresh REPORT showed it
completed with 100 percent/UTC time and a new ETag, while GET still returned the
old unfinished resource. GET ignored request no-cache headers and reported Age
546–547 seconds and a 3,600-second edge cache lifetime. One submitted-resource
hash exactly matched the authoritative completed resource. The evidence identifies
a stale read after completion, rather than a failed first completion write.

Resource reads now use the standard
[calendar-multiget REPORT](https://www.rfc-editor.org/rfc/rfc4791#section-7.9)
with one exact href, ETag and full calendar-data. Missing/multiple/substituted
resources fail. XML-normalized line endings return to canonical CRLF before
editing/hashing, so response-loss recovery still requires an exact submitted
resource match. Strong ETags and conditional PUT protection remain unchanged;
GET is no longer used for completion checks. Fifteen completion tests plus 30
reader regressions cover stale-GET behavior, CRLF response-loss recovery and
multiget identity/missing-resource rejection. Live multiget reads passed without
changing Nextcloud. The device firmware, configuration and unresolved local
intents remain unchanged; actual post-repair completion/offline acceptance is
separate from these checks.

Read-only inspection on 2026-10-19 confirmed Nextcloud **35.0.0** (internal
version `35.0.0.10`), Tasks **0.18.1**, an AIO Docker deployment, and host Python
**3.12.3**. Public status and `occ` agree; maintenance mode and database upgrade
are false. The owner verified the SSH host fingerprint before connection.
The service host had about 6.9 GiB free on its root filesystem. The owner reports
Alibaba ESA in front of the service; authenticated PROPFIND/REPORT behavior
through that edge was checked by the local read described below. Private hostnames, account
names, credentials, and raw results stay outside tracked documentation.

After private local configuration and the owner's explicit selection of both
available lists, authenticated discovery and REPORT reads succeeded through the
public HTTPS endpoint. All returned tasks carried ETags; the cached API pagination
and database reopen checks passed. This is the server-read portion of SA-11 only,
not physical-device download acceptance. This dataset had UTC/absent due dates and
no recurring tasks or notes, so other date, recurrence and note behavior remains
fixture-tested. Sanitized counts, durations and resource measurements are retained
locally under `build/acceptance/20261019-nextcloud-reader/`.

The existing provisioning website host was first inspected read-only: Caddy
2.11.7 and Python 3.12.3 are available, with roughly 722 MiB available RAM and
6.1 GiB free disk at inspection. On 2026-10-19, the owner explicitly approved
deploying the service there, transferring the private app-password/token config,
creating a dedicated account and adding `/suite/v1/` to the existing HTTPS origin.
The deployed source/dependency bundle has SHA-256
`0c8bdec141bf1d0ab65bee0e636baba7b5c4d76b5760cc506615101d48f73bf5`.
The installer verified every packaged file and the unchanged base Caddyfile,
checked the loopback API, validated the proposed Caddy configuration and reloaded
it. Nextcloud remains a remote CalDAV source; its AIO containers were not changed.

The server read both selected lists successfully in 3.628 seconds: 154 tasks,
all with ETags. External HTTPS checks passed for authenticated reads, five-page
snapshot retrieval (largest page 32,252 bytes), `no-store` responses, rejection of
unauthenticated reads (401) and rejection of selection writes with the device
token (403). The manual refresh service completed with exit status 0. The API
is active and enabled at boot; its measured service memory after refresh was
36,937,728 bytes, below the 192 MiB cap. This is an observation, not a load-test peak.
The three existing provisioning files remained byte-identical, and their public
HTTPS responses match the current source. No firmware was flashed.

Reviewable deployment files are in [`companion/deploy/`](../../companion/deploy/):

- `gunicorn.conf.py` uses Gunicorn 26.2.0, one worker/two threads, loopback port
  8787, request limits, and disabled access logging. The Linux host test exercises
  its real HTTP startup, authenticated response and unauthenticated rejection.
- `passport-tasks.service` uses a dedicated `passport-tasks` account, code and
  hash-verified pure-Python dependencies under `/opt/passport-tasks/`, private
  config at `/etc/passport-tasks/config.json`, and SQLite under
  `/var/lib/passport-tasks/`. Its memory cap is 192 MiB. Packaging dependencies
  in `vendor/` avoids changing system Python or requiring ensurepip on the host.
- `passport-tasks-refresh.timer` schedules the same oneshot
  `passport-tasks-refresh.service` five minutes after boot and five minutes after
  each completed job; the service remains available for an explicit
  `systemctl start`. It uses the same private config/cache and a 30-minute job
  deadline. The timer was deployed and enabled on 2026-10-22; observed results are
  recorded below.
- `caddy-route.example` adds only the API route with a 4 KiB request-body limit;
  existing static provisioning files remain unchanged. The actual current
  Caddyfile hash must match the reviewed base before applying the local proposal.

Private configuration was transferred separately; the source/dependency bundle
contains no credentials or task data. Config and SQLite files are accessible only
to the service account and administrators. The deployment retained a private
Caddy backup; rollback restores that reviewed base only if the live configuration
still matches the deployed proposal, then stops the new service. It must not
overwrite later unrelated proxy changes. Future deployments still need their own
authorized scope. [Gunicorn's deployment guidance](https://gunicorn.org/deploy/)
describes the proxy boundary used here.

An authorized operator can refresh the server snapshot on demand with
`sudo systemctl start passport-tasks-refresh.service` and inspect its result with
`systemctl show passport-tasks-refresh.service -p Result -p ExecMainStatus`.
This reads Nextcloud without modifying tasks. The periodic timer schedules the
same oneshot service five minutes after boot and again five minutes after each
completed job, so a slow job delays rather than overlaps the next run. A failed or
authentication-halted read preserves the last committed cache and does not promise
five-minute freshness. The API continues to serve the committed snapshot and marks
it stale after 24 hours; the device still downloads that snapshot only when the
operator presses Refresh. Repair credentials privately and run the CLI's
`resume-auth` as the service account before retrying an authentication failure.

Once the timer unit is installed, enable and inspect it with:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now passport-tasks-refresh.timer
systemctl list-timers passport-tasks-refresh.timer
systemctl show passport-tasks-refresh.timer -p NextElapseUSecMonotonic
systemctl show passport-tasks-refresh.service -p Result -p ExecMainStatus
```

Use the service unit, not a concurrent direct CLI `refresh`, for production manual
runs so systemd serializes them with the timer. Minimal rollback disables the timer
and leaves configuration, database, API and data unchanged:

```bash
sudo systemctl disable --now passport-tasks-refresh.timer
sudo systemctl daemon-reload
```

Removing the unit file is optional; disabling it is sufficient to return to manual
operation. The timer was deployed and enabled on 2026-10-22; these commands remain
the enable/inspection/rollback runbook. Observed acceptance is recorded below.

## Read and cache behavior

On 2026-10-20 the operator reported 154 device items, including completed history.
The source snapshot contained 148 `COMPLETED` and six `NEEDS-ACTION` records.
The active-only view correction passed the complete repository gate (30 companion
tests on Linux) and was applied to the existing service's `store.py` only, with
precondition hashes and a rollback copy. The deployed file SHA-256 is
`742719788b71a78a0f8e19b56c0b2837cd91ce3fdf7a6ee8608665f7251401bd`;
this is an overlay on the original source bundle above. Loopback and external
verified HTTPS returned six active records in one 5,241-byte `READY` page.
Old all-task cursors were rejected, and 401/403 scope checks passed. Hashes of the
database, private configuration, Caddyfile and provisioning assets were unchanged.
The filtered real payload also passed the device parser/cache host replay.
No device firmware or token configuration was written; physical Refresh,
detail and offline acceptance of the corrected view remains pending.

- The device snapshot includes only `NEEDS-ACTION` and `IN-PROCESS` tasks.
  `COMPLETED` and `CANCELLED` history remains in SQLite and the original calendars;
  filtering performs no CalDAV writes. Filtering precedes totals and pagination,
  and an all-terminal snapshot returns `EMPTY`. The active-view revision differs
  from the stored source revision so old all-task cursors cannot mix with new pages.
  Existing cached snapshots are filtered immediately, without another CalDAV read.
- Discovery follows `current-user-principal`, `calendar-home-set`, and VTODO
  collections. List IDs bind the account and collection URL; names are not IDs.
- `calendar-query REPORT` retrieves complete VCALENDAR resources with ETags.
  The adapter preserves returned calendar text, including unknown properties,
  alarms, and recurrence relationships. XML transport normalizes line endings;
  this is not a byte-for-byte copy of a separately downloaded `.ics` file.
- The identity includes account, collection, UID, and recurrence ID. Explicit
  recurring instances and subtasks are labeled read-only; there is no generated
  recurrence expansion or claim of full recurring-task interoperability.
- Date-only, UTC, TZID/VTIMEZONE, floating, and absent due dates remain distinct.
  Zoned times also have UTC and configured display values. Floating values retain
  their lack of timezone. Unresolved zones or malformed calendars fail the refresh
  rather than silently inventing a time or dropping an item.
- The adapter rejects cross-origin/out-of-DAV hrefs and all redirects. TLS peer
  verification remains enabled. There is a 15-second connect/read timeout and a
  60-second response-body budget; DNS and an in-flight socket call can add latency.
  XML DTD/entity expansion is forbidden. Error responses expose fixed codes only.
- Limits are 20 collections/8 KiB, 200 tasks/1 MiB, 8 KiB per record, 32 KiB per
  API page, 512 UTF-8 bytes per summary, and 4,096 bytes per note preview. Notes
  report truncation and original byte count. A CalDAV response is at most 4 MiB;
  one calendar resource is at most 256 KiB, retained originals total at most
  8 MiB per snapshot. Overflows fail visibly; they are not accepted partial lists.
- SQLite commits the new snapshot and original resources together. Partial reads,
  duplicate identities, missing selected collections, overflow, or storage failure
  retain the old committed snapshot. Selection changes use a revision check; an
  in-flight refresh cannot publish against a newer selection. Old-selection data
  is not returned as the new selection's snapshot.
- Cached data becomes STALE after 24 hours or a failed refresh. Authentication
  failure preserves the cache and halts remote retries until explicit operator
  recovery. Pages carry a snapshot revision; a concurrent replacement returns
  `REVISION_CONFLICT` instead of mixing pages from two snapshots.

The service's `downloaded_at` denotes its completed CalDAV fetch, not a receipt
that the physical device has downloaded or activated that snapshot.

The [SC-06 contract](passport-suite-contract.md#sc-06-nextcloud-caldav-and-offline-completion)
remains the product target. The periodic server read timer was deployed and
enabled on 2026-10-22. The implementation still does not provide a browser
management screen, a device download receipt, owner login/recovery, multi-account
hosting, backup, or encrypted storage. Private local files and distinct read/admin
bearer tokens are a development service boundary, not acceptance of the proposed
SC-02/SC-03 binding system.

## Local setup and use

Create a dedicated Nextcloud app password in personal security settings. Enter
it locally; do not put it in chat, shell arguments, tracked files, or logs. The
Nextcloud password stays on the companion side. Never send it or a server bearer
token through the current ordinary BLE settings protocol. A dedicated password
can still access more than tasks in Nextcloud; the adapter's method limits
are not a server-side credential permission restriction.

Use a private directory outside tracked sources, for example `build/nextcloud/private/`.
On POSIX, use mode 700 for the directory and 600 for configuration/database files;
on Windows restrict its ACL to the owner and system administrators. Configuration
does not overwrite an existing file. `configure` hides both login name and password
and generates distinct random device/admin tokens without printing them.

```bash
python3 -m venv build/nextcloud/venv
source build/nextcloud/venv/bin/activate
python -m pip install --require-hashes --only-binary=:all: -r companion/requirements.lock
python -m companion.tasks --config build/nextcloud/private/config.json configure
```

The examples below abbreviate the common arguments as `$task_args` for clarity;
use an array so paths remain separate arguments:

```bash
task_args=(--config build/nextcloud/private/config.json --database build/nextcloud/private/tasks.sqlite3)
python -m companion.tasks "${task_args[@]}" discover
python -m companion.tasks "${task_args[@]}" collections
python -m companion.tasks "${task_args[@]}" select --revision 0 <collection-id>
python -m companion.tasks "${task_args[@]}" refresh
python -m companion.tasks "${task_args[@]}" status
python -m companion.tasks "${task_args[@]}" preview --port 8787
```

`collections` intentionally displays private list names locally; do not share that
output. `discover`, `refresh`, and `status` print counts/revisions/status only.
`select` accepts zero IDs for an explicitly empty selection and requires the
current selection revision. `refresh` is a bounded read job invoked by the timer or
manually; the preview server only serves committed cached data. After privately repairing
an app password, run `resume-auth` before retrying. Revoke an abandoned app password
in Nextcloud; deleting the local config does not revoke it.

`preview` binds only to `127.0.0.1` and uses a development WSGI server. It is not a
production internet server. The WSGI `API` callable provides:

| Method and path | Authority | Result |
| --- | --- | --- |
| GET `/suite/v1/task-collections` | Device or admin token | Collections, selection and revision |
| PUT `/suite/v1/task-selection` | Admin token | `{collection_ids, revision}` compare-and-swap |
| GET `/suite/v1/tasks/snapshot` | Device or admin token | Bounded cached page; later pages require `revision` and `offset` |
| POST `/suite/v1/task-completions` | Completion token only | Durable intent/receipt; optional explicit `retry` |
| GET `/suite/v1/operations/{operation_id}` | Completion token only | Stored operation receipt |

All use `Authorization: Bearer ...`; no token in a query string, cookie, or access
log. Responses are `Cache-Control: no-store`. Read/admin tokens cannot complete;
the completion token cannot read snapshots or change selection. Snapshot
`read_only` describes the read API; per-item `completion_allowed` advertises
eligibility for the separate write capability.

## Historical reader validation and handoff

The following server checkpoint and device proposals describe the preceding
increment. Current device implementation and validation boundaries are in the
[device reader record](passport-device-tasks.md).

`bash tools/test-companion.sh` runs synthetic host tests with hash-pinned,
isolated Python dependencies under ignored `build/tooling/companion/`. The
complete repository gate also invokes it. The dependency cache is keyed by
Python version and lockfile hash; no ESP-IDF/system packages are replaced.
An offline wheel directory may be supplied with `PIP_NO_INDEX=1` and
`PIP_FIND_LINKS`; dependency hashes are still mandatory.

Calendar parsing uses [icalendar 7.3.0](https://icalendar.readthedocs.io/en/stable/)
and XML parsing uses [defusedxml 0.7.1](https://github.com/tiran/defusedxml).
Transitive packages and wheel SHA-256 values are pinned in
[`companion/requirements.lock`](../../companion/requirements.lock).

Tests cover discovery, VTODO parsing, timezone/date forms, note truncation,
unsafe links/redirects/entities, response and item limits, missing properties,
duplicate identities, offline/stale/auth states, restart, transactional rollback,
selection races, revision-bound pagination, and device/admin API scopes. These
fixtures do not satisfy actual Nextcloud SA-11 or device acceptance. SQLite
rollback/reopen tests are not a physical power-cut test.

Validation checkpoint, 2026-10-19:

- **Build: PASS** — complete `./tools/validate.sh` under ESP-IDF 5.5.3,
  followed by independent archive verification. Firmware version `21c5110-dirty`,
  merged image SHA-256 `8285859fa1c230379312b685a542f47ebc323b09d988f008538cf5ce82a84097`,
  matching ELF SHA-256 `a311c62c08cea2ac72f472439d76ea8fa776f46683a0b7b9f19b7d6e8ef16354`.
  The archive is `build/firmware/<merged-image-sha256>/`; the image was not flashed.
- **Host tests: PASS** — repository/static regression gate and the latest
  27 companion tests, including Linux Gunicorn HTTP startup. The sealed service
  bundle matches the current first-party source files. Final documentation checks
  also passed. Real server/HTTPS evidence is recorded separately above.
- **Device tests: NOT RUN** for Nextcloud — the previously accepted scene build
  remains on the device; its acceptance does not transfer to this feature.
- **Unverified:** device UI, protected token setup, persistent cache/power cuts,
  dynamic font rendering and integrated RAM/Flash margins; actual recurring,
  shared, subtask and floating-date compatibility; completion/conflict replay;
  server reboot/load/rollback and backup recovery. Automatic refresh and device
  refresh controls are not implemented.

The earlier scene/acceptance commit is pushed. This Nextcloud increment remains
uncommitted; private configuration, logs, task data and generated artifacts stay
outside tracked sources.

Private account setup, explicit list selection, real reads through ESA and the
approved server deployment are complete. Evidence separates synthetic tests,
local real reads and deployed HTTPS checks; none proves physical-device download.
Before device integration, resolve the flash
cache layout and power-safe update strategy: the current 24 KiB NVS must not be
used as a 1 MiB task store. Budget TLS, task-page decoding, and dynamic Chinese
glyphs against measured internal RAM. The real snapshot exceeds NVS capacity, and
its titles require characters absent from the current declared UI charset. No
private task text or derived character list was exported to source/font assets.
Implement a protected device-token setup
path before using private task data on the board. Any new firmware needs its own
complete gate, verified archive, and exact-image flash approval.

The next device increment has the following implementation order:

| Work | Current evidence and required result |
| --- | --- |
| Storage and fonts (Q-05) | The measured JSON snapshot is 148,940 bytes; current generated font coverage misses 275 distinct characters in this dataset. Combining it with the bundled CJK font still misses 138. Choose a licensed public character inventory, measure the generated font/image, and define an application-specific cache partition and power-safe commit procedure before changing partitions. Keep Wi-Fi/city NVS offsets intact. These counts do not certify arbitrary-title coverage. |
| Device authorization (Q-02/03/04/11) | The deployed device bearer token has only read access but is still secret. Implement and review a protected setup path and TLS/time checks before transferring it; do not put it into firmware, the QR, ordinary BLE settings, or source. Owner login/recovery remains outside this deployment. |
| Download and persistence | Add a background synchronization owner with bounded page parsing, revision checks, cancellation and late-result suppression. Only a fully verified, committed cache may become active; display device-download time separately from server-fetch time. Exercise interrupted writes with synthetic data before real-data/device checks. |
| UI and acceptance | Replace only the task entry's placeholder in `main/suite_launcher.c`, with list/detail/refresh views, visible read-only and offline/stale/auth states, and bounded scrolling. Verify dynamic Chinese rendering, heap/stack margins, preserved Wi-Fi/city settings and restart cache recovery on the exact approved firmware. Completion remains a separate increment. |

The current `main/suite_storage.c` only handles small NVS settings; its existing
initialization may erase NVS when initialization reports a layout/version failure.
A new task-cache backend must not use that recovery path to erase settings after
a task-cache failure. No such firmware or storage change has been made here.

A local font feasibility check used Noto Sans SC Regular 2.004 (source SHA-256
`faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9`),
[lv_font_conv 1.5.3](https://github.com/lvgl/lv_font_conv), 16 px, uncompressed
bitmaps and fast class kerning. Its public requested ranges were U+0020–007E,
U+2000–206F, U+3000–303F, U+4E00–9FFF and U+FF00–FFEF. Both candidates generated
21,393 code points; 110 requested code points are absent from the source font,
so this is not a claim of full coverage of those ranges. Both cover this private
dataset, but no private text or derived character inventory was exported.

| Candidate | RISC-V object read-only bytes | Bitmap bytes | Writable static bytes |
| --- | --- | --- | --- |
| 4-bit grayscale | 2,920,353 | 2,531,072 | 0 |
| 2-bit grayscale | 1,656,220 | 1,266,939 | 0 |

Both standalone objects compiled against the gate's IDF 5.5.3/LVGL headers with
`CONFIG_LV_FONT_FMT_TXT_LARGE=1`; their bitmaps exceed the default 20-bit index
range. These measurements exclude linked-image alignment, changes to other font
descriptors and runtime drawing/cache allocations. The 2-bit candidate fits the
proposed 2 MiB font ceiling, subject to integrated resource and visual acceptance.
Candidate sources/objects remain in ignored build output, not the firmware.
Before distributing a derived font, include its copyright notice and the
[upstream OFL 1.1 license](https://github.com/notofonts/noto-cjk/blob/main/Sans/LICENSE).

## MiMo voice-task candidate, 2026-10-21

The user selected MiMo pay-as-you-go for SU-17. Code follows
[SC-06.1](passport-suite-contract.md#sc-061-voice-created-tasks); it has not been
deployed, and no real provider call or new task has been made by the tools.
The existing completion token gains only the explicit voice/creation routes;
device read and admin-selection scopes remain separate. One selected owned
collection with DAV bind/write/all is required, even when its active task count
is zero. Run the source refresh job after a future deployment to discover the
new privilege flag; completion-only write-content does not grant creation.

Use `python3 tools/configure-task-speech.py --output <private-path>` in a private
terminal to enter the ordinary pay-as-you-go API key without echo. Do not put the
key in a chat, command-line argument, tracked file, image or device configuration.
Default `daily_seconds` is 300 (configurable 1..3600). On the server install the
result as `/etc/passport-tasks/speech.json`, mode 0600, readable by the service
account. Add `SUITE_TASKS_SPEECH_CONFIG=/etc/passport-tasks/speech.json` through a
systemd override after exact-candidate approval. No new Python dependency is
required. Missing/invalid optional speech configuration leaves existing Tasks
available and reports voice unconfigured. Configure one Gunicorn process, two
HTTP threads, as in the existing service; the recognition lock is process-local.

The precise transcription route in `companion/deploy/caddy-route.example`
accepts 384,044 bytes; merge it without replacing existing provisioning routes
or increasing other request limits. Recognition does not save an audio file.
The SQLite usage ledger charges before the external call, with no automatic
retry/refund after uncertain results. New durable creation receipts are bounded
to 1,024; full storage rejects admission. Production deployment, authentic ASR,
microphone/heap/stack and on-device creation/replay remain acceptance gates.

## Empty creation probe repair, 2026-10-21

The operator confirmed the recognized title and pending creation survived normal
reboot. Reconnection nevertheless ended in paused/retry/pending states. The
server had one durable creation receipt, five attempts, RETRY_PAUSED and
INVALID_DAV; the target was still selected/owned/creatable, and authentication
was not halted. Read-only diagnosis found an empty DAV multistatus for the new
resource URI. The transport required one response and rejected it before the
conditional create. Earlier synthetic resources returned NOT_FOUND directly
and did not exercise this real transport behavior.

The candidate permits a validated empty multistatus only as a creation-before-PUT
absence hint. If-None-Match:* remains the authoritative protection; post-write
and completion reads remain strict and require one exact resource. Malformed,
foreign and multiple-resource reports still fail. New regression cases cover
the real transport, the successful create/readback, lost response and a racing
existing resource; the actual read-only endpoint probe confirms the difference.
No new intent or provider call is needed. Existing selected-list, credentials,
speech key/budget, cache and device firmware remain unchanged. The complete gate passed (61 companion cases, zero skips), and the two-file
backend correction is deployed. HTTPS health and token scopes passed; the
original paused intent, all three completion receipts, settings, keys and five
charged audio seconds are unchanged. The installed voice firmware remains
10f8cb21/d7654fa9 (full-image/ELF prefixes); the additional complete-gate image
4e5beb17/73a04243 is archived but not flashed. The subsequent device retry and independent single-resource audit passed;
see the acceptance record below.

## Voice creation acceptance, 2026-10-21

After the backend-only correction, the operator retried the original confirmed
request once. The device displayed creation success and the website showed the
new task. An independent read-only PROPFIND/REPORT audit found exactly one
matching resource and one SUCCEEDED creation receipt; UID, destination and title
match the durable confirmed intent, and the task is NEEDS-ACTION. The verified
HTTPS snapshot now contains one active task. All three earlier completion
receipts remain; service-admission audio usage remains five seconds, with no
additional recognition call for this replay. The audit did not mutate Nextcloud.

Build: PASS. Host tests: PASS (complete gate, including 61 companion cases and
zero skips). Device tests: PASS for real microphone/MiMo recognition, Chinese
preview/default Cancel, post-recognition offline save, normal reboot retention,
and same-intent online retry ending in a single actual task. Wi-Fi/city settings,
credentials, keys, cache and completion journals were preserved. Installed
firmware remains 10f8cb21/d7654fa9; no additional flash was required.

Unverified: measured audio stop latency and runtime peak heap/stack, mixed
voice/BLE/weather operation, long-duration behavior, physical power cuts and
genuine concurrent website edits. Speech recognition requires connectivity;
the offline acceptance covers submission after recognition. The success page
can now be acknowledged with OK to continue normal use.

## Source freshness diagnosis, 2026-10-22

After the reliability firmware flash, the operator confirmed that original tasks
were still visible but initially saw the refresh-failed/keep-cache message. That
first failure was not captured, so its cause remains unverified. A captured
retry returned `Refresh result=2, cached=1` with no error logs: device download
and cache activation succeeded, but this does not prove source freshness.

The committed server snapshot was 80,792 seconds old: 151 source records and
one active task, one selected collection, selection revision 2, no authentication
halt or recorded source error. The existing one-time `passport-tasks-refresh.service`
then completed successfully, publishing 160 source records and six active tasks
without changing the selection or writing Nextcloud tasks. An independent
read-token API check returned HTTP 200, `READY`, six items in a 6,200-byte page,
no pagination, and `Cache-Control: no-store`. The operator subsequently confirmed
that the six latest tasks display correctly on the device; the later passive
window captured no new refresh.

OpenCode Go `deepseek-v4.1-flash`, default variant, reviewed the source-only
scheduling choices in session `ses_e9cb86cbaffeukdNjpG6M7gxNb`. Codex verified the
model metadata, local implementation and live count-only evidence. The operator
selected the five-minute server timer targeting the existing oneshot job; a
device-triggered source read would require a separate API/security design. The
timer unit is now deployed and enabled as recorded below. Inactive-
based scheduling is measured after job completion; it is not a freshness guarantee
when reads fail. Keep production manual starts on the same service unit to avoid
parallel direct CLI reads.

The installed `7c292cfc/e8b6ae41` firmware is unchanged; no further flash,
configuration/token rewrite, application-source replacement or Nextcloud task write occurred.
Local sanitized summaries and private bounded serial logs are under
`build/acceptance/20261022-task-refresh/`.

Build: PASS for the unchanged verified firmware. Host tests: PASS from its
complete gate; this incident made no source change. Device tests: PASS for
retained visible cache, the captured one-task download and the operator-confirmed
six-task display; the earlier startup/flash subset remains separately recorded.
Unverified: initial refresh-failure cause, recording/preview/Cancel, error-label
rendering, measured latency/memory peaks, power cuts and integrated operation.

### Five-minute timer deployment, 2026-10-22

The user-selected timer was implemented by OpenCode Go `deepseek-v4.1-flash`,
default variant, in session `ses_e9cb86cbaffeukdNjpG6M7gxNb`, then independently
reviewed and deployed by Codex. The service changed only its Description; its
oneshot command, account, paths, hardening and 30-minute timeout remain unchanged.
The deployed timer SHA-256 is
`30895929af695494450e066bacd31bf9f6d2f9d6efc3e8fadbb5a5e254a0569d`;
the service SHA-256 is
`3dd65f5ea8862e2a1a133b1f5700f21202b1db1d1d770ba37241870994066f24`.
The previous service was backed up before replacement. Only those two systemd
unit files were installed; API/Caddy/application code and database schema were
not replaced.

Effective configuration and the boot-enable symlink verified: `OnBootSec=5min`,
`OnUnitInactiveSec=5min`, `AccuracySec=1s`, targeting the existing oneshot service.
Two automatic reads exited successfully, with 304 seconds between snapshot
publications; the timer's trigger and service-start timestamps advanced and the
next run remains scheduled. The latest checked API returned HTTP 200 `READY`
with seven active tasks. The operator's earlier six-task device display is
confirmed separately; the server count is not a new device display receipt.

During the first live read, 17 API requests all returned 200, maximum observed
latency 220.44 ms; the API process did not restart. Configuration/token bytes,
permissions, selected collection/revision, authentication state, completion and
creation receipts, and speech usage all matched their pre-deployment hashes
through both observed runs. The reads did not modify Nextcloud tasks.

An isolated transient-unit fixture on the same server failed its first run, then
automatically retried. Later four-second jobs at a two-second inactive interval
did not overlap; repeated manual starts of the same active unit coalesced.
The fixture was stopped and did not call the production reader. Existing host
regressions cover source failure/rollback, authentication halt and cache retention;
no production credentials or source failures were injected.

The complete `./tools/validate.sh` passed, including 64 companion cases with zero
skips, the runtime suite and firmware layout/archive validation. Independent unit
verification passed locally and on the server. The extra gate build is validation
only: full-image SHA-256 `21c12bfb12befb43f52274617ed5c60feae573f5c237f2d711217daef45a9a90`, matching ELF SHA-256
`f90c023d8da41412f9c1049fb9e4b4832b8dd4fcd427fd9a7e83a9eb9aa50228`, archived under
`build/firmware/21c12bfb12befb43f52274617ed5c60feae573f5c237f2d711217daef45a9a90/`. It was not flashed; the installed
firmware remains `7c292cfc/e8b6ae41`. Evidence and the reviewed rollback procedure
are retained locally under `build/acceptance/20261022-refresh-timer/`.

Build: PASS. Host tests: PASS.
Server synchronization: PASS for two automatic reads, effective configuration,
API availability, preserved state and the isolated scheduling fixture.
Device tests: PASS for the operator-confirmed six-task download/display on the
unchanged installed firmware; no new flash was needed.
Unverified: the initial sporadic device refresh failure, recording/preview/Cancel,
error-label rendering, measured device latency/memory peaks, power cuts and mixed
operation. A real server reboot or forced 30-minute production timeout was not run.
