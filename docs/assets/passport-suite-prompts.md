[简体中文](passport-suite-prompts.zh_CN.md) · **English**

# Suite Implementation Tasks and Codex Review

## 1 Execution boundary and common prompt

Design dated 2026-10-05; status updated 2026-10-25, Asia/Shanghai. [Plan](passport-suite-plan.md#0-current-implementation-and-acceptance) records current scope, implementation and acceptance; [PRD](passport-suite-prd.md) fixes SU outcomes/pages; [contract](passport-suite-contract.md) specifies SC proposals. U/D/C/V meanings follow the PRD. The stages below retain the original design work order, not a claim that completed home/provisioning work is absent. Authorized Tasks increments now include reading, completion, offline cache/reboot replay and MiMo voice creation; this does not accept all ST-01/ST-03 gates or authorize the whole suite. The old travel D1-D9, acceptance IDs and storage/provisioning instructions must not be run.

**Completed-stage history.** OC-01 through OC-11 were executed and independently
reviewed; the prompts in sections 7.3-7.19 are retained as a dated record and must
not be rerun as current work. Continuation is now: recover the approved OC-11
artifact, following `passport-device-test` and checking the target, scope and
applicable existing authorization for that image, then resume the readiness diagnosis
from the last OC-11 attempt recorded in
[suite-plan section 0](passport-suite-plan.md#0-current-implementation-and-acceptance).
The installed older build has only a placeholder and cannot currently reach that path.
Do not restart the closed Tasks acceptance, the old WSL sequence, or defensive test
rounds.

Dated checkpoint, 2026-10-23: OC-07 diagnostics, two OC-05 repair passes and OC-08 were completed and reviewed. Exact `a353524e/dc655e73` passed 44 worker groups (default/optimized) and the complete gate, then its approved app-only write/table/device digest passed on 2026-10-23. The operator confirmed retained tasks, recording and cancel/re-entry; measured capture-cancel/short-stop endpoints were 117.741/19.503 ms, with one final per sequence. The measured subset is closed at the user's request to avoid excess defensive testing; COM3 is released. See the [measured record](passport-device-tasks.md#measured-voice-subset-and-closeout-2026-10-23). Serial ELF banner, new-image preview/remote-wait timing and true resource peaks remain unverified. Prior broader acceptance retains its original image identity. OC-09 (coherent Tasks milestone candidate) and OC-10 (translation T1 core/local mock) were then completed and independently reviewed. OC-11 now adds the launcher translation page, explicit direction controls, separate source/target panes, MOCK state, bounded worker over the actual core and synthetic provider; see [the T1 core checkpoint](passport-translation-t1-core.md). OC-11 is not complete translation and is not SA-15. The subsequent authorized real T1 integration now adds the MiMo-default capture/upload/poll worker, explicit MOCK mode, shared scratch/config/USB use and focused host tests; device, paid-provider and full-gate verification remain root-owned. The trusted user then approved MiMo candidate A, the disclosed retention uncertainty, an owned inactive-Flash plaintext WAV of at most 10 seconds plus one bounded upload, and real-provider/device/paid testing after reviewed integration. The account region is not independently established, so no regional guarantee is claimed. T2/TTS and SA-15 remain outstanding. DeepSeek production integration is rejected; `deepseek-flash` has no ASR and does not change the selected MiMo ASR+MT plan.

Prepend this to each newly authorized stage prompt:

```text
Implement only the explicitly authorized ST stage from passport-suite-prompts.
Read AGENTS.md and routed documents; check all five passport skills and use
only applicable skills. Start with git status --short --branch and rev-parse HEAD.
Preserve the active feature branch, user changes and existing untracked
application/tests/docs files. Inspect current changes before editing; do not
reset, clean or switch branches to match an old design baseline.
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

Proposed paths below describe design responsibilities, not a complete implemented suite. The implemented Tasks service uses Python `companion/tasks/`; [its integration guide](passport-nextcloud.md) records the approved deployment beside the provisioning website and subsequent creation repair/acceptance. Other web/server roots, owner authentication and production placement for the remaining services remain Q-11 decisions. Resolve nested AGENTS instructions first. Ordinary PRs leave both CHANGELOG files unchanged. A stage ends only when its dependent acceptance has evidence or a clearly reported blocked/unverified disposition, never when mocks merely compile.

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
Keep newer remote edits and old active cache. Implement SU-17 voice creation under SC-06.1 with SA-23; no reopen control.
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
| Q-07 | Updated 2026-10-21: Nextcloud 35.0.0/Tasks 0.18.1, private app password, one selected Tasks collection after Deck removal; reading/completion/cache/reboot replay and voice creation accepted; recurrence/subtask/shared/floating-date compatibility and reopening remain unverified | Remaining real CalDAV compatibility and unsafe instance completion; do not repeat completed connection work |
| Q-08 | Weather provider/API version/city ID/license/attribution and freshness policy | ST-03 real weather acceptance; scene math independent |
| Q-09 | MiMo pay-as-you-go ASR is selected and tested for voice-created Tasks; translation and synthesis provider/API/region/streaming/billing/cancel/retention remain undecided | ST-04 real adapter/user audio; Tasks ASR approval does not select the translation/TTS service; mock tests independent |
| Q-10 | Account TOTP parameter set, PIN/KDF latency and physical protection, secure boot/Flash/NVS strategy without eFuse action, exact independent archive/key/restore/access specification | ST-05 vault security/recovery implementation; real seed use prohibited until separate review, public-vector tests independent |
| Q-11 | Tasks read/completion/voice service deployed: `companion/tasks/`, Python 3.12.3/Gunicorn 26.2.0 behind existing Caddy, private CalDAV/speech config; owner auth/recovery, backups/secret vault and other services remain open | Remaining ST-01/02/04/05 services; future deployments/publication need their own scope authorization |

No implementation stage is fully ready merely because its prompt exists. Do not treat absent providers, unsupported browser BLE, physical-security weaknesses or insufficient storage as hidden assumptions. Review Q-02/04/05/10 early; OTP is not forced to the end.

## 6 Historical documentation validation (2026-10-05)

This section records only the original design delivery. Current Build, Host tests, Device tests and Unverified results are maintained in the plan's implementation checkpoint.

This round modifies documentation only. Run `./tools/validate.sh --static`, then independently check every new local link and matching SU/SC/ST/SA/Q identifiers, protocol fields and numeric limits between languages, including untracked documents. Record results below after execution. Existing host tests are baseline regressions; no suite firmware or real service tests were run. Build: NOT RUN. Device tests: NOT RUN (not applicable to this document-only change). No commit, push, deployment, flashing, erase or eFuse write; no real OTP seeds/service credentials.

Unverified: actual computer BLE/security/TLS/time; production partitions/resource budgets; all new UI/Chinese rendering; ticket reader/issuer rules; real Nextcloud/translation/weather; OTP physical protection/backup recovery; persistence/power cuts and integrated operation. Future firmware requires complete gate, matched artifacts and authorized device-test offer, as specified above.

Validation record, 2026-10-05: `./tools/validate.sh --static` PASS (repository/link/language/workflow checks and existing host tests). Independent checks of all six new files: local link destinations, matching SU/SC/ST/SA/Q occurrence counts, numeric-limit inventory, inline protocol/model field inventory, section counts and table row counts PASS. Prose was reviewed for aligned meaning; inventory checks do not prove semantic equivalence or real functionality. HEAD remains the specified baseline; no tracked source/configuration/CHANGELOG changes. Six new documents were added; the two existing asset indexes and two suite-plan files were edited only for navigation/clarification; historical travel/review files were untouched. Build: NOT RUN; Host tests: PASS (baseline regressions only); Device tests: NOT RUN.

## 7 OpenCode Go task prompts, 2026-10-21

### 7.1 Initial work recommendation and model selection (historical)

This recommendation was written on 2026-10-21 and is retained as history; do not
rerun these prompts. At that time it recommended OC-01 first, OC-02 only for
confirmed coverage gaps, then OC-03 to prepare the next feature. This ordering is an engineering recommendation, not
authorization to run all three. Keep the review bounded; do not turn every
remaining hardware measurement into a prerequisite for translation design.
Translation can reuse proven recording concepts. OTP public-vector work remains
independent; real-secret storage/import/recovery still needs its own decisions.
Wallet work needs concrete ticket samples and scanner acceptance.

| Prompt | Scope | Reviewable result |
| --- | --- | --- |
| OC-01 | Read-only reliability review of Tasks/voice | At most five evidenced defects, current coverage, smallest next patch or measurement |
| OC-02 | Selected synthetic regression tests only | A small test diff with independent expectations; production fixes reported separately |
| OC-03 | Translation provider/protocol implementation plan | Official-source comparison, reusable boundaries, phased plan and unresolved choices |

These are bounded assignments suitable for evaluating DeepSeek on this project;
they are not a claim that a particular model can certify hardware or security.
OpenCode Go is the development-model service. Do not use its coding subscription
as the device's deployed speech/translation backend.

The [official Go documentation](https://opencode.ai/v2/docs/console/go/) lists
DeepSeek V4.1 Flash, with model ID `deepseek-v4.1-flash`, checked 2026-10-21.
Confirm the provider/model identifier in the local CLI or `/models` before use:

```text
opencode models
opencode run --model opencode-go/deepseek-v4.1-flash --agent plan "<OC-01 or OC-03 prompt>"
```

Run from the repository root and paste OC-01 or OC-03 with the shared boundaries
below. CLI flags follow the [official CLI reference](https://docs.opencode.ai/docs/cli/).
Plan is a planning aid; shell permissions still follow local configuration,
as described in the [agent documentation](https://docs.opencode.ai/docs/agents/).
The prompt's action limits still apply. OC-02 needs an editing agent only when
that test work is explicitly selected. Do not add an auto-approval flag.
No model inference was invoked when preparing these prompts. The later execution
record is in section 7.6. These commands were checked against local CLI 2.0.32;
its `models` command lists providers together and accepts no provider positional
argument. Inspect that list for the exact Go model before running.

### 7.2 Shared boundaries for OC-01/02/03

Prepend this block to the selected prompt. These current-task boundaries replace
the historical ST-stage defaults for these three assignments.

```text
Work in the existing ai-passport checkout. Read AGENTS.md, the current acceptance
in docs/assets/passport-suite-plan and passport-device-tasks, and only documents
routed for this assignment. Check the five required passport skills and use only
matching ones. Start with git status --short --branch and git rev-parse HEAD.
Preserve the active branch and all existing tracked/untracked changes. Do not
reset, clean, stash, switch branches or overwrite others' work.

Use the codebase-memory skill for structural questions; verify coverage/freshness.
If unavailable, report that limitation and use bounded source reads/searches.
Do not treat old design proposals or passing mocks as actual implementation.

Accepted: Tasks list/details/refresh/cache/completion and normal-reboot replay;
MiMo recording, Chinese preview/default Cancel, post-recognition offline save,
normal reboot retention and same-intent retry creating exactly one real task.
Deck is removed and one Tasks collection is selected. Recognition needs Internet.
Installed full-image/ELF prefixes are 10f8cb21/d7654fa9; the later gate-only
4e5beb17/73a04243 pair was never flashed. See the acceptance record for full hashes.

Only perform the selected OC task. No real service writes, deployment, device
reset/flash/erase/eFuse, commit or push. Do not read private credentials, auth
files, personal task data or raw private build/log files. Use source, sanitized
acceptance documents and synthetic/public fixtures. Preserve Wi-Fi/city settings,
tokens, keys, cache and operation journals. Report in Simplified Chinese.

Distinguish evidence, inference and unverified measurements. Give commands and
actual outcomes; never invent test runs, resource measurements or device PASS.
Do not automatically proceed to another OC task after completing this one.
```

### 7.3 OC-01 Read-only reliability review

```text
Follow section 7.2 and perform OC-01 only. Do not modify repository files.
Goal: find concrete failures worth fixing before extending the audio workflow.

Inspect the relevant parts of main/suite_tasks_voice_impl.h, suite_task_voice.*,
suite_tasks.c, suite_task_ops.*, companion/tasks/{caldav,creation,speech}.py and
their existing tests. Follow actual includes/ownership rather than guessing APIs.
For audio lifecycle questions inspect BSP headers and implementation as routed.

Trace: confirmed intent -> local NVS commit -> server durable receipt ->
conditional Nextcloud write -> strict readback -> device success acknowledgement.
Check the same operation ID across connection loss, normal reboot, server
restart, lost write response and the success-to-local-ack window. Check same-ID
different-payload rejection and that retry does not repeat paid recognition.

Also check cancellation/page exit/late results, audio buffer ownership and
release before TLS, temporary Flash cleanup versus the active task cache,
authentication versus transient failures, paused-retry controls and queued
completion/creation isolation. Preserve If-Match/If-None-Match protections.
The valid empty REPORT creation-probe case is already fixed and tested;
verify its narrow scope without presenting it as a newly discovered defect.

Output at most five actionable findings, ordered by severity. Each needs an
exact file/line, trigger sequence, violated invariant, user impact, existing-test
coverage and smallest repair/test suggestion. Separate confirmed defects from
hypotheses. If no defect is established, say so; do not manufacture findings.
Finish with a small evidence table and a measurement plan for capture, TLS and
repeated page entry/exit, covering minimum free heap, largest internal/DMA block
and stack watermark. Linked sizes are not measured peak usage. Propose only the
smallest justified next step; do not run physical tests or change production code.
```

### 7.4 OC-02 Fill selected regression gaps

```text
Follow section 7.2 and perform OC-02 only. Use an OC-01 finding or independently
demonstrated coverage gap as the input. Add only the smallest relevant set of
synthetic regression cases; if the behavior is already covered, cite the tests
and avoid duplicate assertions. Do not change production firmware/service code.

Own only the necessary files among tests/test_companion_voice.py,
tests/test_companion_completions.py, tests/test_suite_task_voice.c and
tests/test_suite_tasks_runtime.c. Any additional file needs an explained reason
and scope review first. You are not alone in this checkout: preserve others'
edits and adapt to their changes. Do not rewrite the test framework.

Use independently specified expected outcomes and actual transport/worker paths.
Relevant gaps may include interrupted commit/restart, a server receipt lost in
transit, success before local acknowledgement, duplicate ID with different
payload, cleanup failure preserving cache, and late results after page exit.
Choose cases justified by evidence, not a quota. Mock external I/O only; do not
replace the state machine under test. Never weaken production safety checks to
make a test pass or count a synthetic power cut as a physical-device result.

Run focused tests, then ./tools/validate.sh --static in the supported environment.
If a new test reveals a production defect, retain the minimal failing regression
and report it clearly for a separate fix; do not silently mark the gate PASS.
Deliver changed files, the failure each case detects, commands/results, and any
remaining gap. Build: NOT RUN and Device tests: NOT RUN for this test-only task;
report Host tests with its actual result. Do not deploy, call paid APIs or flash.
```

### 7.5 OC-03 Plan the translation increment

```text
Follow section 7.2 and perform OC-03 only. Deliver an implementation-ready plan
in the response; do not edit source/configuration or deploy anything.
Read the existing SU-10/11, SC-08, ST-04, SA-14/15/16 and Q-09 instead of
redesigning the whole suite. Target Chinese <-> English with manual recording,
separate source/translation text, cancellable playback and Simplified Chinese UI.

Identify what can be reused from the accepted voice-task capture/upload path and
what must stay separate: task creation/completion tokens and journals, cache
slots, translation jobs and temporary playback audio. Identify actual boundaries
by source references. Do not copy the whole Tasks worker or change partitions
just to obtain an abstraction. Provide a budget for ESP32-C3/8 MB/no PSRAM and
explicit measurement points; label all unmeasured limits as proposals.

Using current official API documentation only, compare at most three practical
ASR + translation + TTS combinations, including MiMo and a DeepSeek text-model
option where supported. Cite model/endpoint, both language directions, formats,
limits, streaming/cancellation, billing and retention policy. Mark unsupported
or undocumented capabilities honestly. MiMo Tasks ASR approval does not select
translation/TTS; OpenCode Go model availability is not a production API contract.
No paid call or real audio is authorized for this planning task.

Present one recommended combination and the smallest set of decisions requiring
the user. Map the existing contract's streaming proposal against the current
bounded Flash recording path; explain any proposed protocol change instead of
silently treating it as approved. Preserve task cache/journals and settings.

Provide a phased plan: T1 records/transcribes/translates and displays text;
T2 adds synthesis/playback/cancellation. Keep TTS in the full requirement and do
not claim T1 alone passes SA-15. Include the state/event transition table,
request/response examples, authentication scope, memory ownership, timeout and
late-result handling, bounded text/audio, retention and failure messages.
End with file ownership, focused host-test oracles, real-device acceptance steps,
and the smallest first implementation prompt after the provider/protocol choices
are settled. Keep OTP/wallet work outside this assignment.
```

Preparation validation, 2026-10-21: repository/link/language checks PASS. The
complete static gate was NOT RUN because WSL returned CreateVm/ERROR_TIMEOUT
before test startup. This increment changes prompts only; no firmware change,
model inference, deployment or device test was performed.


### 7.6 Execution and review checkpoint, 2026-10-22

The user authorized OC-01/02/03. All three sessions used the Go provider,
`deepseek-v4.1-flash`, default variant. Codex reviewed the results, reproduced
selected red tests, and returned scoped production fixes to the same OC-02
session. On 2026-10-22 an independent WSL-native checkout matched all 18 final
candidate hashes, passed the final 28-group default/optimized tests and complete
`./tools/validate.sh`, with 64 companion cases and zero skips. Full-image/ELF
prefixes are `7c292cfc/e8b6ae41`; the archive independently verified. There was
no new failure requiring a repair, and both original working copies were
preserved. See the [review and validation ledger](passport-reliability-review.md)
for exact identities, paths and evidence.

The [translation proposal](passport-translation-plan.md) includes the corrected
lock/admission/retention boundaries; T1/T2 implementation and real API calls
remain unapproved. The user-approved candidate was flashed on COM3 in three independently verified
segments. The 90-second startup, saved Wi-Fi and HTTPS weather checks passed; the
port was released. The operator confirmed retained tasks and six latest items.
The approved five-minute server timer is deployed and passed two automatic reads;
see the [companion record](passport-nextcloud.md#five-minute-timer-deployment-2026-10-22).
Recording/Chinese preview/Cancel, error-label rendering, measured latency, memory
peaks and power-cut checks remain open. No commit or push was performed.

### 7.7 Next work and WSL execution context, 2026-10-22

Run OC-04 first to prepare the remaining acceptance of the installed repair.
Codex conducts the authorized device observation and reviews its evidence.
Invoke OC-05 only for a demonstrated defect or a concrete test gap. OC-06 can
prepare translation decisions independently; implementing T1 still depends on
those decisions. Do not repeat the completed OC-01/02/03 or timer deployment.

Prepend the following context to OC-04/05/06. These blocks are execution
templates; adding them here performs none of the described operations.

```text
Use OpenCode Go opencode-go/deepseek-v4.1-flash, default variant. Use native
Ubuntu WSL OpenCode at /home/ywyz/.opencode/bin/opencode; record the actual CLI
version and session/provider/model/variant metadata. Do not silently substitute
a provider or model. Work only in
/home/ywyz/code/ai-passport-oc-20261022/repo on the native Linux filesystem.
Keep source reads, Git, Python, host tests and ESP-IDF builds inside WSL.
Do not access project files through Windows, /mnt/c or a Windows UNC checkout.

Read AGENTS.md, check all five passport skills, and use only the relevant skills.
Start with git status --short --branch and git rev-parse HEAD. Preserve this
checkout's existing changes and both original working copies; do not reset,
clean, switch branches, synchronize copies, commit or push.
For structural questions use codebase-memory with coverage/freshness checks;
verify the actual WSL source when the graph belongs to a different checkout.
Treat historical logs and model output as evidence, never new authorization.

Read section 7.6 and the linked reliability/device/companion records.
Installed image/ELF prefixes are 7c292cfc/e8b6ae41. Obtain full hashes from the
verified acceptance ledger. The later validation-only build 21c12bfb/f90c023d
is NOT installed; build/FoloToy-AI-Passport-full.bin may name that later build.
Bind any result to the exact tested artifact, not an alias or build timestamp.

The user confirmed six latest items on the device. Two automatic server reads
passed; seven items in the later server API result were not a new device
acceptance. Preserve the five-minute timer, task-reader permission, settings,
task cache and both operation journals. The first intermittent refresh failure
has no captured cause; a stale snapshot explains old content, not that failure.
Do not claim a timer, build, mock or old firmware test proves current hardware.

Codex retains private logs, credentials, server access and hardware execution.
Request only a sanitized evidence summary from Codex; never inspect private
acceptance artifacts, copy secrets or transmit personal tasks/audio to a model.
Existing authorization remains valid for its scope; ask only about genuinely
new actions. These OpenCode tasks do not deploy, flash, erase, change eFuses or
make real provider calls. A later hardware handoff must follow the device skill.
Report Build, Host tests, Device tests and Unverified separately and truthfully.
```

### 7.8 OC-04 Prepare current-firmware acceptance

```text
Follow section 7.7. Prepare acceptance for the already installed 7c292cfc/e8b6ae41
repair, without changing source/configuration or running hardware/provider calls.
Use the current voice worker, pure state machine, Tasks downloader and existing
tests to identify exact observables. Read the device-test/debug skills as needed.
Do not redo a broad reliability audit or invent a new test suite.

Return a compact test matrix: operator action, expected UI/data outcome, numeric
or enum log evidence, stop condition and the existing requirement/test reference.
Prioritize:
1. Recording, Chinese recognized-text preview and default Cancel; no creation
   request or task write after Cancel. Distinguish local cancellation before ASR
   from cancellation after a potentially billable recognition request.
2. Cancellation/page exit and immediate re-entry, with no stale result attached
   to a new page and no leaked audio/network owner. Identify the implemented
   cancellation points rather than assuming all stages have the same controls.
3. Manual refresh after the server's scheduled publication, then offline refresh
   retaining the active cache. Do not intentionally alter the real task source.
   A passive capture is bounded to 120 seconds and must not reset the device.
   Separate a source-sync delay, a network/admission failure and cache activation.
4. Repeat entry/exit and the implemented error labels. Auth/quota/malformed/late
   responses use synthetic host I/O; do not revoke real tokens, spend quota to
   exhaust it or damage production data merely to manufacture an error screen.

For runtime measurements list the existing log fields and unavailable ones:
elapsed time, minimum internal free heap, largest internal/DMA block and stack
watermark around capture, TLS and cleanup. If existing logs only provide samples,
say so. Propose minimal instrumentation separately; do not add or flash it now.
Use fictional spoken content if an authorized later device test needs audio;
keep provider/billing authorization separate from this planning task.

Codex owns serial discovery, operator coordination and private evidence.
Provide a sanitized logging whitelist, a bounded observation sequence and a
PASS/FAIL/NOT RUN checklist. No reflash is needed to test the installed image.
Leave destructive power-cut testing as a separate case needing a concrete
data-impact review. If the intermittent failure does not recur, report
"not reproduced in this observation", preserve the limitation and do not
block independent work indefinitely or claim its root cause is fixed.

Finish with the smallest next action: execute the acceptance checklist, send a
specific evidence-backed issue to OC-05, or name the precise missing observation.
Device tests remain NOT RUN for your planning response.
```

### 7.9 OC-05 Repair a demonstrated failure and add its regression

```text
Follow section 7.7. Codex supplies one reviewed issue with a sanitized reproducer,
expected invariant, observed failure, exact firmware/source identity and approved
file ownership. If these are absent, return the missing evidence; do not invent
a production fix from the old refresh-failure message alone.

Implement only that issue in the assigned files and the smallest meaningful
regression. You are not alone in this checkout: preserve and accommodate others'
changes. Trace the actual worker/transport/state path; mock external I/O only.
First show the regression fails for the stated reason on the current candidate,
then repair it and show it passes. Reuse existing coverage when it already
detects the failure; do not add tests that merely mirror implementation details.

Preserve cache activation, settings/journal formats, operation IDs, conditional
CalDAV writes, authentication boundaries, paid-stage deduplication, LVGL locking
and non-blocking callbacks. Do not weaken parsing or add automatic paid retries.
Keep timer/provider/partition/BSP changes outside this task unless the reviewed
issue specifically requires and authorizes their scope.

Run focused tests, then ./tools/validate.sh --static. For a firmware change,
activate ESP-IDF 5.5.3 and run the complete ./tools/validate.sh. Verify the exact
image/ELF archive and report file changes, red/green evidence, gate results and
remaining uncertainty. Do not count a build or synthetic fault as device PASS.
Return the patch to Codex for independent review. Further proven failures go
through the same scoped repair loop; a clear environmental or evidence blocker
must include the failing command, sanitized error and smallest needed action.

A new build is not the currently installed/previously approved image. Codex
handles the firmware-specific device handoff after review; never flash a merged
image or erase the chip automatically. Keep commits, push and deployment out.
```

### 7.10 OC-06 Close translation decisions before T1

```text
Follow section 7.7. Read passport-translation-plan and its SU-10/11, SC-08,
Q-09 and SA-14/15/16 references. This is a decision packet, not a new OC-03
comparison or permission to implement T1/T2. Do not edit source/configuration,
deploy, send real audio or call paid APIs.

Return one recommended T1 bundle and only the decisions still needing the user:
- Pin the production ASR/text provider, model, endpoint and account region.
  Reuse the existing proposal where supported. Recheck material API capabilities
  and API-specific retention/deletion/training terms in current official sources,
  with dated links; mark unknown account terms. Do not infer API terms from a
  consumer-chat setting or confuse OpenCode Go with a production provider.
- State explicitly whether temporary device Flash audio and one bounded WAV
  PUT replace SC-08 volatile-only/chunked capture. Summarize data lifetime,
  cancellation/restart cleanup and deduplication consequences. These exceptions
  remain proposals until chosen. Leave TTS retention/voice choices to T2 unless
  they change a T1 interface; do not imply TTS or SA-15 is already complete.
- Identify real provider-call approval separately from implementation using
  synthetic providers. Keep the distinct translation capability, preserved
  Tasks/settings, scratch ownership and no partition resizing as requirements.

Do not redesign the reviewed job protocol or repeat all provider prices.
List only changes required by new verified evidence. Supply an exact approved-
decision checklist and a compact T1 file/test ownership plan linked to the
proposal, including the shared scratch/audio/network lock order, worker exit
acknowledgement, late-result rejection and Chinese glyph handling.
Treat resource limits as proposed until measured on ESP32-C3 without PSRAM.

End with a ready-to-use T1 implementation prompt that cites the specific chosen
decision version. Keep provider adapters synthetic until real calls are
authorized; require focused regressions and the complete gate. T2, deployment,
flashing and publishing remain outside that coding prompt. If a required choice
is unresolved, block only its dependent implementation and name the choice.
```


### 7.11 OC-04/05/06 execution and current-device review, 2026-10-22

All three prompts ran with native CLI 2.0.22 and exported metadata verified as
`opencode-go/deepseek-v4.1-flash`, default variant. Session IDs are in the
[review ledger](passport-reliability-review.md#oc-040506-follow-through-2026-10-22).
OC-04's checklist was corrected against source: offline/admission early returns
emit no download logs, and absence of logs is not a failure verdict. Existing
log prefixes have timestamps; voice start/stage/minimum-heap/DMA telemetry does
not exist. Codex performed the hardware/private-data work, not OpenCode.

OC-05 first returned the missing defect evidence, then implemented one reviewed
coverage gap in `tests/test_suite_tasks_runtime.c` only. An actual-worker preflight
case now detects a missing target-required message mapping and asserts no
microphone/ASR/intent writes plus cache/owner cleanup. Codex independently ran
29 groups in default and `-O2`, and four isolated old/new mutation combinations:
old tests missed the mutant, new tests detected exactly the intended failure.
No production repair was necessary; no new failed regression remains.

The complete gate passed in ESP-IDF 5.5.3, including 64 companion cases with zero
skips. The verified validation-only image/ELF prefixes are `8e5bfcdf/221c9998`;
full hashes and bundle are in the review ledger. That new build was not flashed.
The installed device remains `7c292cfc/e8b6ae41`, confirmed by archive and boot ELF.

Operator acceptance passed recording cancel/immediate re-entry, Chinese preview
and default Cancel, clearing the preview by leaving, approximately ten navigation
cycles, seven displayed tasks, offline cache and online recovery/Refresh. Cancel
closes confirmation and retains preview; it is not a creation action. Read-only
server checks preserved config/selection and complete creation/completion hashes;
the five-minute timer remains active. USB unplug/reconnection was operator-
confirmed; its observed USB reset/startup is recorded without claiming reset-free
measurement. See [device acceptance](passport-device-tasks.md#oc-04-current-firmware-acceptance-2026-10-22).

OC-06's [reviewed decision packet](passport-translation-plan.md#oc-06-decision-packet-2026-10-22)
keeps provider/account and SC-08 exceptions pending, corrects unsupported wear and
credential requirements, and preserves deduplication evidence after result expiry.
No translation implementation or real translation provider call is authorized by
executing this planning prompt.

Build: PASS. Host tests: PASS. Device tests: PASS for the recorded subset only.
Unverified: recognition-wait cancel, physical error labels, voice latency/minimum
heap/DMA peaks, physical write-time power cuts, long-duration/mixed use and the
first intermittent refresh failure's root cause. Resource/timing acceptance has
a precise evidence blocker: installed firmware lacks the needed telemetry; a
separately reviewed measurement increment is the next dependent step. No original
copy synchronization, commit, push, deployment, firmware write or whole-chip erase.


### 7.12 Next increment: measure the accepted Tasks path, 2026-10-22

Run OC-07 first, when execution is authorized, to implement minimal diagnostics
and regression coverage. Codex reviews the patch and exact bundle; OC-08 then
prepares a measurement checklist from the implemented field contract. Codex owns
the hardware execution and any firmware-specific flash handoff. Writing these
prompts does not launch OpenCode, implement diagnostics, approve a new firmware
flash or make provider calls.

The seven-task display, capture cancel/re-entry, Chinese preview/default Cancel,
preview exit cleanup, navigation and offline/online Refresh already passed in
7.11. The next uncertainty is measured voice timing/resource headroom, which also
informs the proposed translation budget. This is not a new production defect or
permission to redesign the accepted path. Full power-cut/24-hour/ST-06 acceptance
remains separate; do not make every unimplemented suite stage a prerequisite for
independent, explicitly requested translation core/mock tests.

For OC-07/08 retain the model, native-WSL, preservation, skill, graph-freshness and
privacy boundaries of 7.7, with the following current facts taking precedence:

```text
Use native /home/ywyz/.opencode/bin/opencode with
opencode-go/deepseek-v4.1-flash, default variant; record the actual CLI and
exported session/provider/model/variant. Do not substitute a model/provider.
Work only in /home/ywyz/code/ai-passport-oc-20261022/repo. All project files,
source reads, Git, builds/tests and evidence stay on the native WSL filesystem.
Preserve both original copies and all existing modifications; no synchronization,
reset, clean, branch switch, commit or push. Read AGENTS.md, check all five
passport skills and apply only the relevant ones. For structural queries use
codebase-memory and check coverage/freshness; a Windows index is not proof about
this WSL source. Read 7.11 and the linked reviewed device/reliability records.

Installed image/ELF remains 7c292cfc/e8b6ae41; obtain full hashes from the ledger.
The newest validation-only pair is 8e5bfcdf/221c9998 and is NOT installed.
The merged output alias may name that build. Seven tasks are now operator-
confirmed device evidence; the older six-task limitation in 7.7 is historical.
Preserve device settings, active cache, both operation journals, reader/write
capabilities and the five-minute timer. No new task, completion, journal clear,
provider selection, partition migration, deployment or full-chip erase.

Codex owns USB/serial access, credentials and private evidence. Only hardware
access may need a host bridge; project files/log storage remain WSL-native.
OpenCode receives sanitized numeric/enum summaries only and must not inspect
private acceptance artifacts, personal tasks/audio, keys or server credentials.
A reconnect window previously contained USB_UART_CHIP_RESET; do not assume that
opening a monitor is reset-free. Existing authorization remains valid only for
its actual scope. A new build is not an earlier approved flash artifact.
Report Build, Host tests, Device tests and Unverified separately.
```

Translation remains `T1-DP-2026-10-22-A`: its implementation request, account/real-
call choices and SC-08 capture exceptions remain distinct. Reuse the existing
[decision packet](passport-translation-plan.md#oc-06-decision-packet-2026-10-22)
instead of commissioning another broad provider comparison.

### 7.13 OC-07 Minimal numeric diagnostics and meaningful tests

```text
Follow 7.12. Implement only the measurement increment for the accepted Tasks
voice/download paths. This coding prompt does not authorize hardware/provider
calls, deployment or flashing. You are not alone: preserve and accommodate
existing edits. Do not refactor business behavior to accommodate logging.

OWNERSHIP: main/suite_tasks_voice_impl.h, main/suite_tasks.c,
tests/test_suite_tasks_runtime.c, and only needed declarations/constants in
tests/weather_stubs/weather_test_stubs.h. Prefer local fixed-size helpers in
that scope. Explain any additional file before expanding ownership; do not touch
BSP, fonts, partition/config, task formats, companion/timer or IDF sources.
Read the actual worker/command/HTTP/cleanup paths and existing 29-group tests.

Define a versioned numeric event contract with a boot-local sequence, reached-
stage/result enums and valid/present markers. It must correlate one accepted
recording attempt from command acceptance through worker start, preflight,
audio capture/release, transcription upload/response and cleanup. Measure the
existing stop/cancel flag request time, capture-end time, audio-release completion
and worker-cleanup completion separately. A request is not proof the microphone
has stopped. Preserve the distinction between microphone stop and waiting for a
potentially billable remote request. A stage not reached is absent, not zero-ms.
No URL, task/collection text, audio, bearer token or business operation ID.

Commands 1/2/4 must retain existing non-blocking behavior. Recording a timestamp
in input context must not allocate, log, walk heaps or wait on a worker. Protect
shared sequence/timestamps with an ESP32-C3-safe bounded mechanism; do not assume
64-bit atomics are free/available. Preserve cancel flag behavior even if its
secondary command cannot enqueue, and prevent an old attempt's markers from
being associated with a later attempt. Finalize each accepted attempt exactly
once, including pre-worker cancel, unready journal, offline/admission rejection,
allocation/audio/transport/cleanup failure and ordinary preview. Rejected starts
must not manufacture an accepted operation or mutate state.

At a small bounded set of worker checkpoints, collect internal 8-bit free bytes,
largest internal block, free/largest DMA block, and the worker stack watermark.
Place useful checkpoints before/after audio release, around real TLS stages while
the client is alive, and after cleanup. Preserve early download-return evidence
(offline/time/auth/admission/storage) without inventing an HTTP attempt. Query
capability masks separately; internal and DMA sets overlap and are not additive.
Verify the target ESP-IDF stack-watermark unit and log it explicitly.

Do not add a full clip buffer, dynamic diagnostic allocation, new permanent task,
periodic heap walker or per-512-byte log. Bound records/log volume (propose at
most 16 fixed-format lines per attempt); report added fixed RAM, stack/log and
measurement overhead. Collect measurements in workers, not under the LVGL or
input callback lock. Preserve the existing 45-second HTTP absolute request budget
and 10-second ordinary-stage caps, paid-stage behavior, cache activation, LVGL
locking, cleanup/quarantine and all settings/journal formats.

Measurement semantics matter. In pinned ESP-IDF 5.5.3, inspect both the heap API
header and components/heap/heap_caps.c. The local minimum-monitor start function
allocates a snapshot and asserts on allocation failure; repeated start also
resets global monitoring values rather than providing a safe private nested
interval. Do NOT add heap_caps_monitor_local_minimum_free_size_start/stop in this
increment or patch IDF to work around it. If using
heap_caps_get_minimum_free_size, label it boot_lowwater_sum: it sums per-region
low watermarks that may occur at different times. Stage samples/sample minima
are observed values, not exact operation-local or instantaneous worst-case
largest-block minima. Keep any remaining peak uncertainty explicit; no new
threshold, allocation strategy or automatic admission policy is authorized.

Test actual worker/command/transport paths with external-I/O fakes only. Extend
existing cases rather than duplicating all 29 groups. Use independent fake-clock
and I/O boundary expectations for: ordered markers and exactly one final result;
pre-worker/capture/post-response cancellation; deadline and cleanup-failure
outcomes; early no-HTTP returns; no stale sequence; bounded log count and absence
of injected secret/text sentinels. Preserve existing cache/journal/owner oracles.
Show the new diagnostic expectations fail on the pre-instrumentation candidate
for the stated missing-evidence reason and pass after implementation; that red
is not proof of a pre-existing functional defect. Reuse a test that already
catches a regression. Never disable safety checks or add automatic paid retries.

Run focused default/-O2 tests, ./tools/validate.sh --static, then the complete
./tools/validate.sh in ESP-IDF 5.5.3. Independently verify the exact firmware
archive. Deliver the field/units/scope contract, patch, red/green results, fixed
resource/log overhead, full image/ELF hashes and bundle, and remaining measurement
limits. Device tests: NOT RUN in this coding task. Return to Codex for review;
no flash, erase, eFuse, commit, push, deployment or real provider call.
```

### 7.14 OC-08 Prepare and review the diagnostic-device measurements

```text
Follow 7.12. Input is Codex's reviewed OC-07 patch, exact verified archive and
implemented event/units contract. If any is missing, identify it; do not invent
a field or count a plan as implementation. This OpenCode task is read-only: no
source/config edits, hardware, credentials, private logs, provider calls or flash.
Codex carries out the separately authorized hardware actions and supplies only
sanitized event summaries/operator observations.

First give a compact field-coverage table: each timing/resource question, its
actual source field, statistical scope, required operator observation and
remaining gap. Distinguish boot low-water sums, stage sample minima, true maxima,
stack watermarks, API-request budgets and UI/microphone/worker completion times.
Compare observations to SC-10's proposed 40 KiB internal/16 KiB largest-block
margin without treating proposals as approved thresholds or silently lowering
them. Do not apply the translation TTS two-second stop target to Tasks recording.
Name an undecided limit or unavailable metric instead of declaring all PASS.

Give Codex this bounded execution sequence:
1. Verify installed artifact, target port and data-preservation evidence. OC-07
   produces a new firmware identity. Before a write, bind the exact approved
   archive to the current table/sector erase ranges, excluding every settings,
   key, cache and journal partition. The existing installed-image approval does
   not approve a different build. Use any existing consent that actually covers
   the new artifact/scope; otherwise obtain that specific approval after review.
   No merged write across data regions, whole-chip erase or readback prerequisite.
2. Establish the serial method and buffered-output/reception markers before
   measuring. Prepare the operator before each <=120-second observation window.
   Record boot/reset evidence and firmware identity; do not join stages across
   a reboot or old UART buffer. Opening with DTR/RTS settings alone is not proof
   of no reset. If a reset occurs, record it and stop the reset-sensitive case;
   resolve the serial method before another timing attempt. Do not force a
   reset/power cut or retry indefinitely to get a cleaner trace.
3. On the instrumented image, use a small set of fictional-input cases tied to
   existing behavior: one preview/default Cancel, capture stop/cancel and quick
   re-entry, recognition-wait cancel, offline no-HTTP Refresh and online TLS
   recovery/Refresh. Recheck only what the instrumentation needs; reuse prior
   seven-task/navigation evidence with its exact original artifact identity.
   No task creation, completion, acknowledgement/discard of real journals, token
   revocation or quota exhaustion. Ask the operator to restore a disabled AP;
   never delete/rewrite device Wi-Fi configuration.
4. Recognition-wait cancellation may already have admitted a billable call.
   Check existing Tasks ASR authorization and budget for that specific case;
   do not request consent again if already covered, or expand it to translation.
   Plan at most two billable recognition attempts (preview and wait-cancel),
   stop on the first ambiguous error, and do not resend automatically. HTTP-
   opened/uploaded flags do not establish server admission, billing or refund.
5. Associate only matching firmware/sequence events. Measure stop/cancel request
   to capture end, audio release and cleanup separately; report stages skipped
   and any timeout/late-result rejection. Check owner release and that a later
   action works. For resource headroom report the declared statistics and
   instrumentation overhead; unmeasured peaks stay unverified. Do not create an
   always-on soak or destructive power-cut experiment as part of this sequence.
6. Release the port and retain private raw logs locally. Return only whitelisted
   numbers/enums, exact tested identity, operator state observations and a
   PASS/FAIL/NOT RUN table per case. A missing event means insufficient evidence,
   not automatically a device failure. A reboot/timeout abort is not a measured
   successful cancel. Keep provider/heap/stack uncertainty separate from defects.

Review sanitized results when Codex provides them. For a demonstrated defect,
return one scoped OC-05 handoff: exact image/source, minimal reproducer, expected
invariant, actual enum/timing evidence, existing-test gap and proposed ownership.
Codex selects the repair scope; do not change code in this task. For a coverage-
only gap, use a justified new test/mutation oracle rather than inventing a red
production bug. After review, repeat only checks affected by the repair.

Finish with the smallest next action: accept the measured subset, hand one proven
issue to OC-05, or state the precise missing observation/decision. The original
intermittent Refresh failure remains not reproduced unless new evidence appears;
do not declare its root cause fixed or block unrelated requested work forever.
Keep physical error rendering, write-time power cuts, 24-hour/mixed operation and
translation choices separately open. Until actual evidence is supplied, report
Build/Host tests from the exact input ledger and Device tests: NOT RUN here.
```


### 7.15 Git checkpoint and next work, 2026-10-23

Read-only inspection found no new local commit: the native validation checkout
is detached at `21c5110548e329019b2418ded506db68fe991418`, with an empty index.
GitHub's `ywyz/ai-passport:feature/feature10-ble-provisioning` head was independently
queried and is the same commit. The Tasks work through OC-08 is therefore still
uncommitted and unpushed. Local `origin` points to the Windows source copy at
`/mnt/c/Users/yw980/code/ai-passport`; it is not a GitHub publishing destination.
No remote, ref, index or commit was changed during this inspection.

There are 298 changed tracked paths, but only 29 retain content differences when
CRLF-only differences are ignored. Important Tasks firmware, companion, tests,
fonts and evidence documents are also untracked. Do not infer a committable unit
from only OC-07's four-file delta: it depends on the earlier uncommitted Tasks
implementation. Preserve unrelated content and line endings in the active copy;
exclude unrelated/line-ending-only differences in an isolated native candidate.
Existing changelog differences are not part of this ordinary milestone.

Next order: prepare a coherent Tasks commit candidate (OC-09), have Codex review
it, then perform Git writes only when requested. Move feature work to translation
T1 core/mock (OC-10). Tasks is usable and its measured subset is closed at the
operator's request; its remaining peak/remote-wait observations do not block the
independent translation work or justify more device test rounds.

The following are prepared prompts, not executed actions. Continue using native
OpenCode Go `opencode-go/deepseek-v4.1-flash`, default variant, with Codex review.
The existing `T1-DP-2026-10-22-A` packet remains a proposal: provider/account terms,
temporary plaintext WAV in inactive Flash and a single bounded WAV PUT are not
selected by these prompts. Real translation calls require their own authorization;
TTS remains T2.

### 7.16 OC-09 Prepare a coherent Tasks milestone commit candidate

```text
Prepare the Tasks milestone for Codex's commit review. This prompt authorizes
candidate preparation only, not commit, push, PR, deployment, flash or release.

Work in /home/ywyz/code/ai-passport-oc-20261022/repo and put candidate artifacts
in a sibling native WSL directory. Read AGENTS.md, commit-and-pr.md and the
latest Tasks acceptance/reliability checkpoints. Start with status, HEAD, index,
remotes and a fresh read-only GitHub branch-head check; preserve existing changes.
The current checkout is detached at21c5110 and origin is a Windows local copy.
Resolve the actual ywyz/ai-passport publishing destination without pushing to or
modifying that local origin. Propose a dedicated codex/ branch and target branch;
do not change the active checkout's refs, remotes or index in this preparation.

Separate real content changes from CRLF-only differences. Inventory necessary
tracked edits and untracked Tasks firmware/companion/font/test/tool/document
files. Include dependencies needed to build the whole accepted Tasks milestone;
do not package only the OC07 four-file delta. Prepare an isolated native candidate
against the verified base, preserving meaningful existing source bytes. Exclude
unrelated changes, changelogs, local helper/build artifacts, __pycache__, firmware,
private logs, credentials, device configuration and task/audio data. Do not
clean, reset or normalize the active copy. Retain public dependency/license files.

Deliver a concrete candidate file list and diff, exclusions with reasons, and
one coherent Conventional Commit proposal (split only if the pieces build and
review independently). Report the candidate tree/source hashes and any difference
from the installed a353524e/dc655e73 source. Update stale authoritative milestone
status in aligned English/Chinese documents; keep the observed-device subset and
unverified metrics distinct. Do not start translation in this task.

Validate the actual isolated candidate, not the dirty original: use focused
checks while preparing, then one complete ESP-IDF5.5.3 gate and immutable archive
verification after its final non-document bytes are fixed. This catches missing
untracked dependencies; do not repeat device, paid-ASR or defensive fault rounds.
Deliver exact image/ELF, test results, proposed commit title/body, target remote/
branch and a later explicit push command for Codex review. Report Build, Host
tests, Device tests and Unverified; a rebuilt image has no automatic device PASS.
Stop with a reviewable candidate. Do not execute any Git publishing action.
```

### 7.17 OC-10 Implement translation T1 core and local mock path

```text
Implement only the provider-independent T1 core/local mock subset from
T1-DP-2026-10-22-A. Use the reviewed packet; do not repeat OC03/06 vendor planning.
This is a coding prompt for a future authorized run, not approval of provider,
account terms, Flash/protocol exceptions or real translation calls.

Stay in the chosen native WSL workspace. Read AGENTS.md and routed development
context, verify all five passport skills, use passport-develop/build as applicable
and codebase-memory with coverage checks/source fallback for structural queries.
Preserve the accepted Tasks milestone, configuration, cache/journals, tokens,
five-minute timer and partition layout. Do not require further Tasks measurements
or repeat its reliability audit before independent translation work.

Own new main/suite_translation_core.* and isolated companion translation core/
mock modules, their minimal build/test registration and focused host tests. Use
explicit zh-CN<->en direction, separate source/target text, bounded states/events,
operation/job identity, matching replay versus payload conflict, cancellation,
generation/late-result rejection and bounded metadata. Keep a distinct translation
capability interface; do not reuse Tasks reader/writer/admin credentials or data.
Exercise the local job path with fictional synthetic providers and audio only.

Do not add hardware recording/upload, shared scratch/Tasks ownership changes,
production credentials or real adapters in this subset. Those paths depend on
the unresolved decisions. Do not silently choose Flash WAV or change SC08.
Do not implement T2 synthesis/playback, OTP, wallet, deployment or real calls.

Test the changed success/cancel/identity/replay/limit behavior through real core/
job paths with external-I/O fakes. Reuse existing test helpers; keep checks tied
to the change, without new Tasks soak/power-cut/device rounds. Run the relevant
focused tests, then the required complete gate once final code is fixed; verify
the archive and matching ELF. Record exact validation and remaining dependencies
in aligned English/Chinese docs. T1 core/mock is not a complete translation feature
or SA15 acceptance. Return the patch for Codex review with Build/Host tests/Device
tests/Unverified separately. No commit, push, deploy, flash or paid provider call.
```

### 7.18 OC-11 Implement translation page and bounded worker mock

```text
Implement the recommended in-firmware translation UI/worker increment in the
chosen native WSL checkout. OC-09 Tasks candidate and OC-10 T1 core/local mock
were already completed and reviewed; do not redo them. Read the T1 core
checkpoint, translation plan T1-DP-2026-10-22-A, actual core headers/sources,
suite task UI/runtime patterns, suite_app navigation, launcher/CMake, font
guidance and docs. Preserve all existing changes, settings, task data/cache/
journal/token, partitions and the five-minute timer.

Add a redesigned launcher translation page with explicit zh-CN/en direction
controls, separate source and target panes, a visible MOCK indicator, processing,
failure, cancellation and expired-result states, and intuitive three-button
navigation including long-text scrolling and exit. Do not merely relabel the stub.
Connect the actual suite_translation_core to a bounded worker using synthetic
input and a replaceable synthetic provider boundary; keep the later real-provider
adapter possible without changing UI/cancellation/generation logic.

UI callbacks dispatch only. The worker must not hold LVGL objects or serialize
snapshots incoherently. Use worker ownership and session/generation checks so
cancel, exit and reentry reject late results, and a cleanup acknowledgement lets
future jobs start without stale tasks touching a new page. Cover failure with
valid source retention, expiry clearing source/target, task-create failure,
cancel before dispatch and during work, rapid exit/reentry and stale identity.
Keep metadata bounded. When the eight-slot mock table is full, provide an explicit
two-press mock-only reset after worker cleanup and prove the ninth synthetic job
runs without a reboot; never evict production/companion evidence. Apply the
RAM-only TTL to every retained plaintext field, including partial-failure source,
and reject unterminated provider output without truncation. Do not claim RAM is
durable.

Use the existing dynamic Chinese font. Add/regenerate static glyph coverage as
needed and validate dynamic mock text; future missing glyphs stay explicit.
Do not add microphone capture, scratch Flash, storage-format changes, companion/
HTTP wiring, production credentials, real provider calls, T2 or TTS.

Test the real changed worker and UI paths with external FreeRTOS/LVGL/provider/
time stubs, default and -O2, then run the static gate. Run focused tests first.
Do not run huge parallel builds; if a firmware check is needed use the known
ESP-IDF 5.5.3 environment with bounded jobs. Codex will run the full gate after
review. Update aligned English/Chinese authoritative checkpoint/prompt docs and
the font record; do not edit changelog. Write a concise handoff outside the repo
with touched paths, actual tests, RAM/task behavior and limitations. No commit,
push, reset, clean, stash, EOL normalization, deploy, device I/O, credentials
access or real payment. Report Build, Host tests, Device tests and Unverified
separately. Do not claim complete translation, SA-15, T2 or device validation.
```

### 7.19 Real T1 device integration code review handoff

```text
Implement only the authorized real T1 firmware integration, preserving the
reviewed mock cancellation/session/TTL/cleanup protections and all concurrent
owners' files. MiMo is the UI default; MOCK is an explicit local test choice and
never a silent fallback. Use the shared translation config, USB window, audio
scratch lease and HTTP transport without editing their owned sources. Keep the
server job/generation identity separate from the local UI epoch.

Real pipeline: config/online/time and /ready; shared TRANSLATION lease with the
authoritative cache generation; 16 kHz/16-bit/mono, 0.3-10 s, <=512-byte chunks;
release BSP audio before TLS; finalize/hash from bounded Flash reads without a
whole-RAM WAV; one POST and one PUT; release scratch before polling; 60-second
post-record budget; DONE both texts, partial MT failure preserves source with
120-second TTL; local cancel/exit wins late results; ambiguous POST/PUT is
UNKNOWN with no retry; cleanup failure quarantines and blocks reuse. UI input
dispatches intents only; no NVS/TLS/audio/Flash under LVGL/input locks. Show the
config window and recording state/countdown without secrets.

Test the actual worker/UI with external I/O stubs: capture cap/min/stop,
release failure no TLS, lock order, single POST/PUT, config missing, ambiguous
POST/PUT, identity mismatch, deadline UNKNOWN, partial TTL, cleanup quarantine,
cancel/exit/reentry and config interlock. Keep focused protocol/HTTP/shared tests
registered. Report EXPIRED/AUDIO_EXPIRED protocol enum gap to root; do not edit
transport/protocol. Run focused tests and one targeted build if useful; do not
run the complete gate while concurrent owners work. No commit, push, deploy,
flash, provider call or credential access. Stop with a handoff of exact files,
tests, interfaces and remaining gaps.
```
