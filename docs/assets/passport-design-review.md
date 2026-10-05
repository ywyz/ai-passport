[简体中文](passport-design-review.zh_CN.md) · **English**

# AI Passport D0 Design Review Record

> **Superseded (2026-10-05):** Historical reference only, no longer implementation or acceptance authority. The [integrated firmware plan](passport-suite-plan.md) retains translation, OTP, the merged wallet, Nextcloud tasks and the three-body clock/weather; other features are cancelled. Do not execute old stages, phone/SoftAP provisioning proposals or implementation prompts directly.

Date: 2026-10-03. Reviewer: Codex. Scope: [first-batch PRD](passport-travel-prd.md), [data contract](passport-travel-contract.md), [OpenCode prompts](passport-travel-prompts.md). This is document/boundary inspection, not independent external review, implementation verification or security certification.

Conclusion: eight groups of gaps are corrected in both document languages, ready for revised-design confirmation and D1 handoff preparation. Storage, provisioning and database choices remain proposals. Physical capacity and external services remain unverified; corrected prose is not product PASS. No branch, project, executable fixture or firmware feature is created.

## 1 Findings and revisions

Revised means a document/contract correction, not implemented or tested behavior.

| ID | Priority | Gap and impact | Revision | Acceptance |
| --- | --- | --- | --- | --- |
| DR01 | High | Arbitrary record IDs and unscoped reused stops could duplicate/cross journey progress | Account/kind/context_id/target_id uniqueness, server ID mappings, route_id guide context | A05, A09 |
| DR02 | High | Lost binding responses lacked recovery, risking duplicates/inaccessible tokens | Durable request/retry proof, idempotent exchange/confirmation, five-minute recovery, expiry and no revoked-session revival | A02, A15 |
| DR03 | High | Offline ordering and feed pagination/compaction could create false conflicts or omit changes | Predecessors, immutable change log, bounded cursor rounds, retained dedup dependencies and resolvable permanent errors | A05 |
| DR04 | High | Multi-key NVS writes, acknowledgement cleanup and long IDs lacked crash consistency | Journal-derived state, durable receipts/base before cleanup, recoverable pointers and 64-byte IDs | A03, A04, A05 |
| DR05 | Medium | Missing manifest file_id and deleting fonts could break downloads/history text | Unique per-revision file_id, persistent summary glyph/display material outside caches | A03, A13 |
| DR06 | High | Records-only snapshots omitted identity, publication and authentication recovery | Stable identities, published objects/mappings/watermarks; authentication bootstrap rather than revived tokens | A06, A15, A17 |
| DR07 | Medium | Recording limit had no send action; text/action keys, menu closing and stamp reversal ambiguous | Explicit send-confirm state, reading mode, parent Back versus Close, current/historical stamps | A01, A10, A13 |
| DR08 | High | Category budgets were not a usable layout and count caps implied capacity | Experimental layout/IDF storage, isolated read-only active/update cache slots, dual-version arithmetic, count/space bounds, no automatic record formatting | A03, A04, A16 |

Scope/order, clock home, deferred features and OTP-last decisions remain intact. OTP still requires separately encrypted original-device-independent backups; no cryptographic algorithm is selected here.

## 2 Implementable D1 boundary

D1 has three increments on `feature/passport-core`. This is handoff sequencing, not present implementation authorization.

| Increment | Inputs and outputs | Acceptance focus |
| --- | --- | --- |
| D1a Website/server/DAV | Linux/Docker Compose, local working DB, private website, fixture publication, events, complete snapshots/restoration | Host/contracts; actual DAV without inputs remains unverified, mocks separate |
| D1b Device persistence/resources | New UI, fixed Chinese subset, experimental partitions, journal, preflight, per-file download/verification | Static layout/formats, allocation/save/cancel/power-loss faults; experimental device without existing real records |
| D1c Provisioning/full loop | Local SoftAP website, physical binding, HTTPS bootstrap, retries, offline completion, reconnect/restore reconciliation | Actual phone/browser/device, repeated events, DAV outages, older snapshot/device differences |

D1 content/records are foundation fixtures; complete railway/guide/check-in pages remain D2 through D4. Avoid separately implementing incompatible complete firmwares. Regress earlier increments and perform only authorized Git commits/integration.

## 3 Decisions and measurements still needed

| Item | Current state | Timing |
| --- | --- | --- |
| Linux/Docker Compose | User-confirmed | D1 local deployment materials; real deployment separately authorized |
| SoftAP/SQLite/single-user/experimental layout | Proposals with clarified boundaries | Confirm D1 tasks, prototype during coding and revise if unsuitable |
| Jiangsu fixture | Explicitly fictional, real province pending | Does not block D1; select before D4 real data |
| Partitions/storage | Address/boundary/arithmetic inspection only | D1 actual table gate and physical space/power-loss tests |
| Fonts/TLS/heap | Baseline information, not new-product measurements | D1 measurements; no final capacity promise |
| Actual DAV | No endpoint/credentials, no connection this turn | D1 A06; privately configure credentials |
| Railway timetable/board/model | No selected verified live source | D2 A08; schedules never substitute for live boards |
| Compressed audio/AI | No codec/decoder/provider measurement | D3 A10/A11; offline narration cannot be silently dropped |
| OTP | Backup/loss-recovery requirements only | D9 test seeds/replacement-device acceptance, no early live accounts |

Keep the default partition table unchanged. Proposed 5 MiB app space includes linked fonts; two FAT slots total 2.8125 MiB, 1.40625 MiB each, raw rather than fully available narration capacity, and 128 KiB records cannot guarantee unlimited history. At capacity, show failure and require sync/export without silently deleting records. Establish migration/flash preservation before real data.

## 4 Design checks and handoff conditions

Design checks cover paired sections/IDs, local links, parsed fixtures, midnight/five-minute dwell, route-scoped progress, receipt versus backup, nonoverlapping partitions ending at 8 MiB and D1 coverage of DR01 through DR08. Host/baseline gates are reported separately; real product A01 through A17 are not passed by this review.

The next OpenCode task is [D1 in the prompt package](passport-travel-prompts.md#2-d1-shared-foundation-implementation-prompt), creating its branch from the exact baseline and delivering D1a through D1c evidence. Confirm revised tasks/coding authority before execution. Codex designs/verifies and OpenCode implements; Codex then reviews, OpenCode repairs, and authorized physical/integration tests follow.
