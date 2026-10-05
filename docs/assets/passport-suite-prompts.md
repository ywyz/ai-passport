[简体中文](passport-suite-prompts.zh_CN.md) · **English**

# Suite Implementation Tasks and Codex Review

## 1 Execution boundary and common prompt

Updated 2026-10-05, Asia/Shanghai. [Plan](passport-suite-plan.md) fixes scope; [PRD](passport-suite-prd.md) fixes SU outcomes/pages; [contract](passport-suite-contract.md) specifies SC proposals. U/D/C/V meanings follow the PRD. All stages below are D/V future work, not current authorization; C covers baseline APIs/tooling only. This round delivers the complete translation and OTP detailed design, not firmware. The old travel D1-D9, acceptance IDs and storage/provisioning instructions must not be run.

Prepend this to each newly authorized stage prompt:

```text
Implement only the explicitly authorized ST stage from passport-suite-prompts.
Read AGENTS.md and routed documents; check all five passport skills and use
only applicable skills. Start with git status --short --branch and rev-parse HEAD.
Review any delta from 0b9e4c81ee4421c0bac39ca3561d65a8285acd4a, preserving user
changes and the existing untracked docs/assets documents. Do not reset/clean.
For the first firmware stage, create feature/passport-suite from the verified
baseline; do not develop on main. If branch/baseline differs, inspect changes
and preserve the active feature branch. Do not merge several demo branches.
Use codebase-memory and CodeGraph when indexed, check coverage/freshness;
when unavailable/unindexed verify targeted source and state that limitation.
Treat unapproved Q decisions as blockers for the dependent implementation;
continue only independent approved work, never quietly choose production
security/provider/storage policy. Use fictional data/public vectors only.
Redesign all application UI. Put pages/state/workers in main, reusable hardware
extensions in BSP, reusable fonts/assets in assets. Never copy ui_pixel shell.
Keep pure logic/protocol tests separate from ESP-IDF/LVGL; handle overload,
long-press suppression, cancellation, late callbacks and power loss.
Report U requirements, approved D choices, actual C implementation and V gaps.
No commit/push/deploy/flash/erase/eFuse action without separate authorization.
For firmware delivery run complete ./tools/validate.sh in ESP-IDF 5.5.3,
deliver exact verified merged image and matching ELF/MAP/manifest bundle/hash,
and report Build, Host tests, Device tests, Unverified separately.
After a complete firmware implementation, proactively offer authorized device
testing; follow ai-guide detection/approval/data-impact handoff. Gate or flash
success alone does not prove on-device acceptance. Do not request real seeds.
```

Proposed paths below are responsibility boundaries for future work, not files created in this round. Web/server roots do not exist as an implemented suite; agree their actual locations in Q-11 before adding them. Resolve nested AGENTS instructions first. Ordinary PRs leave both CHANGELOG files unchanged. A stage ends only when its dependent acceptance has evidence or a clearly reported blocked/unverified disposition, never when mocks merely compile.

## 2 Stage dependencies and ownership

| Stage | Dependencies and decision gates | Proposed file scope / deliverable | Acceptance |
| --- | --- | --- | --- |
| ST-01 Connection foundation | Verified feature baseline; Q-01/02/03/04/05/11; security review before credentials | main/suite_app.*, suite_input.*, suite_connection.*, suite_storage.*, main/CMakeLists.txt; measured application config/layout proposal; tests/test_suite_*; proposed web/ble and server/auth/time; BSP only separately reviewed generic changes | SA-01–06, SA-20 foundation |
| ST-02 Wallet and tasks | ST-01 transport/auth/time/storage; Q-06/07/11 | main/suite_wallet.*, suite_tasks.*; web/tickets and web/tasks; server/tickets, server/caldav, server/backup; schema fixtures/tests; reviewed cache protocol | SA-07–12, SA-21 ordinary backup |
| ST-03 Home and weather | ST-01 time/model/UI; Q-05/08; does not require speech or OTP implementation | main/suite_home.*, suite_weather.*, pure scene math; assets/fonts and license/inventory; web/settings; server/weather; default boot/navigation | SA-13, font/resource portions SA-16/20 |
| ST-04 Translation | ST-01 authenticated network/storage/admission/audio ownership, ST-03 font integration (or equivalent independently verified font work); Q-05/09 | main/suite_translation.*, suite_audio_job.*; server/speech adapters; web/language/provider settings; bounded audio/text tests | SA-14–16 |
| ST-05 OTP | ST-01 authenticated BLE/time/storage; verified font/input; Q-02/04/05/10; real-seed gate separate from software tests | main/suite_otp.*, suite_vault.*; pure TOTP tests; isolated web/otp local parser/recovery; server ciphertext-only backup endpoint; never ordinary ticket export | SA-17–19, OTP portion SA-21 |
| ST-06 Integrated validation | All feature stages, resolved relevant Q gates and matched build; authorized device access separate | tests suite integration/fault injection; tools only if needed for new tests; docs evidence and app migration/flash guide; no unrelated upstream rewrites | SA-20–22 and all unfinished device/service acceptances |

