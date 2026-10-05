[简体中文](passport-travel-contract.zh_CN.md) · **English**

# AI Passport First Travel Data and Synchronization Contract

> **Superseded (2026-10-05):** Historical reference only, no longer implementation or acceptance authority. The [integrated firmware plan](passport-suite-plan.md) retains translation, OTP, the merged wallet, Nextcloud tasks and the three-body clock/weather; other features are cancelled. Do not execute old stages, phone/SoftAP provisioning proposals or implementation prompts directly.

Updated: 2026-10-03. Status: `passport-travel-v1-draft`, D0 review draft, neither frozen nor implemented. See the [first-batch PRD](passport-travel-prd.md) and [overall plan](passport-suite-plan.md). APIs, fields, directories and limits below are proposed application contracts rather than existing BSP/server capabilities.

## 1 Common data rules

Use UTF-8 JSON with integer `schema_version: 1`. IDs are case-sensitive ASCII strings of at most 64 bytes. Device, account, place, itinerary, revision and event namespaces are separate. Names are not identities. Server-generated revisions are positive integers; devices never increment server revisions. ISO 8601 timestamps carry timezones. If sources omit a date, the server resolves it or marks it missing; devices must not guess midnight rollover.

Reject incompatible required versions. Ignore unknown optional fields only where explicitly permitted, and reject unknown event operations/enums. Missing values use `null` and optional `missing_reason`, not zero/empty strings masquerading as numbers. Bound arrays, text and responses. Convert imported HTML/Markdown to supported plain text; imported content never becomes executable code or tool instructions.

Validate references at publication. Read paginated/streamed data rather than all national places into RAM. User content is private; fixtures explicitly set `fixture: true`. Only the owning account accesses events/content. ID changes require mappings or history. Deletions use tombstones so older backup recovery cannot silently resurrect deleted records.

## 2 Object dictionary

| Object | Required fields | Optional fields and constraints |
| --- | --- | --- |
| itinerary | id, revision, title, timezone, start_date, end_date, entry_ids | `fixture`; dates ordered |
| rail_ticket | id, revision, itinerary_id, travel_date, boarding_station, arrival_station, train_number, review_state, provenance | carriage, seat, departure_at, stops, train_model may be missing; boarding is distinct from origin |
| rail_stop | station_id, name, sequence, arrival_at, departure_at, time_kind | Origin arrival/terminus departure may be `null`; complete dates, nonnegative dwell |
| station_departures | station_id, service_date, data_kind, fetched_at, expires_at, source_id, items | `data_kind`: scheduled or live_board; platform/gate/delay/boarding_status only with actual sources |
| guide_route | id, revision, itinerary_id, review_state, ordered_stop_ids | Published revisions immutable with narration/media references |
| guide_stop | id, place_id, title, narration_text_ref, practical_note_ref | audio_ref may be absent per object, but the stage requires offline-audio acceptance; sourced opening times |
| place | id, province_id, parent_id, kind, name, initials | kind: province, city, county, attraction; city ancestry/full path, reviewed initials |
| province_pack | id, province_id, data_revision, coverage, place_index_ref, glyph_inventory_ref | stamp_refs, selected_refs; coverage does not imply complete national data |
| record | id, kind, context_id, target_id, revision, state, updated_by | kind: visit, wish, guide_progress; visited/wish independent of packs |
| change_event | event_id, device_id, device_epoch, sequence, record_id, context_id, target_id, operation, value, base_revision | device_time, time_trust, predecessor_event_id; unknown time may be `null` |
| backup_snapshot | snapshot_id, schema_version, created_at, server_epoch, included_cursor, files | Sizes/SHA-256, only complete snapshots recoverable; future encrypted OTP reference |

`review_state` is draft, reviewed or published. Field provenance includes type user_ticket, user_confirmed, provider or fixture, source_id, fetched time and field review state. Fixtures never prove live access. Object-wide review state cannot hide unreviewed important fields.

`time_trust` is unknown, user_set or network_synced; time trust does not replace TLS authentication. Province IDs such as `CN-JS` require the complete resident 34-ID/name list to be checked during implementation; merely asserting a count of 34 is insufficient. County/attraction names must not be hashed into permanent identities.

