<p align="right">
  <a href="passport-translation-t1-core.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Translation T1 core and synthetic companion checkpoint

Updated: 2026-10-26. OC-09 (Tasks milestone candidate) and OC-10 (T1 core and
synthetic companion) were completed and independently reviewed. OC-11 added the
redesigned in-firmware translation page, a bounded worker around the actual
`suite_translation_core` bookkeeping and a replaceable synthetic provider. The
authorized real T1 integration adds a MiMo-default capture/upload/poll worker and
keeps MOCK as an explicit local test mode.

Current status: the companion service is deployed and one authorized synthetic
real-provider EN->zh-CN smoke passed. The original OC-11 application was restored
app-only on 2026-10-25 and the operator confirmed the restored translation menu and
original data are normal. The current installed application is the post-recording
response-cache package `d2975d36...` (app `79a0d3b5...`, ELF `9d71323f...`), installed
app-only on 2026-10-26 by one guarded write. A 100.95 s bounded no-reset startup
window received 0 serial bytes, 0 TRDIAG events and 0 fault markers, with no boot ELF
line or trace; absent data is not a clean, stable or startup PASS. After the human
declared readiness, two guided manual trials, one English-to-Chinese and one
Chinese-to-English, completed on this install: the operator reported correct
recognition, translation and display in both directions and no obvious stutter, and
found the response speed not very fast but acceptable. Device tests are PASS only for
that operator-reported one-trial-per-direction subset; the latency cause is
undetermined, and repeatability, TTS and SA-15 remain unaccepted. The prior
`902a0930...` install is
superseded dated history: its operator-reported ready/cancel round was normal, and its
later actual EN->zh-CN attempt failed after recording and OK stop with the
translation-cache out-of-memory message and no A/F/L; the failed HTTP call and onboard
heap largest/free block are unknown. The 2026-10-26 complete-gate Build/Host PASS is
reused with no firmware rerun. The earlier `cb8bd517...` TLS-preflight failure and
weather `task_wdt` remain superseded history. Exact counters, artifacts and open items
are in [Current checkpoint](#current-checkpoint-2026-10-26) and
[the current gate](#current-complete-gate-2026-10-26). T2/TTS is unimplemented, and
SA-15 remains incomplete. User decisions are recorded below.

## Current checkpoint, 2026-10-26

Installed identity. Package `d2975d36...`, app `79a0d3b5...`, ELF `9d71323f...`,
3,697,184 bytes, written app-only at `[0x10000,0x397000)` on 2026-10-26. The guard
returned rc0 PASS `APP_ONLY_WRITE_DIGEST_AND_CLOSE` from a single 78.57 s execute: one
read plus one write bridge session each closed, COM3 release confirmed, the fresh live
4,096-byte partition-table sector matched `ba728028...` before the app write, and the
pinned esptool digest comparison matched with one success marker (the raw device digest
was not logged). The full package is the application-source package identity, not a
whole-Flash clone: bootloader, table and data/key partitions were not written, and
settings/keys/cache/journals are preserved by range separation only.

Bounded startup observation. One fresh no-reset window of 100.95 s received 0 serial
bytes, 0 TRDIAG events and 0 fault markers, with no boot ELF line or trace. That
absence remains dated startup evidence only: it is not the current total-log state and
not a clean, stable or startup PASS. It was followed by the manual acceptance below.

Bilingual manual acceptance. After the human declared readiness, Codex guided exactly
one English-to-Chinese input and exactly one Chinese-to-English input; no retry and no
additional trigger followed. The operator reported the English source recognized and
displayed with the corresponding correct Chinese greeting-and-good-morning
translation, and, in reverse, the Chinese greeting recognized correctly with an
English "Hello, good morning." target. The operator explicitly reported no obvious
stutter in both trials and found the response speed not very fast but acceptable. That
speed observation is a performance item whose cause is undetermined; no service
level, hardware-limit or optimization promise is made.

The English trial window covered 100.96 s and 2,223 serial bytes with five numeric
TRDIAG events (preflight, capture_started, audio_released, capture and admit), all with
result 0 and zero captured fault markers. Cumulative local elapsed values were
5.240764 s at preflight, 9.374270 s at capture and 14.132428 s at admit, with no
upload, status or final event in that window. These are partial local cumulative times
that include local work, network and TLS; they are not full end-to-end or
provider-only latency, and no client-action-to-window time is proven. The largest
block was 9,216 bytes at all five captured phases, with free bytes of 35,524 / 23,560 /
31,524 / 31,524 / 30,876; older post-failure TLS samples are not comparable and do not
establish causal bytes gained. The Chinese trial window covered 101.39 s and 0 serial
bytes with no trace, which does not invalidate the operator result and does not prove
error-free or full-runtime behavior. Both observer contexts closed with rc0 and COM3
was handed back. No further monitoring, recording, performance probe or provider call
followed.

Acceptance limits. These two completed trials reported no out-of-memory message, but
they do not prove a permanent cure or root cause, a stable heap improvement,
repeated-cycle or leak-free behavior, long-term stability, whole SA-15 or TTS
completion. The current settings, cache and task-journal visual audit was not
separately reported, and preservation still rests on the flash-range evidence. The
earlier 100.95 s zero-byte startup window remains dated startup evidence only, not the
current total-log state and not a clean, stable or startup PASS. The prior `902a...`
install remains superseded history; whether its admit, upload or status request was
sent is still unknown. The older `cb8bd517...` TLS fatal and weather `task_wdt` remain
unresolved history with unproven relation, so no old failure is transferred to
`d2975d36...` and the watchdog is not called fixed.

Prior install, dated history. Package `902a0930...` / app `a71b9c3e...` / ELF
`0352858d...`, 3,696,416 bytes, was installed app-only on 2026-10-26 and is superseded
by the current flash. Its operator-reported ready-plus-record-entry plus UP-cancel round
was normal; its 100.91 s / 112-byte startup window belongs only to it. A later actual
EN->zh-CN attempt on that install failed with the translation-cache out-of-memory
message after recording and after pressing OK to stop, with no A/F/L values displayed.
The operator exercised recording and stop; whether an admit, upload or status request
was actually sent is unverified, so neither upload nor provider billing completion is
claimed. The onboard heap largest/free block is also unknown. This failure is distinct
from the earlier pre-ready text failure and prompted the response-cache fix; no
physical audio leak, stable PASS or root cause is inferred.

Response-cache fix (installed source). The installed source allocates a NULL JSON
response buffer sized exactly Content-Length + 1 after bounded header validation, at
most 8,193 bytes, while the 8 KiB JSON maximum is unchanged, for all five HTTP calls.
A late E_BUFFER on admit, upload or status is ambiguous and reported UNKNOWN with the
short status "out of memory; service result unknown" (Chinese on the device). The
operation ID is published before admit only when the session, cancel and exit checks
are valid; a known job identity is preserved; there is no replay and no unsolicited
DELETE. TLS behavior and the 2,048-byte / 512-scalar text limits are unchanged, and
there is no new global 8 KB allocation or any configuration, partition or BSP source
change. The source-level benefit is unmeasured and prevention is not proven.

Memory-lifetime change. The source defers the 8,193-byte JSON buffer per call across
all five HTTP operations and 4,098 bytes at ready, allocates text after ready and
before audio, and keeps the 8 KiB JSON cap, 2,048-byte/512-scalar text limits, existing
OOM message, strict TLS behavior and best-effort cancel. These are source-level
reductions; the actual onboard heap gain is unmeasured, and no fixed-memory or watchdog
cure is claimed.

Superseded history. The previous `cb8bd517...` / app `d0396c22...` / ELF `0ac23853...`
install recorded one EN->zh-CN TLS-preflight failure (`R7 S12 X11` / `E801A T3000 V0` /
`N11 F12004 L4608`; later `N11 F11800 L3840`) and one runtime `task_wdt` during weather
certificate work. These are superseded dated history; their relationship to the current
install is unproven, and they are not current evidence. Only an X.509 fatal was proven
before; no V0 certificate-validity claim is made. The superseded `43501...` trial
remains the source of the historical human `R7 H0 B0`; original `f9a849be...` /
`f5be0652...` and the `8e86` archive remain historical/rollback.

Open. Device tests are accepted only for the operator-reported one-trial-per-direction
subset above; they do not establish repeatability, leak-free or long-term stability, a
permanent memory cure, complete request timing, provider billing, TTS/playback or
SA-15. Exact onboard heap behavior beyond those five partial samples remains
unmeasured, the prior install's failing HTTP call is still unknown, and physical-audio
behavior is unverified. No prior failure is transferred to the current install, and
the weather `task_wdt` is not shown fixed. Current remaining work is latency
localization and the unaccepted scenarios, not another flash or missing basic
translation.

## Implemented scope

- `main/suite_translation_core.*` is a pure C11 state/bookkeeping module with no
  ESP-IDF, LVGL, network, storage or clock dependency. It accepts only the
  explicit `en` to `zh-CN` and `zh-CN` to `en` directions, keeps source and
  translation metadata separate, and validates each text as at most 2,048 UTF-8
  bytes and 512 Unicode scalar values. Malformed UTF-8, overlong forms, surrogate
  code points and NUL bytes are rejected without truncation.
- The synthetic WAV envelope is limited to 320,044 bytes, 16 kHz, signed 16-bit
  little-endian mono PCM. The module validates the header, data length, minimum
  0.3-second payload and SHA-256 binding; it never copies or retains audio.
- Operations carry an operation ID, a distinct job ID and a monotonic generation.
  An operation ID is accepted only at exactly 32 lowercase hex characters and an
  audio SHA-256 only at exactly 64. A valid prefix followed by an extra character
  is rejected before lookup, slot allocation or generation increment. A matching
  replay returns the original admission without another provider claim. A
  different payload for the same operation is a conflict. The bounded eight-slot
  table rejects a new admission with `BUSY` when full and never evicts
  deduplication evidence.
- Upload, running, terminal result and cancellation are explicit states with a
  bounded event ring. A cancelled or terminal job rejects late results. UI-exit
  invalidation cancels active jobs and advances the local generation.
- `companion/translation/` is an independent job implementation with a separate
  SQLite metadata ledger, a separate bearer capability, a synthetic in-memory
  provider and a WSGI admission/status/cancel surface. It does not import or
  share the task store, journals, tokens, cache or admin credentials.
- The companion ledger stores only identifiers, payload/audio/text hashes, byte
  counts, states, generation and timing. Source text, translation text and audio
  stay in bounded RAM or the synthetic provider. A restart after a provider claim
  becomes `UNKNOWN`; a durable `DONE` row whose text is no longer in RAM is shown
  as `TEXT_LOST`. Capacity pressure rejects new admissions instead of removing
  earlier evidence.
- The local job path exercises synthetic ASR, synthetic translation and a
  synthetic WAV end to end. Audio upload remains a local function call: the
  one-shot WAV HTTP PUT, Flash staging and production adapter are not
  implemented by this mock. The user later approved the Flash/WAV direction for
  a future real adapter; OC-11 still does not implement it.
- A lock on each `TranslationJobs` instance serializes RAM-cache publication
  and cancellation, including short SQLite reads/cancellation transactions.
  Provider calls run outside the lock. Publication rechecks the durable state
  and generation and refuses to refill a cancelled slot. ASR and MT use the same guard, so a late callback returns the cancelled
  state instead of raising an unhandled generation error.
- Expired text is swept from every job at each service entry and by the explicit
  `cleanup()` interface. This local mock has no background scheduler or global
  timer: expired text can remain physically allocated until the next service
  call; with the default TTL it is not visible after 120 seconds. Metadata rows remain for
  deduplication evidence.

## OC-11 in-firmware page and worker mock

OC-11 adds the launcher-visible translation increment. It is a redesigned
application page, not a relabeled stub, but it remains synthetic and local.

- `main/suite_translation_ui.c` provides the page: an honest `MOCK local mock`
  badge, two explicit direction rows (`English to Chinese`, `Chinese to English`),
  a status line and a scrollable body containing separate labeled `Source` and
  `Translation` panes. Static
  labels use `suite_font_16`; dynamic source/translation text uses the existing
  public dynamic `suite_tasks_font_16`.
- Controls use the three existing buttons. In the ready state, UP/DOWN switch
  direction and OK starts. While a job runs, OK cancels. A terminal view uses
  UP/DOWN to scroll long text; OK returns to direction selection for a new run.
  OK long-press exits through the shared shell. The hint line always states the
  current action.
- `main/suite_translation.c` owns one bounded worker per dispatched job. The
  worker calls the actual core admission, upload, running and terminal-result
  path with a synthetic 9,644-byte canonical WAV built transiently on the heap.
  The WAV hash is computed outside the snapshot lock; core calls (including
  upload hashing) use a separate short core lock. It never touches LVGL,
  storage, credentials or the network. A provider
  interface in `main/suite_translation_provider.h` isolates the synthetic result
  boundary; `main/suite_translation_mock.c` is the in-firmware implementation and
  can be replaced by a later approved real adapter without changing the UI,
  cancellation or generation logic.
- One worker at a time owns cleanup. `suite_translation_run` refuses another job
  until the worker acknowledgement clears `worker_busy`. Page exit cancels and
  advances the session/generation without waiting; late results and cleanup from
  the old session are discarded. A task-creation failure becomes an explicit
  `UNAVAILABLE` state and a later attempt may succeed. The snapshot release
  helper drains a cancel/exit intent that arrived while the lock was held. Every
  lock owner, including `suite_translation_view_release`, participates; if
  another owner wins the lock it performs the same drain on its release. This
  completes offscreen cleanup without a page timer or later view getter. The
  worker also rechecks before acknowledging cleanup and clears full buffers.
- The page exposes processing, done, partial failure (recognized source retained
  when translation fails), cancellation, expiry, full-metadata and unavailable
  states. Every retained plaintext field, including source text on a partial
  failure, has a RAM-only 120-second TTL observed by the page timer; the exact
  deadline clears both buffers. Page exit also wipes the full text buffers. Core
  metadata remains for deduplication evidence. This is not durable storage and
  no idle physical deletion is claimed.
- Provider output must contain a NUL terminator inside the 2,049-byte cap. The
  worker does not write a terminator itself, so an unterminated buffer is
  rejected instead of silently accepted. Exact 2,048-byte / 512-scalar text is
  accepted unchanged; malformed or oversized output fails closed.
- The core's eight-slot table is unchanged. After eight distinct mock operations,
  a new run reports `BUSY` with `8/8` and does not evict evidence. The page then
  offers an explicit, two-press mock-only recovery (the hint shows OK clear record) after worker
  cleanup. That action reinitializes only the volatile in-firmware mock table,
  advances the identity epoch and lets the ninth synthetic job run without a
  reboot. It does not touch the companion, production deduplication or durable
  stores. Direction selection, scrolling and cancellation remain usable before
  the reset.
- The page's fixed Chinese labels reuse characters already declared in
  `assets/fonts/suite_charset.txt` and covered by `main/fonts/suite_font_16.c`;
  no font regeneration was needed. The built-in mock's dynamic
  text is checked against the public dynamic font. Future provider text outside
  that font remains an explicit LVGL placeholder; it is not silently dropped or
  substituted.
- The OC-11 mock increment itself does not implement a microphone, real
  recording/upload, scratch Flash, storage-format changes, the Python
  companion/HTTP path, production credentials, real translation calls, T2
  synthesis/playback or SA-15. The authorized real T1 section below adds the
  firmware capture/upload/poll path but still does not implement T2/TTS or claim
  SA-15.

Host tests execute the changed worker and page paths with external
FreeRTOS/LVGL/provider/time stubs at default and `-O2`: real thread cancellation
before dispatch and during work, partial source retention with exact TTL
boundary, exact 2,048-byte / 512-scalar acceptance, unterminated-output
rejection, full-buffer exit wipe, cancel at core and final-view publication
boundaries (including partial source), immediate exit/reentry during
publication, direction and pane separation, mutex and worker-allocation failure
recovery, task-creation failure recovery, rapid exit/re-entry, stale-session
rejection, eight-admission reset and page teardown.

## Real T1 device integration (code checkpoint)

The real path is implemented for later root deployment/review. It does not use
the mock core table, local mock reset, or whole-clip RAM WAV.

- The page starts in explicit `MiMo online` mode. `MOCK local synthetic` is a separate,
  visibly labeled choice and never acts as a fallback from a real failure. Rows
  are `EN to Chinese`, `Chinese to EN`, `USB configuration` and the current mode. `USB configuration`
  opens the existing bounded 60-second USB Serial/JTAG window; leaving the page
  cancels it. No NVS, TLS, audio or Flash work runs under the LVGL/input lock.
- Recording is user-initiated only. OK finishes, UP cancels; busy UP/OK cancel.
  `suite_translation_capture.c` is not needed; the existing worker owns the
  pipeline. The worker loads the private translation config, checks online and
  trusted time, calls authenticated `/ready` through the HTTP transport, then
  acquires the shared `suite_audio_job` lease as `TRANSLATION`. It uses the
  shared cache generation, `SUITE_AUDIO_TRANSLATION_SPAN` (0x50000), 16 kHz /
  16-bit / mono, at most 320,000 PCM bytes (10 s) and at least 9,600 PCM bytes
  (0.3 s). PCM is streamed in at most 512-byte chunks. Actual BSP reads can block
  up to about 1 s per call on the codec/I2S semaphore, so the stored PCM cap is
  exact while wall-clock capture and cancel latency are bounded by that read;
  this limitation is not hidden as a 16 ms guarantee.
- BSP audio is released before any TLS call. A release failure refuses network
  and preserves cleanup/quarantine. The canonical WAV is finalized and hashed
  from bounded Flash reads; the 32-byte digest is hex-encoded directly, never
  hashed a second time. The 60-second post-record deadline is set immediately
  after the PCM loop and before BSP release, so it covers release, finalize,
  hash, admission, upload and poll without reset. POST is attempted once; PUT
  streams exactly once through the lease read callback. Numeric diagnostics add
  timestamp/elapsed suffixes plus distinct `capture_started` and
  `audio_released` checkpoints for DMA-resident versus released measurements. Ambiguous POST/PUT becomes `UNKNOWN`; there is
  no retry, replay, or new paid job. After upload the lease is released (S-only)
  before polling. A failed cleanup quarantines the slot and is shown as a
  storage failure. BSP audio is released on every exit after an attempted
  init/format, including cancel/exit before the capture loop; release failure
  blocks TLS and is shown as a failure.
- Status polling uses the separate server `job_id`/generation identity, not the
  local UI session/epoch. It stops on DONE/FAILED/CANCELLED/UNKNOWN/TEXT_LOST or
  the absolute post-record 60-second budget. DONE requires both texts; FAILED
  with valid source preserves source with the same 120-second plaintext TTL.
  Local cancel/exit wins over late server results and sends at most one bounded
  best-effort DELETE when the job identity is known. A lost POST identity is
  shown as UNKNOWN and is never deleted or repeated blindly.
- Worker diagnostics are numeric only (stage/status/bytes/durations/heap/DMA/
  stack); no text, audio, token, URL or host identity is logged. Each of the five
  calls allocates its JSON response buffer at the validated Content-Length + 1 after
  bounded headers, and text buffers are heap-allocated before use; no full WAV is
  held in RAM. Config and temporary text buffers are wiped at cleanup. The reviewed
  release-and-drain cleanup handoff remains active for offscreen plaintext.

The shared protocol now includes `EXPIRED`, and the worker treats it as a
terminal expired result; a `FAILED` response carrying `AUDIO_EXPIRED` is also
shown as expired. Other protocol parse failures remain `UNKNOWN` without retry.
The HTTP transport source landed and the current tree links it, but DNS/TLS/
cancel details remain the transport owner's contract and are not re-verified
here.

## Job behavior covered by host tests

| Event | Implemented behavior |
| --- | --- |
| Matching admission replay | Return the original job and generation; no new provider claim. |
| Same operation, different payload | `OPERATION_CONFLICT`; no overwrite or provider claim. |
| Repeated matching upload | Return the existing state; no second ASR call. |
| Cancel during a provider callback | Discard the late result; no translation stage or text retention. |
| Late result after cancel | Rejected by terminal state and the advanced generation. |
| Restart after claim | `UNKNOWN`; automatic paid replay is not attempted. |
| Restart after `DONE` | `TEXT_LOST`; metadata is not a recoverable translation. |
| Metadata capacity | New admission returns `BUSY`; existing rows remain. |
| Task token presented | Rejected; only the independent translation capability is accepted. |

## Verification

Focused host checks:

```text
cc -std=c11 -Wall -Wextra -Werror -Imain \
  tests/test_suite_translation_core.c main/suite_translation_core.c
cc -std=c11 -Wall -Wextra -Werror -O2 -Imain \
  tests/test_suite_translation_core.c main/suite_translation_core.c
python3 tests/test_companion_translation.py
```

OC-11 focused checks (default and `-O2`) execute the real changed worker and
page:

```text
cc -std=c11 -Wall -Wextra -Werror [-O2] -pthread -DSUITE_TRANSLATION_TEST \
  -Itests/translation_stubs -Imain \
  tests/test_suite_translation_worker.c main/suite_translation_core.c \
  main/suite_translation_mock.c
cc -std=c11 -Wall -Wextra -Werror [-O2] -Itests/tasks_ui_stubs \
  -Itests/translation_stubs -Imain tests/test_suite_translation_ui.c
PYTHONDONTWRITEBYTECODE=1 python3 tests/test_suite_font_charset.py
./tools/validate.sh --static
```

Results for OC-11:

```text
Build: PASS (isolated ESP-IDF 5.5.3 full link of an earlier OC-11 revision;
       application 0x37b310 bytes, 0x234cf0 bytes / 39% free). The latest OC-11
       translation object compiles; current shared-tree final link is blocked by
       the concurrent suite_audio_job integration below.
Host tests: PASS (real worker/UI at default and -O2; font charset and repository checks)
Static gate: BLOCKED in the shared tree by concurrent unowned suite_audio_job
             integration (undefined suite_audio_job_* symbols), after all OC-11
             checks passed
Device tests: NOT RUN
Unverified: current shared-tree final link/merged-image archive/ELF after the
            concurrent integration settles, on-device rendering/input, real
            provider, paid calls, T2 and SA-15
```

The OC-11 build did not run `merge-bin`, archive verification or flashing. A later
shared-tree link failed only because a concurrent unowned `suite_tasks.c` change
references `suite_audio_job_*` functions not yet registered; OC-11 did not modify
those paths. Codex runs the complete gate after the shared tree stabilizes. The
previously reviewed OC-10 complete-gate artifacts remain historical for the OC-10
source, not this increment:

```text
Archive: build/firmware/c284de8dd95dd443b8154714a020f4ffbf4537c94ad7c6bca0acbc3b39d97b21
Full image SHA-256: c284de8dd95dd443b8154714a020f4ffbf4537c94ad7c6bca0acbc3b39d97b21
ELF SHA-256: 859d5cf3d582690a977dbc9a13c16cece1e3704d0aad2511e2640372fc69ecd6
Application image SHA-256: 6f045ff14d551f3f940479371f79ba0ed4829a55a3f20cc654ad61f4e681f40b
Merged image size: 3,696,656 bytes
Application size: 3,631,120 bytes at 0x10000
Version: 21c5110-dirty
```

`python3 tools/archive_firmware.py verify` passed for the listed archive. An
earlier full-gate attempt after the first static pass was interrupted by a WSL
environment restart; it produced no completion marker or new archive and is
recorded as interrupted, not as a build pass. A linker build and synthetic tests
do not prove device capture, provider quality, account terms, retention or SA-15
acceptance.

### Current complete gate, 2026-10-26

The reviewed post-recording response-cache fix passed one complete gate
(`./tools/validate.sh`, not separate static and firmware runs) with the native tree
as the only build input and an activated, pinned ESP-IDF 5.5.3:

```text
Build: PASS
Host tests: PASS
Device tests: PASS (limited to the operator-reported one-trial-per-direction
              recognition/translation/display subset and no obvious stutter;
              see the checkpoint)
Unverified: repeatability and long-term stability, complete end-to-end and
            provider-only latency, onboard heap beyond the partial samples,
            physical audio behavior, billing, TTS/playback, SA-15
```

```text
Archive: build/firmware/d2975d36eca1b89d98dd27f47711833f06afb4a8bb9a618eee11b0d357b94b0d
Full image SHA-256: d2975d36eca1b89d98dd27f47711833f06afb4a8bb9a618eee11b0d357b94b0d
Application image SHA-256: 79a0d3b5a59f5d1084267642837566b66054cb5d3ff3e06003457197371f59c1
ELF SHA-256: 9d71323ff8a1eb3231171dd2e6834f0f19d2ed07fb52c8fd413d4a97091ab66f
Merged image size: 3,762,720 bytes
Application size: 3,697,184 bytes at 0x10000
Partition table SHA-256: 34899c03... (3,072 bytes; padded 4,096-byte sector ba728028...)
Version: 21c5110-dirty
Status: VALIDATED_NOT_FLASHED at gate stage; subsequently installed
```

`python3 tools/archive_firmware.py verify` passed, and the ELF identity embedded in
the application image matches the archived ELF. The prospective sector-rounded
application range `[0x10000,0x397000)` ends before the `0x5c0000` factory boundary and
does not overlap the protected data partitions. The gate-stage `VALIDATED_NOT_FLASHED`
record is historical; `G` is unchanged. A later single guarded app-only write installed
this exact package: guard rc0 PASS `APP_ONLY_WRITE_DIGEST_AND_CLOSE`, the fresh live
4,096-byte table sector matched `ba728028...` before the write, the pinned esptool
digest matched with one success marker, and COM3 was released. It is now the current
installed application, and the two guided manual trials recorded in the checkpoint ran
on it. The installation itself issued no provider or paid call, and no firmware was
rebuilt.

### Real T1 focused verification, 2026-10-23

```text
real worker capture/config/transport/identity/cleanup: PASS (default, -O2)
  including raw-digest hex identity, BSP release after init/format failure,
  cancel during ready/admit/upload/status, one DELETE for known jobs, and
  EXPIRED terminal state
real translation page modes/config/recording/lifecycle: PASS (default, -O2)
translation protocol and HTTP transport focused tests: PASS (default, -O2)
shared audio lease/config/USB tests: PASS
repository checks and font charset: PASS
isolated ESP-IDF 5.5.3 build/link: PASS
  application 0x385ce0 bytes, 0x22a320 bytes / 38% free
Device tests: NOT RUN
Unverified: real USB/config on hardware, capture/upload timing and power loss,
            real MiMo/paid calls, DNS/TLS/cancel behavior on device, EXPIRED
            enum integration, T2/TTS and SA-15
```

The build included the landed `suite_translation_http.c` and the shared
scratch/config/USB sources. It was a targeted integration build, not the complete
gate; root runs the complete gate after all concurrent owners finish.

## User decisions recorded 2026-10-23

The trusted user explicitly approved all requested choices and said continue:

- Candidate A is selected: MiMo `mimo-v2.5-asr` for recognition and
  `mimo-v2.6-flash` for text translation. The disclosed lack of a fixed retention
  TTL and the unconfirmed audio-training treatment are accepted for this plan.
- Temporary plaintext WAV in an owned inactive Flash slot is approved for at
  most 10 seconds, followed by one bounded upload and cleanup. This approves the
  SC-08 capture exception direction, but the real adapter/service is not
  implemented by this firmware mock increment.
- Real-provider, device and paid testing are authorized after reviewed
  integration. The actual account region is not independently established, so no
  regional retention guarantee is claimed.
- DeepSeek production integration is rejected. MiMo ASR plus MiMo MT remains the
  selected plan; DeepSeek V4.1 Flash is not a production translator here. The
  official `deepseek-flash` alias does not provide ASR.
- T2/TTS remains unimplemented and SA-15 is not accepted.

## Remaining dependencies

- The real MiMo adapter, companion HTTP audio path and deployment are implemented
  and active; one authorized synthetic provider smoke passed. Further real or paid
  calls need separate authorization.
- The SC-08 Flash/WAV path and its bounded cleanup are implemented in the real
  worker. On-device erase behavior, timing and wear remain unmeasured.
- Device recording and stop were exercised on the prior `902a...` install: the
  operator confirmed a ready/cancel subset, and a later actual translation attempt
  failed after recording with the translation-cache OOM message. Whether an admit,
  upload or status request actually ran is unverified, so no upload or provider
  billing completion is claimed. The original `FAIL_CACHE_ALLOCATION`
  (`failure_result=6`) and readiness8 remain historical. The current `d2975d36...`
  application is installed and passed the operator-reported one-trial-per-direction
  bilingual subset; the remaining work is localization of the reported slow response,
  whose cause is undetermined, and the unaccepted scenarios: repeat cycles, long
  recordings, cancel/timeout/offline recovery and power cuts on this image, provider
  billing and SA-15. See
  [Current checkpoint](#current-checkpoint-2026-10-26) for exact counters, artifacts
  and open items. T2 synthesis/playback, OTP, wallet and any further flashing remain
  outside this checkpoint, and no further monitoring, recording or provider call is
  planned for this result.
- Device measurements and on-device acceptance are not complete. T1 core/mock is
  not a complete translation feature and is not SA-15 acceptance.