Suggested order ST-01 → ST-02 → ST-03 → ST-04/ST-05 → ST-06 addresses highest shared connection/storage risks first, then exercises real data/CalDAV, then establishes default home/fonts before audio/security integration. ST-04 and ST-05 have no dependency on each other: OTP vectors, URI parsing and vault prototypes may begin after their own gates or independently as pure tests once authorized, not necessarily last. Translation and OTP designs are already complete this round. Provider selection and OTP security review should run early because either can block its own stage; numerical order is not a renewed requirement to postpone OTP.

## 3 Stage prompts

Each block is appended to the common prompt only when that stage is authorized. Codex review must happen before claiming the stage accepted.

### ST-01 Connection foundation

```text
Implement ST-01 for SU-01–04 and SU-14 foundation, SC-01–04/10.
First resolve Q-01/02/03/04/05/11; present measurable decisions, not hidden defaults.
Create the new app shell/launcher with independent pages, input reducer and
page/background ownership; no test-menu/ui_pixel reuse. Instrument memory.
Implement connectable GATT framing/authentication, physical windows, staged
Wi-Fi tests/commit, idempotent binding, revocation, validated TLS/time bootstrap.
Keep old config after wrong password/cancel/disconnect/power loss. Validate
actual computer/browser support and propose approved desktop fallback if needed.
Deliver protocol/schema/negative vectors, pure host tests, configuration/layout
migration design, browser matrix and SA-01–06 evidence. Codex review must trace
startup, PRESS/LONG/CLICK, queue full, delayed callbacks, every secret path,
nonce reuse/replay, binding retries, certificate errors and first-time clock.
```

### ST-02 Wallet and tasks

```text
Implement ST-02 for SU-05–08/14, SC-05/06/10 on accepted ST-01.
Resolve Q-06/07/11. Add manual/paste draft-review-publication and immutable
snapshot activation; four ticket types, missing reasons, full dates/timezones,
original-code provenance and fit rejection. Do not fabricate admission codes.
Implement CalDAV discovery/VTODO reading/list selection and durable completion
intent with UID scope, If-Match ETag, lost-response reconciliation and conflict UI.
Keep newer remote edits and old active cache. No task creation or reopen control.
Deliver mock and real Nextcloud tests separately, offline/power-cut fixtures,
ordinary WebDAV receipts/recovery and SA-07–12/21 evidence. Codex review must
check midnight/DST, QR quiet zones/integer scale, deletion vs cache cleanup,
partial snapshot activation, pending queue reboot and concurrent website edits.
SA-09 actual reader scans require separately authorized device testing.
```

### ST-03 Home and weather

```text
Implement ST-03 for SU-09 and glyph/resource portions SU-11/14, SC-07/08/10.
Resolve Q-05/08. Make new three-body clock/weather the boot home; bounded
procedural scene, explicit city/timezone, 10 FPS ceiling, separate time/weather
trust and stale states. Weather failure must not block any other feature.
Integrate licensed/pinned Chinese fonts and declared dynamic inventory; verify
all labels/popups and negative glyph cases. Measure refresh/heap/stacks/power.
Deliver SA-13 and relevant SA-16/20 evidence. Codex review traces default boot,
launcher entries, timer stop on hiding, physics bounds and invalid weather/time.
```

### ST-04 Translation

```text
Implement ST-04 for SU-10/11, SC-08 with accepted connection/font/audio ownership.
Resolve Q-05/09 before a real provider adapter. Use click start/click stop,
10-second 16k mono PCM ceiling, bounded ring/backpressure/timeout/cancel handling.
Show source and translation separately, allow voice cancellation, refuse new
translation offline. Delete temporary audio/text per approved policy.
Deliver mock fault/bounds tests SA-14 separately from real bilingual service
and playback SA-15 and hardware glyph/cancel SA-16. Codex review inspects every
PCM/network allocation, overload paths, page-exit jobs, late provider responses,
billed retry ambiguity, actual provider/API mode and deletion/retention evidence.
```

### ST-05 OTP