The logical unique record key is `(account_id, kind, context_id, target_id)`. Visits/wishes have null context_id and share place history across journeys; guide_progress uses stable route_id, so reused stops in different routes do not share progress. Only retained route identity inherits progress across revisions. The server preassigns a stable record_id for each logical key and publishes account-scoped offline mappings in resource metadata. Initially the state is absent and base_revision is zero. Events must use these mappings; reject arbitrary alternate IDs for one logical key or reassignment of an ID to another target. Read mappings in pages rather than loading an entire province.

## 3 Fictional ticket fixture

This logical fixture is not a real train, nor does it establish a journey through Jiangsu. Unicode escapes keep samples identical across document languages and must decode into actual UTF-8 for display.

```json
{
  "schema_version": 1,
  "fixture": true,
  "id": "fixture-ticket-001",
  "revision": 1,
  "itinerary_id": "fixture-js-trip-001",
  "travel_date": "2026-10-03",
  "timezone": "Asia/Shanghai",
  "train_number": "TEST-001",
  "boarding_station": {"id": "fixture-station-b", "name": "\u6837\u672c\u7ad9 B"},
  "arrival_station": {"id": "fixture-station-d", "name": "\u6837\u672c\u7ad9 D"},
  "carriage": "02",
  "seat": "03A",
  "departure_at": "2026-10-03T23:58:00+08:00",
  "train_model": null,
  "missing_reason": {"train_model": "source_unavailable"},
  "review_state": "published",
  "provenance": {
    "source_type": "fixture",
    "source_id": "fixture-ticket-001",
    "fetched_at": null,
    "reviewed_fields": ["boarding_station", "arrival_station", "train_number", "travel_date", "carriage", "seat", "departure_at"]
  },
  "stops": [
    {"station_id": "fixture-station-a", "name": "\u6837\u672c\u7ad9 A", "sequence": 0, "arrival_at": null, "departure_at": "2026-10-03T23:20:00+08:00", "time_kind": "scheduled"},
    {"station_id": "fixture-station-b", "name": "\u6837\u672c\u7ad9 B", "sequence": 1, "arrival_at": "2026-10-03T23:55:00+08:00", "departure_at": "2026-10-03T23:58:00+08:00", "time_kind": "scheduled"},
    {"station_id": "fixture-station-c", "name": "\u6837\u672c\u7ad9 C", "sequence": 2, "arrival_at": "2026-10-04T00:20:00+08:00", "departure_at": "2026-10-04T00:25:00+08:00", "time_kind": "scheduled"},
    {"station_id": "fixture-station-d", "name": "\u6837\u672c\u7ad9 D", "sequence": 3, "arrival_at": "2026-10-04T01:00:00+08:00", "departure_at": null, "time_kind": "scheduled"}
  ]
}
```

Acceptance assertions: boarding B, origin A, arrival date October 4, C dwell 300 seconds, unavailable model. Add standing-seat, absent carriage, duplicate station name, incomplete date and actual/scheduled time cases.

## 4 Resource format and activation

Version 1 devices do not extract ZIP archives. The server publishes a manifest; devices download individual files into staging, verify and switch the active index. Allow plain text, supported indexes, verified fonts/bitmaps and supported audio. Reject scripts, executable content and unregistered codecs.

Each file also requires file_id, unique within the pack revision and used by the download API; path is not an authorization identity. Manifest fields: schema_version, pack_id, revision, kind, fixture, total_bytes, files, min_app_contract and glyph_inventory. Each file has relative path, size_bytes, sha256, media_type and format. Digests must be actual SHA-256 values; this document supplies no dummy digest as a valid fixture. Sum file sizes and separately account for filesystem/staging overhead. Reject absolute paths, `..`, empty segments, backslashes and symlinks after defined normalization. Manifests must not redirect to arbitrary external URLs.

Logical directory example:

```text
packs/<pack_id>/<revision>/manifest.json
packs/<pack_id>/<revision>/data/itinerary.json
packs/<pack_id>/<revision>/data/places-index.json
packs/<pack_id>/<revision>/text/stop-001.txt
packs/<pack_id>/<revision>/audio/stop-001.pcm
packs/<pack_id>/<revision>/fonts/glyphs.json
```

