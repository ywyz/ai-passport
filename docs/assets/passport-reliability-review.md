[简体中文](passport-reliability-review.zh_CN.md) · **English**

# OC-01/02/03 review and validation checkpoint

**Dated evidence record.** This document preserves the OC-01 through OC-11 review and
measurement history; it is not current project status. Current authority is
[suite-plan section 0](passport-suite-plan.md#0-current-implementation-and-acceptance).
The earlier WSL `p9_client_rpc` observation was a shared-filesystem access problem,
resolved by moving to native ext4 on 2026-10-22; it is not an open blocker and does
not require restoring WSL before proceeding.

Date: 2026-10-21, Asia/Shanghai. Base HEAD:
`21c5110548e329019b2418ded506db68fe991418`, branch
`feature/feature10-ble-provisioning`, with the existing dirty workspace preserved.
The requested OpenCode work and Codex review reached a validation blocker on
2026-10-21. The WSL-native run below resolves automated validation; hardware
acceptance remains open.

## WSL-native validation, 2026-10-22

The user authorized an independent Linux-filesystem validation copy. The checkout
is `/home/ywyz/code/ai-passport-oc-20261022/repo` on WSL Ubuntu ext4, at the same
base HEAD with the source working-tree changes overlaid. All 487 source entries
were copied and hash-checked; all 18 files in the previous final candidate
inventory matched before testing. Both original working copies were preserved.
The existing `/home/ywyz/code/ai-passport` has a different HEAD and its own dirty
changes; it was not used as this candidate or overwritten.

The final 28-group runtime suite and pure voice tests passed in both default and
`-O2` builds. The complete `./tools/validate.sh` passed using Linux Node.js,
Python, GCC and ESP-IDF v5.5.3. Companion ran 30 Tasks, 15 completion and 19 voice
cases with zero skips; font coverage and the rest of the static gate passed.
There was no new failure requiring another OpenCode repair. Candidate production,
test, font, configuration and lock-file bytes were unchanged by validation;
only these bilingual checkpoints were updated afterward.

The firmware gate used tracked defaults and `partitions.suite.csv`, not a local
ignored sdkconfig. The application is 3,624,704 bytes in the 5,963,776-byte factory
partition; the verified merged image is 3,690,240 bytes for an 8 MB ESP32-C3.
An independent `tools/archive_firmware.py verify` also passed.

- Full-image SHA-256: `7c292cfc48ed58de50ffa71d3fb5dafb0c88cddb21f5d2289588780f51819d51`.
- Matching ELF SHA-256: `e8b6ae41f0bd32690107aafb73bb3990836d688a0349700432f5ddc8fdb3521a`.
- Verified bundle: `build/firmware/7c292cfc48ed58de50ffa71d3fb5dafb0c88cddb21f5d2289588780f51819d51/`.
- Merged output: `build/FoloToy-AI-Passport-full.bin`.
- Local inventories, focused logs, full-gate log and archive verification:
  `/home/ywyz/code/ai-passport-oc-20261022/evidence/`.

Linux-native access and IDF activation were healthy. The earlier `p9_client_rpc`
observation concerns shared-filesystem access and does not establish that native
WSL storage was blocked. No WSL restart was needed. WSL exposed no ttyACM/ttyUSB
serial device during read-only enumeration; this does not establish that the
Windows host has no connected device.

## Execution and review

All three session metadata records confirm `providerID=opencode-go`,
`model.id=deepseek-v4.1-flash`, `variant=default`. The local CLI is 2.0.32.

| Task | Session | Result |
| --- | --- | --- |
| OC-01 | `ses_ea13a76aaffezLo8zwAgLGhM31` | Read-only task/voice reliability review. Codex narrowed the findings and selected regressions. |
| OC-02 and repairs | `ses_ea131593bffezbd6sQeTLUxe9V` | Regression tests first, then separately dispatched production repairs and review follow-ups. |
| OC-03 | `ses_ea134d1f4ffedPDm40hs0S2dCY` | Translation proposal, revised twice after review; saved in the [reviewed plan](passport-translation-plan.md). |

OpenCode had a graph index, but coverage freshness was `metadata_changed`; direct
source checks were necessary. Codex's own session had no graph tools and used
the codebase-memory skill's source fallback. Neither side treated a clean graph
coverage result as proof of completeness.

The review retained the existing stable operation IDs, durable receipts,
conditional CalDAV writes, strict readback and task-cache preservation. It
corrected two overstatements: creation/completion share the limited writer token,
and QUEUE_FULL/unknown failures are not safe discard outcomes. Periodic receipt
polling remains an unmeasured design tradeoff; no backoff policy was added.

## Candidate changes

| Area | Resulting behavior |
| --- | --- |
| Slow transcription | Only transcription response waiting may use the remaining 45-second request budget. Handshake/upload and ordinary preflight/creation operations retain their 10-second stage cap. Every stage is clipped to the absolute request deadline; late responses are rejected. No automatic ASR resend was added. |
| Definite admission rejection | Only an exact HTTP 400 `CREATION_TARGET_REQUIRED` can become the existing READONLY result with explicit discard confirmation. Unknown errors, queue capacity, malformed bodies and ambiguous transport outcomes retain the intent. |
| Error messages | Separate authentication/configuration, quota and temporary speech failures. Unknown/private upstream payloads are not displayed. |
| Unready journal | Starting, clearing a preview, and cancelling before the worker starts preserve a visible failure while storage/configuration is unavailable. An accepted start cannot remain silently CHECKING. |
| Parser/transport boundaries | Reject duplicate/missing/non-string error fields, trailing garbage, escaped NUL in keys/values and raw NUL in the actual HTTP body. Clear incomplete response bodies before diagnostics. |
| Fonts | Add U+989D/U+5EA6 to the fixed charset and regenerate the 16 px font with the pinned public Noto source and lv_font_conv 1.5.3. Widget bindings remain unchanged. |

Production edits are confined to `main/suite_tasks_voice_impl.h` and
`main/suite_task_voice.c/.h`, plus the fixed font/charset. A harmless explicit
unsigned cast also resolves a pre-existing log-format warning exposed by the
standalone RISC-V/stub syntax check. BSP, deployed companion code, partitions,
credentials, completion protocol and journal layout were not changed.

Tests exercise actual voice worker/HTTP paths with external-I/O stubs. They
cover timeout ordering, cancellation after a complete response, definite versus
ambiguous rejection, NUL/duplicate-field errors, unready start/cancel interleaving,
same-ID replay after local acknowledgement failure, server lease recovery,
explicit resume without ASR, and rejection before versus after durable admission.
The final runtime source contains 28 groups. New pure-logic parser assertions
and three server regressions accompany it.

## Historical validation ledger, 2026-10-21

| Check | Actual result |
| --- | --- |
| Initial complete `./tools/validate.sh --static` | PASS before this iteration's edits; 61 companion cases, zero skips under WSL. This does not validate the candidate. |
| Selected red regressions | Codex independently reproduced 7/18 failed groups in default and `-O2`, before production repairs. |
| Repaired runtime before the last transport/cancel additions | Codex independently ran 26/26 groups in default and `-O2`: PASS. |
| Final native Windows Python regressions | Tasks 30 cases with one POSIX-permission skip; completion 15/15; voice 19/19; font coverage 4/4: PASS for executed cases. |
| Final RISC-V GCC 14.2 syntax with test stubs | PASS with `-std=c11 -Wall -Wextra -Werror -O2 -fsyntax-only`. This is not a firmware build or a runtime test. |
| Final 28-group execution | NOT RUN after the final additions. |
| Final complete static gate | BLOCKED. An earlier attempt stopped at an English-font-document issue, which was repaired; its rerun stalled in WSL shared-file I/O and was interrupted. |
| Firmware gate / merged image | NOT RUN; no new validated firmware bundle from this iteration. |
| Device tests | NOT RUN for this candidate. |

The WSL Python process was observed in `D` state at `p9_client_rpc`. Independent
compilation and bounded local-copy probes also timed out. Native Windows checks
remained usable. No WSL restart or termination of unrelated work was performed.
An excluding-red diagnostic run reported by OpenCode was not used to pass a gate.

A later OpenCode compiler-discovery command used `2>nul` in the wrong shell and
created a 34-byte reserved-name file. It was absent at the initial checkpoint;
Codex moved this owned artifact into the local evidence directory without
discarding it. A final 45-second bounded runtime retry still timed out with no
output. The artifact is not established as the cause of the earlier WSL stall.

Local evidence is under `build/acceptance/20261021-oc-cycle/`: model metadata,
red/green focused logs, native Python results, the syntax result, final repair
notes and source inventories. These local records are not committed or published.

## Device flash and remaining acceptance

The user approved segmented flashing of the exact `7c292cfc/e8b6ae41` firmware.
COM3 ESP32-C3/8 MB identity and the current partition sector were verified; the
bootloader, table and application writes passed independent verification. Actual
erasure excluded every data partition. A 90.52-second normal-reset window matched
the ELF, reached readiness, connected saved Wi-Fi and updated weather over HTTPS
without observed errors or crashes; the port was released. See the
[device record](passport-device-tasks.md#reliability-candidate-flash-and-startup-2026-10-22)
for the complete scope and samples.

The operator confirmed retained original cache and the six latest tasks display
correctly; recording/Chinese preview/Cancel remain pending. See the
[device diagnosis](passport-device-tasks.md#source-freshness-diagnosis-2026-10-22).
Startup logs do not accept those interactions. At this date translation T1/T2 were
still proposals and the SC-08 deviations were unresolved; the later approved
decisions and evidence are in the [translation plan](passport-translation-plan.md). No application
source replacement, new-task confirmation, commit or push occurred. The later
approved server timer passed two automatic reads; see its
[deployment record](passport-nextcloud.md#five-minute-timer-deployment-2026-10-22).

Build: PASS.
Host tests: PASS.
Device tests: PASS only for segmented flash/verification and startup/network/weather.
Unverified: initial refresh failure, recording/Chinese preview/Cancel,
new error-label rendering, real speech/cancel latency, heap/DMA/stack peaks,
power cuts and integrated operation.


## OC-04/05/06 follow-through, 2026-10-22

The user authorized the next prompts in the existing WSL-native checkout.
Native OpenCode CLI 2.0.22 executed all three; exported assistant-message metadata
independently confirmed `opencode-go/deepseek-v4.1-flash`, default variant.

| Task | Session | Reviewed outcome |
| --- | --- | --- |
| OC-04 | `ses_e9c64ac39ffeRWkenRepa5uFUL` | Current-image acceptance matrix, corrected after source review: offline/admission early returns emit neither download log; missing logs do not prove failure. Standard log timestamps exist, but voice start/stage timestamps do not. |
| OC-05 | `ses_e9c64ac5fffe4KxP9lFU5hEe6C` | Read-only intake followed by one reviewed test-only coverage fix; no new production defect was invented. |
| OC-06 | `ses_e9c64ac4fffey5pxSI3FepdjI7` | Translation decision packet; see the reviewed [T1 packet](passport-translation-plan.md#oc-06-decision-packet-2026-10-22). No implementation or provider choice was made. |

Only `tests/test_suite_tasks_runtime.c` changed outside the aligned checkpoint
documents. Its new actual-worker preflight case asserts the fixed target-required
message, terminal failure, no microphone/ASR/intent write, preserved cache and
released client/network ownership. A copied-header mutation removed only that
message mapping: the old 28 groups missed it; the new 29 groups caught exactly
that case, in default and `-O2`. Codex independently reproduced all four mutation
runs and both unmutated 29-group passes. Production, BSP, fonts, configuration,
partitions, lock files and deployed service code remained byte-identical.

The complete gate passed in ESP-IDF 5.5.3, including 64 companion cases and zero
skips. The new archive independently verified:

- Validation-only full SHA-256: `8e5bfcdf0d7d8139d12fc02e6d4271d8d5998a91ecc3b3b22e3b3cc33d2fd41f`.
- Matching ELF SHA-256: `221c99980e6b93d16c6b2c487b82c8f8cd7f01f2dd0143b9aab636648745a436`.
- Bundle: `build/firmware/8e5bfcdf0d7d8139d12fc02e6d4271d8d5998a91ecc3b3b22e3b3cc33d2fd41f/`.

This build was not flashed. Device acceptance remains bound to the previously
installed `7c292cfc/e8b6ae41`, reverified against its archive and the observed boot
ELF. The operator confirmed capture cancel/re-entry, Chinese preview/default
Cancel, preview exit cleanup, approximately ten navigation cycles, seven current
tasks, offline cache and successful reconnect/Refresh. Read-only server receipt
hashes were unchanged; no task was created by Cancel. USB unplug/reconnection
explained the temporary missing port. The reconnect window contained one USB
reset/startup; it was not treated as reset-free testing. Exact samples and
limitations are in [device acceptance](passport-device-tasks.md#oc-04-current-firmware-acceptance-2026-10-22).

Private evidence: `/home/ywyz/code/ai-passport-oc-20261022/oc-040506-20261022/`.
No copy synchronization, Git reset/clean, commit, push, deployment, firmware write,
whole-chip erase or eFuse action occurred. Existing original working copies and
device/server configuration, task cache and journals were preserved.

Build: PASS. Host tests: PASS.
Device tests: PASS only for the recorded operator/log subset.
Unverified: first refresh failure's root cause, recognition-wait cancel, physical
error-label rendering, voice timing/minimum heap/DMA peaks, write-time physical
power cuts and long-duration/mixed operation. Existing telemetry cannot close
the resource/timing gap; translation choices remain separate.


## OC-07/05/08 instrumentation review, 2026-10-22

All source, build, test and evidence operations stayed in the independent native
WSL checkout above. Native OpenCode 2.0.22 executed OC-07, two reviewed OC-05
repair passes, then read-only OC-08. Exported message metadata confirmed
`opencode-go/deepseek-v4.1-flash`, default variant, for all three sessions:
OC-07 `ses_e9c3b2ea5ffeiS85vMLLo06ide`, OC-05
`ses_e9c64ac5fffe4KxP9lFU5hEe6C`, OC-08 `ses_e9bef4c10ffeBwlQeSneEowWJ4`.
The available graph covered the Windows checkout with stale path metadata;
targeted native source inspection was used rather than claiming graph coverage.

The increment adds bounded numeric voice/download diagnostics. Codex review
reproduced lost-final/sequence, request-publication, cleanup-boundary and
successful-open-then-cancel evidence defects. OC-05 repaired them and strengthened
actual worker/transport regressions, independent fake-clock/resource-mask oracles
and copied-source mutation checks. Production changes are confined to
`main/suite_tasks.c` and `main/suite_tasks_voice_impl.h`; test changes are confined
to `tests/test_suite_tasks_runtime.c` and its external-I/O stub header. Existing
request budgets, paid-stage policy, storage formats and device configuration
remain unchanged. No automatic ASR resend was added.

The final 44 runtime groups passed in default and `-O2` builds. Static and
complete ESP-IDF 5.5.3 gates passed, including 64 companion cases with zero skips;
Codex independently verified the immutable archive and reviewed source hashes.

- Full-image SHA-256: `a353524e7c8782a75d068aef81007a21c247ff4b0daff12eefc1c00c9077d679`.
- Matching ELF SHA-256: `dc655e73a1242d9959f386440b00f1d320872ca7b21b7b17604046ae5745253c`.
- App SHA-256: `997690fb289aa3dc5f7a1106d3fd87e7ab1f87e8239bc5a014c947dcaa471da4`.
- App/merged sizes: 3,631,120 / 3,696,656 bytes; archive under `build/firmware/` keyed by the full-image hash.
- Four-file diagnostic source inventory SHA-256: `61aa7941841815fd9c5b299af38f7357df39418d28735732c6440ea984a66341`; this is not a whole-tree provenance hash.

Fixed diagnostic RAM is 376 bytes. The final image is 736 bytes larger than the
OC-07-only intermediate. No diagnostic allocation, permanent task, periodic heap
walker, per-chunk logging, IDF patch or local-minimum monitor was added. Heap/DMA
values are overlapping capability snapshots, not additive or true operation-local
peaks; stack values are worker watermarks in bytes. HTTP wait-return/status fields
do not prove admission, billing or refund. SC-10 resource margins remain proposals.

OC-08 prepared the bounded measurement cases and their remaining gaps. The
hardware bridge passed memory-serial cleanup/framing checks; the passive observer
passed synthetic parsing and actual C-formatter compatibility checks. These did
not open COM3 and are not device evidence. The exact new app-only flash and
measurement handoff is recorded in the [device checkpoint](passport-device-tasks.md#oc-0708-instrumented-candidate-handoff-2026-10-22).
Private local evidence, contracts, diffs, red/green/gate logs and exported metadata
are under `/home/ywyz/code/ai-passport-oc-20261022/oc-0708-20261022/`.

Build: PASS (exact reviewed archive above).
Host tests: PASS (44 groups default/optimized, static and complete gates).
Device tests: NOT RUN for this candidate; installed firmware remains `7c292cfc/e8b6ae41`.
Unverified: real event/serial behavior, stop/cancel/release/cleanup measurements,
true transient heap/DMA/largest-block peaks, UI timing/rendering, write-time power
cuts, long-duration/mixed operation and the original unreproduced Refresh cause.
Translation decisions remain separate. No original-copy synchronization, commit,
push, deployment, data erasure or firmware write occurred in this increment.


### Approved instrumented app write, 2026-10-23

The user approved and Codex completed the exact `a353524e/dc655e73` app-only
write on COM3. Current-table matching, write scope, device digest and connection
closure passed; instrumented operator measurements remain pending. See the
[flash record](passport-device-tasks.md#instrumented-application-flash-2026-10-23).
This updates the earlier not-installed status, not the old-image acceptance.


### Measured subset closed, 2026-10-23

The operator confirmed retained tasks, usable recording and cancel/re-entry.
Two complete same-window sequences each finalized once: capture-cancel endpoint
117.741 ms; short-stop endpoint 19.503 ms. The second sequence's later cancel
matched the operator's clarified action. Cleanup records were complete; no new
repair issue was demonstrated. Resource values remain stage samples, with
internal free/largest minima 42,544/23,552 bytes and worker watermark 2,308 bytes.
The operator requested no excessive defensive testing; Codex stopped observation
normally and released COM3. Preview/remote-wait timing and true peaks remain
unverified rather than prompting more rounds. See the
[measured record](passport-device-tasks.md#measured-voice-subset-and-closeout-2026-10-23).
Build/Host tests remain PASS from the exact reviewed build; Device tests PASS
covers only the recorded flash, operator and numeric subset.