```text
Implement ST-05 for SU-12/13, SC-09 with own connection/time/storage prerequisites.
Resolve Q-02/04/05/10. Start with RFC public vectors and fictional accounts;
review algorithm/URI compatibility, local PIN unlock/idle lock, trust bounds,
encrypted seed storage and capability-scoped BLE import/delete/restore.
Keep OTP parser local and independent of ordinary settings/ticket export.
Create an independently encrypted archive and demonstrate recovery without
original device and without backup login relying only on that device's OTP.
Do not enable security/eFuse operations or use real seeds by this prompt.
Deliver SA-17–19 and OTP SA-21 evidence. Codex reviews code paths and vectors,
secret lifetime, logging, nonce/key wrap, low-entropy PIN limitations, lockout
across reboot, deletion/old backups, migration and independent recovery access.
```

### ST-06 Integrated validation

```text
Implement only necessary ST-06 tests/fixes within agreed feature scope.
Recheck SU-01–15 → SC-01–10 → SA-01–22, unresolved Q gates and baseline delta.
Run complete ./tools/validate.sh with IDF 5.5.3; hand off the exact merged image
and matching ELF/MAP/manifest/hash. Test admitted overlap, 100 page cycles,
24-hour mixed operation, radio reconnect, audio cancellation, glyphs, real
Nextcloud conflicts, ticket scans, independent restore and layout preservation.
Device actions require separate approval; if unavailable retain NOT RUN and
explicit Unverified. Codex reviews coverage/evidence, excludes cancelled models,
verifies no inherited demo shell, and rejects mocks as real-service/device PASS.
Report each requirement/acceptance status and Build/Host tests/Device tests/
Unverified. Offer authorized device testing when firmware implementation is complete.
```

## 4 Requirement-to-protocol-to-task-to-acceptance map

All rows U outcome / D test definition / C absent for new features / V pending. Multiple SA IDs mean all corresponding evidence is needed; baseline static tests cannot fill these rows.

| Requirement | Protocol | Task | Acceptance and evidence type |
| --- | --- | --- | --- |
| SU-01 | SC-04/07 | ST-01/03/06 | SA-01 source startup inspection and device new UI; only five launcher entries |
| SU-02 | SC-01/02/03 | ST-01 | SA-02 actual computer chooser/GATT/permission/reconnect matrix; SA-03 candidate Wi-Fi wrong password/cancel/reboot and old config |
| SU-03 | SC-02/03 | ST-01/05 | SA-04 reviewed transcript/replay/MITM/identity/permission vectors plus physical confirmation; SA-05 invalid CA/hostname/expired cert, signed-time nonce/replay, revocation/binding retries |
| SU-04 | SC-04 | ST-01/04/06 | SA-06 host saturated queues, long-short suppression, exit/late callbacks, failed stop; device timing |
| SU-05 | SC-05 | ST-02 | SA-07 four types, manual/paste review/publish/download/details; missing provenance and midnight/DST fixture tests |
| SU-06 | SC-05/10 | ST-02/06 | SA-08 original code/hash/geometry/offline/expiry/old-cache tests; SA-09 separate real screen/reader scans and issuer-rule disposition |
| SU-07 | SC-06 | ST-02 | SA-10 mock discover/read/dates/notes/errors; SA-11 real versioned Nextcloud collections/UID/ETag read and device download |
| SU-08 | SC-06/10 | ST-02/06 | SA-12 real complete/offline reboot/lost response/412 concurrent edits/no overwrite/no recreate |
| SU-09 | SC-03/07 | ST-03 | SA-13 scene bounds host tests; boot home/time/weather stale/network failure/power on device |
| SU-10 | SC-08 | ST-04 | SA-14 mock chunk/order/timeout/overflow/limits; SA-15 real bilingual speech→text→translation→voice and provider failure |
| SU-11 | SC-04/08 | ST-03/04 | SA-16 glyph inventory/negative cases plus device text/source-target layout/play cancel/retention evidence |
| SU-12 | SC-01/02/03/09 | ST-05 | SA-17 RFC SHA algorithms/digits/period/leading-zero/boundary and URI rejection; SA-18 independent import permission/PIN/idle lock/untrusted time/reboot offline bounds |
| SU-13 | SC-09/10 | ST-05/06 | SA-19 fictional-seed delete/upgrade/save/wrong key/corrupt archive/recovery without original device and independent login |
| SU-14 | SC-10 plus each model | ST-01/02/05/06 | SA-20 actual Flash/heap/largest-block/stack peaks/admission; SA-21 local/server/WebDAV receipts, disk-full/power-cut/migration/empty-server recovery with ordinary/OTP separation |
| SU-15 | SC-10 | ST-06 | SA-22 complete gate and matching archive/hash; 100 exits/24 h mix; separately authorized device results and every V gap |

Evidence ledger per SA: baseline commit, approved decision versions, mock/real mode, fixture or sanitized log, command/environment, firmware full-image hash if applicable, actual observed result, U/D/C/V disposition, remaining limits. A real test that was not run stays NOT RUN, not PASS based on host mocks. Scanning one test QR does not validate all ticket symbologies or issuer entry rules.