`glyphs.json` declares coverage requirements and is not a font. D1 uses generated fixed subsets covering the small fixture. D4 must prototype dynamic province text; a manifest alone cannot prove rendering. Missing glyphs reject activation with a reason; prepare a renderable revision rather than dropping text. Removing packs must still leave historical summaries readable via validated name snapshots or separate summary assets, retaining required glyphs or pre-rendered display material. Saving names while deleting their only font is insufficient. Budget summary material with persistent records and clean it separately from province caches.

States: absent → offered → preflight → downloading → verifying → activating → ready, with failed/cancelled exits. Retain the old ready revision until replacement completes. On boot inspect staging and active indexes; partial packs never become ready. D1 restarts interrupted downloads rather than promising resumability, while still requiring streamed writes. Staging cleanup must not remove active packs or personal records.

Preflight requires space for new files, measured storage metadata and a declared reserve. Existing packs/records are not reclaimable unless the user explicitly removes them first. The active-index update must survive power loss. Measure actual overhead instead of substituting an arbitrary percentage.

## 5 Proposed initial limits

These are reviewable rejection boundaries, not measured capacity. Tighten unsuitable limits through review rather than enlarging them until RAM is exhausted.

| Item | Initial proposal |
| --- | --- |
| ID, name, title | 64, 128, 128 UTF-8 bytes; truncate only on character boundaries |
| Search initials | Eight ASCII letters |
| API JSON response, manifest | At most 32 KiB each, preferably streamed; avoid multiple simultaneous copies |
| Sync batch | At most 20 events and 8 KiB total request |
| Object lists | At most 20 per page with cursors; 128 rail stops, 64 guide stops; no all-at-once RAM requirement |
| One narration text | 8 KiB with screen pagination; directory references text files |
| Manifest files | 128; sharded province indexes, place counts measured separately |
| Network/file data block | Initially 4 KiB; TLS buffers budgeted and measured separately |
| D1 fixture pack | 768 KiB cap, subject to actual preflight |
| Local pending events | 256; prompt near limit, reject additions when full without silent loss |
| Connections/requests | Ten-second connect, 30-second ordinary request, 15-second download stall and five-minute total download |
| Retry | Three device attempts per cycle: initial, then after two/four seconds; pause until manual retry or connectivity recovery. Reschedule temporary server DAV failures after 60 seconds; permanent errors await configuration repair |
| Recording | Ten seconds maximum, chunked transmission; monotonic duration independent of wall-clock trust |

Server uploads initially cap individual documents at 10 MiB; this is not a device-response allowance. Browser/server validate formats, counts and decoded size; OCR jobs have separate queue limits. Devices retain chosen complete revisions based on bytes rather than promising a fixed province/itinerary count.

## 6 Proposed device API

These are unimplemented endpoints. Website sessions have separate login and cross-site request protection; devices use revocable credentials, and WebDAV credentials remain server-side. Keep API access on the controlled server origin; redirects must not forward Authorization to untrusted hosts.

| Method and path | Request/response meaning |
| --- | --- |
| POST `/api/v1/web/device-bindings` | Authenticated website creates one-use code/expires_at; does not directly activate device |
| POST `/api/v1/device-bindings/exchange` | Code and display identifier return pending binding_id/account label; rate-limited public exchange |
| POST `/api/v1/device-bindings/{id}/confirm` | Only the originating short-lived authenticated exchange session submits physical confirmation and receives scoped credentials |
| GET `/api/v1/device/catalog?cursor=...&limit=20` | Available revisions, coverage, sizes, review state, next cursor; own account only |
| GET `/api/v1/device/packs/{id}/{revision}/manifest` | Immutable manifest/compatibility/digests |
| GET `/api/v1/device/packs/{id}/{revision}/files/{file_id}` | Manifest ID locates files server-side, not arbitrary caller paths |
| POST `/api/v1/device/sync` | Push events/pull cursor, per-event results and bounded record changes |
| GET `/api/v1/device/status` | server_epoch, server_time, visible cursor and snapshot/status, no other-account content |
| POST `/api/v1/web/backups` | Start asynchronous complete snapshot job_id, never report queueing as completion |
| GET `/api/v1/web/backups/{job_id}` | Job state/error/completed snapshot_id |
| POST `/api/v1/web/restore-previews` | Validate snapshot in isolated recovery environment; final restoration requires confirmation |

