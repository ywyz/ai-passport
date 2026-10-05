[简体中文](passport-travel-prompts.zh_CN.md) · **English**

# AI Passport First Travel Feature OpenCode Tasks

> **Superseded (2026-10-05):** Historical reference only, no longer implementation or acceptance authority. The [integrated firmware plan](passport-suite-plan.md) retains translation, OTP, the merged wallet, Nextcloud tasks and the three-body clock/weather; other features are cancelled. Do not execute old stages, phone/SoftAP provisioning proposals or implementation prompts directly.

Updated: 2026-10-03. These are design deliverables, not implementation authorization in this conversation. Execute coding prompts only after the user approves the detailed PRD and the stage. See the [PRD](passport-travel-prd.md), [data contract](passport-travel-contract.md) and [overall plan](passport-suite-plan.md). Codex reviews/verifies; OpenCode implements/repairs.

## 1 D0 design review prompt

Codex performs this design review. OpenCode implementation begins with an approved D1.

```text
Review design only. Do not implement code, create/switch branches, commit,
push, deploy or flash. Start with git status --short --branch; preserve work.
Read AGENTS.md, docs/README.md, docs/assets/passport-travel-prd.md,
docs/assets/passport-travel-contract.md and the overall plan.

Check reachable screens and Back/cancel paths; fixture versus real railway
fields; bounded audio/text/font processing with no PSRAM and 8 MiB Flash;
power-loss-safe old packs; idempotent event retries and concurrent revisions;
reconciliation of previously acknowledged but not DAV-persisted records after
restoring an older server snapshot; provisioning/binding/TLS boundaries; complete
DAV snapshots and independently recoverable encrypted OTP backup requirements.

Report blockers, suggested changes and acceptance IDs. Never present proposed
choices as confirmed user decisions. Review only OTP backup/recovery requirements,
not algorithms, seed provisioning or hardware security implementation.
```

## 2 D1 shared foundation implementation prompt

This covers a minimal firmware/website/server/WebDAV loop. Execute only with D0 review results and explicit D1 authorization. Update it for final deployment/provisioning decisions rather than treating superseded proposals as approved.

```text
You are OpenCode implementing the user-approved D1 foundation. Codex reviews
and verifies. Start with git status --short --branch, preserving untracked
planning documents and other user work. Read AGENTS.md, docs/README.md and the
three passport-travel-* documents under docs/assets/. Check all five passport-*
Skills and use applicable development/build Skills.

Design baseline: 0b9e4c81ee4421c0bac39ca3561d65a8285acd4a.
Target branch: feature/passport-core; never implement on main. If the base has
changed, report exact differences and a compatible base. Inspect an existing
same-name branch's ownership; never recreate/reset it.
Contract: passport-travel-v1-draft, frozen only for the revised, user-confirmed
D1 core scope. Do not freeze later audio, dynamic fonts or OTP here.

Complete this loop: phone/desktop website login and publication of a labeled
fixture pack; device provisioning and physically confirmed binding; download,
verify and activate; disable Wi-Fi, view text and persist fixture stop completion;
reconnect/upload; website displays records and separate DAV backup status;
restore an empty test server from a complete DAV snapshot, rebind and reconcile
local device differences.

Implement new P00/P01/P02/P03/P04 screens and a labeled fixture content detail.
Do not reuse demo menus or ui_pixel. Generated fonts cover all test content,
including U+752A and a negative coverage test; UTF-8 alone proves no glyphs.
Follow proposed click/hold suppression, repeat acceleration, Back and cooperative
cancellation rules as accepted in the PRD.

Implement approved time-limited SoftAP website provisioning without browser BLE,
physical binding, scoped device credentials and revocation. Design TLS bootstrap
and time initialization without disabling hostname/trust/validity checks. Never
log or commit passwords, binding codes or tokens.

Proposed location: companion/passport-server/. Implement approved private
single-user server, responsive website, local database, durable upload queue and
DAV adapter. Verify official dependency documentation and pin versions at coding
time. Local test instances are allowed; production deployment is not. Show queued,
ready, server acknowledgement and DAV complete as different states.

Implement per-file manifests, bounds/path checks, actual SHA-256, space preflight,
old-pack retention, power-loss-safe activation, cancel and restart-on-interruption.
Measure fixture storage, propose and validate application partitions/migration
before adding them on the feature branch. Never make them mandatory upstream
layout or test destructive migrations with real records. Report flash data impact.

Persist events, explicit set operations, per-event acknowledgement, retry-safe
deduplication, revision conflicts and paginated pull. Database transactions include
records and upload jobs. Restores create new server_epoch, revoke old tokens and
reconcile records. DAV uses immutable resources, complete snapshots and a final
completion marker, not live SQLite on a remote mount. Reserve only the encrypted
OTP backup boundary; no OTP generation or real seeds.

Read docs/assets/passport-design-review.md and implement DR01 through DR08.
Use three verifiable increments on the same branch: website/server/DAV; local
persistence/resource activation; provisioning/binding/full device loop. Validate
the proposed experimental layout and storage fault prototype first; never assume
old-layout compatibility before real records. Regress checks after each increment.

Acceptance: A01/A02/A03/A04/A05/A06/A15, plus A16 for current foundations.
D1 A04/A05 use fixture stop progress, not completed check-ins. Actual-device or
actual-DAV criteria without evidence remain NOT RUN/NOT VERIFIED, never mock PASS.
Do not implement complete D2/D3/D4 pages, real railway, AI, clock or deferred work.

Wire new host/website/server tests into reproducible entry points. Cover hold-click
suppression, queue full, save failure, traversal, digest/glyph/size rejection,
power loss retaining old packs, event duplicates, stale conflicts, DAV outages,
damaged snapshots and empty restoration. Report mock and real integration separately.
Run the complete ./tools/validate.sh in ESP-IDF 5.5.3 and verify merged image/debug
archive identity. Report Flash/static RAM and observed heap/stacks without inventing
hardware measurements.

Deliver scope, run/test commands, startup/keys, dependency versions, migration,
artifact identities and evidence per acceptance ID. Separate Build, Host tests,
Device tests and Unverified. Do not commit, merge, push, publish, deploy production,
modify real accounts, flash, erase or burn eFuses. Offer device testing with the
exact image and data impact after implementation and wait for flash authorization.
```