## 5 Decisions that block dependent implementation

All Q rows are D/V, no implied user approval. They do not reopen cancelled scope. Resolve as a dated decision with chosen option, rationale, affected SU/SC/SA and verified limitations in both languages. Independent public-vector/model tests need not wait for real-service/security deployment choices.

| Decision | Concrete review needed | Blocking scope |
| --- | --- | --- |
| Q-01 | Actual computer OS/browser/adapter matrix; native BLE bridge or USB helper fallback, packaging/origin consent | ST-01 actual provisioning support; no phone fallback |
| Q-02 | Canonical commit/reveal/SAS transcript, identity enrollment/TOFU, approved crypto library/primitives/key storage, authenticated scope separation and threat review | ST-01 sensitive BLE/binding and ST-05 import/recovery; no credentials before review |
| Q-03 | WPA2/WPA3/open-network policy and supported SSID/password encodings, candidate/old-config test route | ST-01 Wi-Fi production connection |
| Q-04 | Signed-time operator/trust-root distribution/rotation, outage recovery, measured drift/sleep uncertainty and rollback policy | ST-01 first TLS, ST-05 trusted offline OTP; browser/SNTP hint alone insufficient |
| Q-05 | Measured Flash/RAM, font/license/dynamic glyph inventory, cache quotas, exact application partition/backend and power-safe migration/flash preservation | ST-01 persistent foundation and later persistent/cache/font/audio stages |
| Q-06 | Issuer static-code format/validity/symbology samples and scanner coverage; any dynamic issuer adapter requires its own approval | ST-02 code claims/SA-09; metadata work can proceed with fictional static fixtures |
| Q-07 | Real Nextcloud/Tasks version/auth method/selected lists, recurrence/subtask/shared/floating-date support; reopening remains unconfirmed with no active implementation | ST-02 real CalDAV acceptance and unsafe instance completion; creation excluded |
| Q-08 | Weather provider/API version/city ID/license/attribution and freshness policy | ST-03 real weather acceptance; scene math independent |
| Q-09 | Speech/translation/synthesis provider/API/region/streaming/billing/cancel/retention and user-audio privacy policy | ST-04 real adapter/user audio; mock tests independent |
| Q-10 | Account TOTP parameter set, PIN/KDF latency and physical protection, secure boot/Flash/NVS strategy without eFuse action, exact independent archive/key/restore/access specification | ST-05 vault security/recovery implementation; real seed use prohibited until separate review, public-vector tests independent |
| Q-11 | Website/server repository paths/framework versions, owner auth independent recovery, reliable store/backup queue/secret vault, deployment responsibility and CalDAV/WebDAV credential boundaries | ST-01 server/web persistence/auth, ST-02/04/05 service implementation; deployment still separately authorized |

No implementation stage is fully ready merely because its prompt exists. Do not treat absent providers, unsupported browser BLE, physical-security weaknesses or insufficient storage as hidden assumptions. Review Q-02/04/05/10 early; OTP is not forced to the end.

## 6 Current documentation validation

This round modifies documentation only. Run `./tools/validate.sh --static`, then independently check every new local link and matching SU/SC/ST/SA/Q identifiers, protocol fields and numeric limits between languages, including untracked documents. Record results below after execution. Existing host tests are baseline regressions; no suite firmware or real service tests were run. Build: NOT RUN. Device tests: NOT RUN (not applicable to this document-only change). No commit, push, deployment, flashing, erase or eFuse write; no real OTP seeds/service credentials.

Unverified: actual computer BLE/security/TLS/time; production partitions/resource budgets; all new UI/Chinese rendering; ticket reader/issuer rules; real Nextcloud/translation/weather; OTP physical protection/backup recovery; persistence/power cuts and integrated operation. Future firmware requires complete gate, matched artifacts and authorized device-test offer, as specified above.

Validation record, 2026-10-05: `./tools/validate.sh --static` PASS (repository/link/language/workflow checks and existing host tests). Independent checks of all six new files: local link destinations, matching SU/SC/ST/SA/Q occurrence counts, numeric-limit inventory, inline protocol/model field inventory, section counts and table row counts PASS. Prose was reviewed for aligned meaning; inventory checks do not prove semantic equivalence or real functionality. HEAD remains the specified baseline; no tracked source/configuration/CHANGELOG changes. Six new documents were added; the two existing asset indexes and two suite-plan files were edited only for navigation/clarification; historical travel/review files were untouched. Build: NOT RUN; Host tests: PASS (baseline regressions only); Device tests: NOT RUN.