Errors have stable code, user message, optional retry_after_seconds and request_id. Initial codes: `AUTH_REQUIRED`, `BINDING_EXPIRED`, `REVISION_CONFLICT`, `SCHEMA_UNSUPPORTED`, `SIZE_LIMIT`, `NO_SPACE`, `HASH_MISMATCH`, `GLYPH_MISSING`, `PROVIDER_UNAVAILABLE`, `DAV_PENDING`, `DAV_QUOTA`. Never log passwords, seeds, full tickets or binding codes.

Display identifiers are not secrets. Rate-limit bad binding codes and keep exchanged bindings pending until physical confirmation. Review exchange randomness, TLS bootstrap and identity proof before D1 implementation; endpoint names alone do not establish security.

Before binding requests, devices persist a random request ID and short-lived retry proof as sensitive session material. Both exchange and confirmation are idempotent: lost-response retries for the same request return the same pending session; different requests cannot re-exchange a consumed code. After a successful confirmation with a lost response, the originating session can recover the same result within its five-minute window. Protect results temporarily, erase recovery material on expiry and require rebinding; unredeemed codes retain their original expiry. Retries do not create duplicate devices or invalidate previously returned credentials. Clients privately retain long tokens; the server retains their digests long-term. Implement/test protection and deletion of temporary recoverable results. Revocation prevents session retries from resurrecting credentials.

## 7 Event deduplication and conflicts

device_epoch is a durable event-instance identifier unchanged by ordinary reboot and carried explicitly to validate predecessor scope. sequence is an integer from 1 through 2^53−1; start a new epoch before exhaustion rather than overflowing or reusing IDs.

Propose IDs combining durable device_id, a durable boot-independent epoch and monotonic sequence. Persist sequence allocation consistently with events; new identity/reinitialization requires a new epoch. Deduplicate by account, device_id and event_id. Identical retries return the original result; reused IDs with different content are rejected.

Operations are `visit.set`, `wish.set` and `guide_progress.set`, using explicit booleans/defined stop states rather than retry-sensitive toggle. Website changes use the same revision model. New records use `base_revision: 0`; updates carry known revisions. A local database transaction persists accepted events, record updates and upload jobs before acknowledgement.

Matching revisions update; independent targets merge. A stale same-target operation matching the current value can be a no-op; a different value conflicts, returning current revision/state. Keep conflict events visible, never overwrite based on unknown time. A website resolution retains current value or creates a new explicit overwrite with current base_revision; mark the original conflict resolved rather than replaying it.

Design device_id, event epoch, sequence and payload as one recoverable log. Event IDs remain at most 64 bytes; fixed-length opaque IDs are allowed, concatenating three maximal 64-byte fields is not. Device state is an acknowledged snapshot plus pending events. Clear pending events only after acknowledgement; reconnect retries do not duplicate changes. Preserve visible conflict information. Full queues/save failures never show stamp success. Personal records and resource packs persist separately.

Sequential offline changes to one record need ordering rather than treating the second change as external conflict. Propose predecessor_event_id: the server uses the accepted predecessor's resulting revision as the effective base. A subsequent website modification still creates a real conflict. An unreceived predecessor returns dependency pending without acknowledgement; predecessor conflicts leave dependent events in the same unresolved group. Scope dependencies to one record, do not block unrelated records and reject fabricated cross-epoch dependencies. Extend A05 with offline visit/undo/revisit and intervening website corrections.

Example sync request/response shapes use test IDs only:

```json
{
  "schema_version": 1,
  "server_epoch": "fixture-server-epoch-1",
  "pull_cursor": null,
  "events": [{
    "event_id": "fixture-event-1",
    "device_id": "fixture-device-1",
    "device_epoch": "fixture-device-epoch-1",
    "sequence": 1,
    "record_id": "fixture-progress-stop-001",
    "context_id": "fixture-route-001",
    "target_id": "stop-001",
    "operation": "guide_progress.set",
    "value": true,
    "base_revision": 0,
    "predecessor_event_id": null,
    "device_time": null,
    "time_trust": "unknown"
  }]
}
```