## 3 Later stage task blocks

Add these to the overall plan's common coding prompt. Fill the base with an actually accepted integration commit; do not invent it ahead of time. Stage approval is required before execution.

| Stage and branch | Required tasks and acceptance |
| --- | --- |
| D2 `feature/passport-rail` | R10 through R14, R01 through R07 and web ticket review. A07 tests midnight/intermediate boarding; A08 separately verifies actual sources. Do not claim automatic screenshot/document extraction. Keep unknown models/board fields absent, and live access unverified where unavailable rather than claiming all railway complete. |
| D3 `feature/passport-guide` | G20 through G24/G01 through G07, three-stop offline audio, draft/review/publication, multiple journeys, completion/undo and revision migration. Measure compression before freezing. Route-scoped bounded/cancellable voice follow-up. A09/A10/A11 and applicable A16. |
| D4 `feature/passport-checkin` | C30 through C35/C01 through C08, resident 34-region structure, chosen province downloads, initials/repeats, independent records, stamps/achievement reversal and all auxiliary pages. Review actual data and map licenses. A12/A13/A14 and regression A03 through A06/A15/A16. |

Every stage regresses existing behavior and delivers its own feature branch. Only authorized Git operations merge into `feature/passport-suite`; final integration tests remain mandatory. OTP uses A17 in its final stage, not early implementation because backups appear in the current contract.

## 4 Codex review and verification prompt

```text
You are Codex reviewing and verifying an authorized OpenCode implementation,
not repairing firmware yourself. Check exact branch/base/diff/contract and pending
work first, preserving others' edits. Read AGENTS.md and the three travel documents.
Run relevant host/companion checks and the complete firmware gate. Match full.bin
to ELF/MAP identity and confirm tests are actually registered.

Map A01 through the current stage to PASS/FAIL/NOT RUN/NOT VERIFIED, with evidence
and conditions. Real DAV, provisioning, glyphs, audio and live railway cannot PASS
without actual evidence. Inspect older-snapshot record reconciliation, cancel and
page lifecycle, disk/queue exhaustion, authorization, old-pack retention and caps.

Deliver severity-ranked issues with files/locations, triggers, user impact,
acceptance IDs, OpenCode repair instructions and rechecks. OpenCode makes repairs.
Provide reproducible physical-device steps, exact image and data impact without
unauthorized flashing/deployment. Report Build, Host tests, Device tests and
Unverified separately. Compilation cannot hide integration failures or untested
hardware. Draft documents do not automatically approve all implementation.
```

## 5 This stage delivery checks

D0 provides pages/keys, logical fixtures, data/API drafts, recovery states, caps, acceptance IDs and prompts. Next are review decisions and D1 coding authorization, without production credentials. This stage performs document checks and baseline verification rather than implementation or real-service acceptance.