```json
{
  "schema_version": 1,
  "server_epoch": "fixture-server-epoch-1",
  "results": [{"event_id": "fixture-event-1", "status": "acknowledged", "record_revision": 1}],
  "changes": [{"id": "fixture-progress-stop-001", "kind": "guide_progress", "context_id": "fixture-route-001", "target_id": "stop-001", "revision": 1, "state": true, "updated_by": "fixture-device-1"}],
  "next_cursor": "fixture-cursor-1",
  "has_more": false,
  "backup": {"state": "pending", "included_cursor": null, "snapshot_id": null}
}
```

Result states are acknowledged, conflict, rejected or dependency_pending. Unlisted events remain unacknowledged. Targets must exist in the account's published content or valid history; events cannot create arbitrary private targets. Reject mismatched server_epoch for reconciliation before accepting pushes. Backup included_cursor is a change-coverage watermark, not a timestamp comparable lexically by clients; the server reports whether current operations are covered.

The server change log stores immutable record revisions/tombstones in commit order. Cursors retain a read watermark and a cycle upper bound, rather than paginating a mutable current-record list; finish the bound before reading newer commits to avoid omissions. Retain deduplication/predecessor versions until explicit device watermarks permit safe compaction; D1 does not automatically delete entries with remaining dependencies. Conflicted/rejected events remain visible and can be abandoned or corrected; permanent errors do not retry indefinitely.

## 8 Cursors and server recovery

Pull cursors are opaque server strings stored with server_epoch. Responses include next_cursor/has_more; apply and persist a page before advancing its cursor. Database deduplication/transactions cannot rely only on HTTP success.

Restoring creates a new server_epoch and revokes old device tokens. Rebound devices enter reconciliation on epoch change rather than trusting old sequence positions. Fetch snapshots in pages while retaining unacknowledged events. Also compare acknowledged local records missing from the restored snapshot; a previous acknowledgement that cleared a queue must not silently erase them. Resubmit differences as new events with current revisions and user conflict handling, and reverify resource versions.

Include deletion tombstones in retained backups. Missing from one list does not imply deletion. D1 initially supports one device; future multi-device use still requires server revision conflicts rather than assuming conflict-free merging.

## 9 WebDAV persistence and restoration

Keep the working database on a local volume and immutable resources/complete record snapshots in WebDAV. Proposed private-root layout separates account IDs:

```text
passport/v1/accounts/<account_id>/resources/<pack_id>/<revision>/...
passport/v1/accounts/<account_id>/snapshots/<snapshot_id>/records.json
passport/v1/accounts/<account_id>/snapshots/<snapshot_id>/manifest.json
passport/v1/accounts/<account_id>/snapshots/<snapshot_id>/complete.json
passport/v1/accounts/<account_id>/otp-backups/<backup_id>/ciphertext.bin
passport/v1/accounts/<account_id>/otp-backups/<backup_id>/manifest.json
```

OTP paths are future isolation boundaries, not real-seed files or a frozen cryptographic format in D1. Encrypt sensitive labels/parameters later rather than leaking them through public manifests.

Proposed publication: obtain a consistent database export/included_cursor, upload missing immutable resources, upload records/manifest, verify required references, then publish complete. Interrupted snapshots lack complete and cannot restore. A latest index is only a hint; enumerate and verify complete snapshots if damaged rather than trusting one overwritten file. Digests verify integrity, not authentication/authorization.

Verify actual WebDAV service behavior. [RFC 4918](https://www.rfc-editor.org/rfc/rfc4918.html) defines methods and conditional processing but does not establish whole-snapshot transactions on the target server. D1 checks directory creation, upload/download/listing, overwrite conflicts, permissions, quotas and interrupted uploads. Verify ETags/conditions if used. Version 1 does not depend on LOCK or cross-file transactions; use unique revision paths and a final completion marker.

Local durable storage, DAV file upload and a complete recoverable snapshot are distinct. Uploaded files without a complete manifest remain partial. Show pending, uploading, complete, failed and the last complete revision. Devices receive backup state for the relevant revision; do not display an older snapshot as coverage for current changes.

Restore in an empty database/isolated environment: privately configure DAV, list complete snapshots, verify versions/sizes/digests/references, preview record counts/missing content, confirm import, establish new server_epoch, rebind devices and reconcile differences. Recovery from DAV must not require the original database. Pending uploads and unretained deletions cannot be guaranteed recoverable. Decide retention/deletion before real data; never automatically remove the last complete snapshot initially.

Complete snapshots also contain stable account/place/route identities, published objects/revisions, resource mappings, records/tombstones and consistent change watermarks/deduplication material. Restoring records.json alone without referenced routes/places is insufficient. Password hashes, long device tokens, binding sessions and DAV passwords must not automatically revive authentication. An isolated local bootstrap resets admin/DAV credentials, preserves the data account ID and creates a new server_epoch. Restore accepts only validated relative references within snapshot/allowed resource roots, never arbitrary local files or external addresses. Retention protects both complete snapshots and referenced resources.

## 10 Required OTP backup boundaries

Seeds and generation parameters require a separately encrypted backup persisted to WebDAV through the server. The user independently retains decryption ability; it cannot be bound solely to the original device or packaged with ciphertext in the same ordinary backup. Recovery login must not rely solely on that device's OTP. Select algorithms, provisioning, counters and key management last; this contract does not freeze them early.

Backup status must correspond to the current account revision; changes pending upload are incomplete. Missing credentials, damaged ciphertext or incompatible formats explicitly fail rather than generating plausible wrong codes. Verify replacement-device recovery with test accounts in the final stage. Restore does not invalidate lost-device seeds; loss response includes token revocation and account-secret replacement.

### D1 experimental storage layout

This reviewed experimental proposal is not written into partitions.csv or evidence of final integrated capacity. Implement only on an authorized feature branch and review real-data migration separately. No OTA dual slots.

| Region | Type | Offset | Size |
| --- | --- | --- | --- |
| nvs | data/nvs | 0x9000 | 0x6000 |
| phy_init | data/phy | 0xF000 | 0x1000 |
| factory | app/factory | 0x10000 | 0x500000, 5 MiB including linked fonts |
| records | data/nvs | 0x510000 | 0x20000, 128 KiB |
| cache_a | data/fat | 0x530000 | 0x168000, 1.40625 MiB |
| cache_b | data/fat | 0x698000 | 0x168000, 1.40625 MiB |

The layout ends at 0x800000, retaining the original app offset; boot/partition metadata occupy earlier addresses. Propose IDF FAT with wear levelling for resources and separate NVS for personal events/summaries, following [NVS](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-reference/storage/nvs_flash.html) and [FAT](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-reference/storage/fatfs.html). Physical power-loss tests remain required; storage APIs are not multi-field transaction guarantees.

The two cache regions total 2.8125 MiB, each containing a complete resource generation. Keep the active slot read-only, write updates only to the inactive slot and switch the versioned NVS pointer after verification. D1 generations contain one fixture pack; later multi-pack generations or alternate strategies need review. Writing other files in an active FAT volume does not isolate old packs. Two 768 KiB fixtures occupy 1.5 MiB, leaving 0.65625 MiB raw per slot, 1.3125 MiB total, before measured overhead. Validate pointers/digests at boot and retain the old slot for incomplete updates. Mount failure never autoformats; physical power-loss tests remain required. A 128 KiB records region does not guarantee 256 worst-case events plus all summaries. Enforce count and actual entry/byte availability, rejecting additions at either limit without losing history. Persist the journal before deriving state; persist acknowledgement receipts and updated base state before clearing queues. Version cursors/active pointers recoverably rather than treating multi-key NVS writes as atomic. Cache corruption never automatically formats records; show recoverable failure and clear resource caches only after explicit user choice.

## 11 Version 1 freeze conditions

Review fields, resource encoding, event revisions, rejection bounds, provisioning/TLS bootstrap, experimental partitions/migrations, DAV publication and test entry points before freezing. Parsing the sample JSON is design verification, not proof that implementation supports it. Actual manifest/audio/font outputs are produced in D1/D3.

Mocked DAV, simulated devices and fixed-success responses leave A06/device acceptance unverified. Live railway/AI adapters belong to their stages. Contract extensions need versions and compatibility rather than changing v1 so old devices silently misread it.
