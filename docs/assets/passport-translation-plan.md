[简体中文](passport-translation-plan.zh_CN.md) · **English**

# Translation T1 approved design and current evidence

Updated: 2026-10-25, Asia/Shanghai. T1 is implemented and deployed: firmware
capture/upload/poll with MiMo speech recognition and text translation, an
independent companion service, encrypted configuration and translation-only USB
provisioning. One authorized synthetic real-provider EN->zh-CN smoke passed, and the
historical translation page opened on device. The last real OC-11 on-device attempt
stopped at service readiness before capture; the device now holds an older
placeholder-only build that cannot reach that path. T2/TTS remains unimplemented;
this document is not complete feature acceptance and SA-15 is incomplete. Requirements remain SU-10/11, SC-08, ST-04 and SA-14/15/16 in the
[suite PRD](passport-suite-prd.md) and [contract](passport-suite-contract.md).

## Approved decisions

| Decision | Recorded outcome |
| --- | --- |
| Provider | MiMo `mimo-v2.5-asr` for explicit `zh`/`en` recognition and `mimo-v2.6-flash` for text, both at `POST https://api.xiaomimimo.com/v1/chat/completions`; non-streaming ASR, JSON-object text output, thinking disabled and a bounded completion. |
| Rejected alternative | DeepSeek is not a production translation provider; the `deepseek-flash` alias has no ASR. OpenCode Go DeepSeek V4.1 Flash remains an OpenCode development tool. |
| Capture exception | Temporary plaintext WAV in an owned inactive Flash slot, at most 10 seconds, followed by one bounded upload and cleanup, replaces volatile-only capture. |
| Retention | The user accepted the disclosed lack of a fixed retention TTL and the unconfirmed audio-training treatment. No fixed TTL, zero-retention setting or account-region guarantee is claimed. |
| Real calls | Real-provider, device and paid testing were authorized after reviewed integration and used only within that authorization. |
| T2 | Synthesis and playback remain unimplemented and are not approved by this document. |

Provider interfaces were rechecked on 2026-10-22: [ASR API](https://mimo.mi.com/static/docs/api/audio/Speech-Recognition.md),
[text API](https://mimo.mi.com/static/docs/api/chat/openai-api.md) and
[TTS API](https://mimo.mi.com/static/docs/api/audio/tts.md). The
[MiMo platform policy](https://privacy.mi.com/XiaomiMiMoPlatform/zh_CN/) supplies no
fixed API TTL. The user accepted the disclosed uncertainty for the selected plan; the
actual account region, settings and audio-training treatment remain unverified. No
fixed TTL, zero-retention setting or privacy guarantee beyond that recorded acceptance
is claimed. SSE availability does not establish a provider cancellation or refund
guarantee; closing the connection and discarding a late result may still incur charges.
This boundary applies to T1 ASR as well as T2 TTS.

## Reuse and isolation

Reuse BSP PCM access, trusted-time/TLS checks, `suite_net`, strict parsing,
the page lifecycle and the existing public dynamic font. Keep task creation and
completion tokens, `taskops`/`taskvoice`, CalDAV writes, receipts and active cache
generations outside translation. The existing two task operations share a limited
writer token; translation receives a different capability.

The independent translation capability is a separate encrypted configuration key,
with safe commit/readback preserving every existing namespace. Adding a key alone is
not a format migration. Provider keys remain server-side. Missing translation setup
does not disable Tasks, weather or provisioning.

Shared inactive-slot audio uses `suite_audio_job` as the explicit scratch owner; the
task worker is not copied and partitions are unchanged. Task voice, task downloads
and translation all follow the same acquisition order below.

## Bounded T1 protocol (approved)

The two SC-08 capture exceptions are approved: temporary device Flash audio instead
of volatile-only audio, and one WAV PUT instead of chunk PUT/finish. TTS retention
beyond successful retrieval remains a T2 question and is not approved.

1. Preflight ONLINE, trusted time and service readiness. Capture at most 10 seconds
   of 16 kHz, signed 16-bit little-endian mono PCM, at least 0.3 seconds.
2. Finalize a canonical WAV in the owned inactive slot and compute its full-file
   SHA-256 before POST. Never hold the full clip in RAM.
3. POST `/suite/v1/translations` with `schema`, `operation_id`, explicit language
   pair, `format`, `audio_sha256` and `audio_bytes`. Persist admission before 202.
4. PUT `/{job}/audio`, at most 320,044 bytes. Validate complete length, WAV and hash
   before any provider claim. A repeated matching upload must not repeat ASR.
5. GET `/{job}` polls state/result; DELETE cancels idempotently. No automatic new
   paid operation follows an ambiguous response. T2 adds `/{job}/speech` and an
   authenticated GET for normalized playback audio.

All routes require a distinct Bearer translation token. Task reader/writer/admin
tokens cannot call them, and the translation token cannot call task routes. No
token goes in URLs or logs. The response identity must match job, operation and
device generation. Compute Content-Length and text counts from actual bytes.

```json
{"schema":1,"operation_id":"0123456789abcdef0123456789abcdef","source_lang":"en","target_lang":"zh-CN","format":"wav","audio_sha256":"<computed SHA-256>","audio_bytes":320044}
```

```json
{"schema":1,"job_id":"0123456789abcdef0123456789abcdef","state":"DONE","source_lang":"en","target_lang":"zh-CN","source_text":"<recognized source>","target_text":"<translated text>"}
```

These illustrate fields; the hash and audio length must describe the actual clip.
Each text is limited to 2,048 UTF-8 bytes and 512 Unicode scalar values. Validate
both limits without truncating or replacing translation with a summary. Source
and translation use separate labeled scroll areas. Preserve a valid source when
translation or synthesis fails. Unsupported glyphs must remain explicit.

## Restart, payment and retention

Use a separate bounded metadata ledger, never the task journals. Persist the
operation ID and payload fingerprint at POST; atomically claim and commit each
ASR, MT and TTS stage before its external call. A stage can be claimed once.
The TTS fingerprint also binds text hash, model, voice and output format.

| Event | Required behavior |
| --- | --- |
| Lost POST response, matching durable row | Return that admission/state; do not create a second operation. |
| Lost POST response, no row | The earlier transaction never admitted a provider call. A genuine first admission is safe. |
| Same ID, different payload | Conflict; no overwrite or provider call. |
| Restart after a provider claim | UNKNOWN; never automatically replay the paid stage. A pre-call crash may also be UNKNOWN. |
| Restart after DONE, text absent | TEXT_LOST/expired result; metadata is not a recoverable translation. |
| Lost speech response | Reuse the same speech ID/state; no automatic synthesis with a new ID. |
| Local replay with existing PCM | Reuse local audio; zero new provider calls. |
| Metadata capacity reached | Reject new admission with BUSY; do not silently evict deduplication evidence. |

Terminal rows may become compact tombstones. Do not automatically discard the
only deduplication evidence after 24 hours. The guarantee lasts while the row or
tombstone remains; explicit administrative removal ends it. A genuinely new
user action may use a new ID and can incur a new charge.

Implemented lifetimes: jobs/text in RAM for 120 seconds and queued server audio
for at most 300 seconds, expiring as `EXPIRED`/`AUDIO_EXPIRED` with restart cleanup.
Uploaded audio is deleted after ASR and provider results are discarded on
cancellation. No text/audio enters SQLite, request logs or backups; metadata contains
only identifiers, hashes, states and timing. Python text references can be released,
but immutable strings cannot be promised secure zeroization. Provider retention
remains a separate unresolved boundary.

## Ownership, resource bounds and controls

S is the outer scratch mutex. Audio A and network N are mutually exclusive phases
inside S; never hold A and N together, or acquire S/A while holding N.

| Phase | Ownership |
| --- | --- |
| Capture | S + A; release A before TLS. |
| Upload/download | S + N; bounded requests; release N before playback. |
| Playback (T2) | S + A. |
| Cleanup | S only; erase data before removing its persistent marker. |
| Weather/completion without scratch | N only; never wait for S/A. |

Existing task voice/download paths must obey the same order. UI exit stops its
timer, invalidates generation and signals cancellation without waiting on the
network. The worker owns cleanup and has no LVGL pointers. Reuse scratch only
after worker confirmation. On cleanup failure, mark the slot quarantined under
S, release S, and refuse further use; never leave an exited worker holding a
mutex. Startup cleanup only addresses a marked inactive slot. Task cache commits
also require S, so an active generation cannot change under a scratch owner.

The following are design bounds, not all measured admission thresholds. Playback and TTS rows are T2 proposals:

| Resource | Proposed bound |
| --- | ---: |
| Capture WAV | 320,044 bytes; erase span including 4 KiB marker: `0x50000`. |
| Playback WAV (T2) | 480,044 bytes / 15 seconds; erase span: `0x77000`. |
| PCM I/O scratch | 512 bytes; no whole-clip RAM. |
| Worker stack / HTTP JSON buffer / view | 8 KiB / 8,193 bytes / about 5 KiB. |
| Parsing headroom after TLS cleanup | Initially budget 12–16 KiB; measure actual peak. |
| ASR / MT / post-record pipeline | 20 s / 15 s / 60 s absolute budget; TTS 20 s remains a T2 proposal. |

T2 only: MiMo's streaming TTS example is 24 kHz PCM16LE mono; inspect actual non-streaming
WAV headers rather than assuming their rate. The server normalizes supported
input to 16 kHz mono PCM16 using a maintained, pinned low-pass/anti-alias resampler.
Do not use removed `audioop` or bare linear interpolation. Validate duration,
bytes, amplitude and 1 kHz / above-8 kHz synthetic tones. Overlong/unsupported
audio leaves usable text and a voice-unavailable status, never partial playback.

| State/event | Behavior |
| --- | --- |
| READY: UP / OK | Switch direction / preflight then record. Offline or untrusted time shows a clear error. |
| RECORDING: OK / 10 s / UP | Finish / finish at cap / cancel. |
| Upload, ASR, MT, T2 synthesis: UP | Cancel, invalidate generation, discard late results. Long OK cancels and exits. |
| RESULT: UP/DOWN | Scroll labeled source and translation. T2 OK requests/reuses synthesis. |
| PLAYING (T2): OK or UP | Stop and return to RESULT. Long OK stops and exits. Target stop latency is at most two seconds, requiring measurement. |
| Failure | Distinguish authentication, quota, temporary failure, expired/unknown result and storage cleanup. No silent billed retry. |

## Implementation and acceptance handoff

T1 is implemented as pure `suite_translation_core.*`, a worker in
`suite_translation.*`, a redesigned `suite_translation_ui.c`, `suite_audio_job`
scratch ownership, separate companion translation adapters/jobs/auth, and
launcher/build registration. LVGL access stays locked and slow work stays outside
callbacks. Static Chinese labels reuse the generated font; dynamic text uses the
existing public font with explicit coverage handling. See the
[T1 core](passport-translation-t1-core.md),
[storage/configuration](passport-translation-device.md) and
[provider/HTTP contract](passport-translation-provider.md) records.

T2 adds synthesis, normalization, bounded download/playback and cancellation. T1
alone does not satisfy SA-15, and T2 is not implemented.

Host acceptance covers actual worker/transport paths with synthetic I/O: limit
edges, malformed/duplicate JSON, identity mismatches, upload/hash conflicts, each
crash/claim boundary, cancellation/late responses, token matrix, metadata capacity,
cleanup quarantine, task data preservation, UTF-8/font coverage and resampling.
Focused suites pass, and the 2026-10-24 native complete gate passed and verified
archive `119f73c2...`; that build was validation-only and was never flashed.

Device acceptance still requires authorized flashing and real audio in both
directions, separate source/translation, long text/glyphs, cancellation in each
stage, repeated entry/exit, TTS pitch/rate/stop latency, offline errors, and
preserved settings, task cache and journals. Measure minimum internal free heap,
largest internal and DMA blocks, stack watermarks, Flash erase/write time and
TLS/audio peaks. Linker size and synthetic tests are not those measurements. The
2026-10-24 OC-11 on-device attempt stopped at readiness before any capture or provider
admission; the device now holds an older placeholder-only build.

Provider/API terms and the SC-08 Flash/protocol exceptions are recorded under
"Approved decisions" above. Independent translation authentication and data
preservation remain engineering requirements, and further real audio or paid calls
need their own authorization.

Current status:

```text
Build: PASS (2026-10-24 native complete gate; archive 119f73c2..., validation-only)
Host tests: PASS (focused translation, shared and companion suites)
Device tests: FAIL for real on-device translation readiness; T2 not implemented
Unverified: provider quality/billing, readiness root cause, true resource peaks,
            T2/TTS, account-region and retention terms
```


## OC-06 decision packet, 2026-10-22

Historical packet. Its provider, capture and retention choices were resolved by the
user on 2026-10-23 (see "Approved decisions" above); the table is retained as a
dated record, not as current pending work.

Decision version: `T1-DP-2026-10-22-A`, reviewed by Codex. OpenCode Go session
`ses_e9c64ac4fffey5pxSI3FepdjI7` used `deepseek-v4.1-flash`, default variant;
actual native CLI was 2.0.22. This packet selects nothing on the user's behalf
and does not implement T1 or authorize real translation calls. Current-firmware
Tasks acceptance proceeds independently.

### Recommended T1 and remaining choices

Keep candidate A: MiMo `mimo-v2.5-asr` for explicit `zh`/`en` recognition and
`mimo-v2.6-flash` for text, both at
`POST https://api.xiaomimimo.com/v1/chat/completions`. Use non-streaming ASR,
JSON-object text output, explicit `thinking: {type: disabled}`, bounded
`max_completion_tokens`, and strict byte/scalar/identity checks. The current
[text API](https://mimo.mi.com/static/docs/api/chat/openai-api.md) defaults
v2.6 thinking to enabled; the [ASR API](https://mimo.mi.com/static/docs/api/audio/Speech-Recognition.md)
supports an explicit language. These official interfaces were rechecked on
2026-10-22; no provider call or quality test was performed.

The [MiMo platform policy](https://privacy.mi.com/XiaomiMiMoPlatform/zh_CN/),
effective 2026-10-10, covers its API platform. It states text is not used for
training without prior consent and describes purpose/legal-need retention,
deletion requests, delayed backup deletion and PRC storage for PRC service use.
It supplies no fixed API TTL; audio-training treatment and the actual account's
region/settings remain unconfirmed. Do not infer a zero-retention contract,
provider cancellation or refund guarantee from this policy.

| Choice | Recommendation | Dependent work if unresolved |
| --- | --- | --- |
| Provider/account terms | Candidate A; confirm actual account region and whether the documented retention and unknown audio-training treatment are acceptable. | Real adapter and real translation audio remain blocked; synthetic independent work is possible only after implementation is requested. |
| SC-08 capture exceptions | Explicitly approve temporary plaintext WAV in an owned inactive Flash slot and one bounded WAV PUT, replacing volatile-only/chunk/finish capture. | Device capture/upload and the corresponding contract edit remain blocked. |
| Real calls | Obtain separate approval for future real translation calls; existing Tasks ASR approval does not select translation. | Real-call acceptance remains blocked. |

A distinct translation capability/token, isolated budgets, preserved settings,
cache/journals/timer and no partition resize remain requirements. A separate
vendor API key is an optional operational choice, not a new mandatory
requirement. TTS voice and retention stay with T2; T1 cannot satisfy SA-15.

### Data and ownership review

Retain the reviewed 10-second/320,044-byte WAV limit, RAM-only text lifetime and
server audio cleanup. Result/audio expiry must preserve a durable deduplication
row or tombstone; it never authorizes automatic replay of a claimed paid stage.
Do not characterize Flash wear as low: erase frequency, cleanup erases and
lifetime remain unmeasured. Adding an owner/version to scratch metadata would
change that metadata; explicitly design legacy-marker compatibility and restart
cleanup while preserving task journal/cache/configuration formats.

Use outer scratch lock S, then audio A or network N; never A+N, and never S/A
while holding N. Task voice/download/cache activation must follow the same
ownership. Worker exit acknowledges cleanup and released ownership before slot
reuse. UI exit cancels and invalidates its generation without waiting; discard
late results. Keep text/audio out of task tokens/journals, logs and durable
translation metadata. Follow the existing Chinese-font checklist for static
labels and explicit missing-glyph handling for dynamic text.

### Scoped implementation handoff after a user choice

```text
Implement only the explicitly approved T1-DP-2026-10-22-A subset. If D1/D2
choices are unresolved, block their dependent real-adapter or capture/upload
work only; do not invent approval. Implementation itself still needs a request.
Use synthetic providers until real translation calls are separately authorized.
Own suite_translation_core/worker/UI, suite_audio_job scratch ownership,
companion translation jobs/auth/mock adapters, minimal task ownership wiring,
fonts/config registration and relevant host tests. Preserve existing settings,
Tasks formats/journals/cache/tokens, timer and partitions.
Cover real worker/transport cancellation, generation/late results, S->A/N order,
worker exit acknowledgement, cleanup quarantine, restart/paid-claim ambiguity,
matching replay/conflicting payloads, token matrix, text/audio limits and glyphs.
Run focused tests and the complete gate; verify exact image/ELF identities.
T2, deployment, flashing, real calls, commits and publication are excluded.
```

The detailed file responsibilities remain in the implementation handoff above.
Build: NOT RUN; Host tests: NOT RUN; Device tests: NOT RUN for this packet.
Unverified: user choices, account terms and all translation implementation,
provider quality, runtime/resource and device behavior.

## OC-10 T1 core/mock checkpoint, 2026-10-23

OC-10 implemented only the provider-independent T1 subset described above; see
[the T1 core checkpoint](passport-translation-t1-core.md). Production provider
selection, account terms, the capture/Flash exceptions, real translation calls,
T2, device acceptance and SA-15 remain unresolved. Post-review corrections
enforce exact descriptor lengths, guard the RAM cache against cancel/publication
races and sweep every expired job at service entry. This checkpoint does not
authorize any of them.

## OC-11 in-firmware UI/worker mock checkpoint, 2026-10-23

OC-11 implemented the recommended local UI increment: a launcher-accessible
translation page with explicit `zh-CN`/`en` direction controls, separate source
and target panes, a visible MOCK badge, processing/failure/cancellation/expiry
states, three-button navigation and scrolling for long text. A bounded worker
uses the existing `suite_translation_core` admission/result bookkeeping with
synthetic input and a replaceable synthetic provider. Every retained plaintext
field, including source text on a partial failure, is RAM-only with a 120-second
observed TTL and is wiped at exit. The eight-slot core table still never evicts
deduplication evidence; after eight distinct mock operations the page reports
`BUSY` and offers an explicit two-press, mock-only reset after worker cleanup.
That reset advances a fresh identity epoch and lets the ninth synthetic job run
without a reboot; it does not touch production/companion stores. Provider output
must be NUL-terminated within the cap; exact 2,048-byte / 512-scalar text is
accepted and unterminated output is rejected. The page and worker do not
implement microphone capture, upload, Flash staging, the Python companion/HTTP
path, production providers, real calls, T2 or TTS. See
[the T1 core checkpoint](passport-translation-t1-core.md).

The trusted user then approved all requested choices and said continue: MiMo
candidate A (`mimo-v2.5-asr` plus `mimo-v2.6-flash`); the disclosed lack of a
fixed retention TTL and unconfirmed audio-training treatment; temporary plaintext
WAV in an owned inactive Flash slot for at most 10 seconds plus one bounded
upload and cleanup; and real-provider, device and paid testing after reviewed
integration. The actual account region is not independently established, so no
regional guarantee is claimed. The later real T1 firmware section implements the
capture/upload/poll adapter, but deployment and actual provider calls remain
root-owned. T2/TTS and SA-15 remain outstanding. DeepSeek
production integration is rejected; the `deepseek-flash` alias has no ASR and the
selected MiMo ASR+MT plan remains unchanged.
OC-09 (Tasks candidate) and OC-10 (T1 core/mock) were completed and independently
reviewed before this increment.

## Real T1 device integration code checkpoint, 2026-10-23

The firmware now contains the authorized real T1 path: MiMo is the UI default,
MOCK remains an explicit local test mode, and a real failure never falls back to
mock. The worker loads the private translation config, checks online/trusted time
and `/ready`, uses the shared `suite_audio_job` lease as `TRANSLATION`, records at
16 kHz/16-bit/mono for 0.3–10 s in <=512-byte chunks, releases BSP audio before
TLS, finalizes/hashes the WAV from bounded Flash reads, performs one POST and one
PUT, releases scratch before polling, and enforces the post-record 60-second
budget. Local cancel/exit wins over late server results. Ambiguous POST/PUT is
UNKNOWN with no retry; partial MT failure preserves valid source under the same
120-second plaintext TTL. The separate server job/generation identity is kept
apart from the local UI session/epoch. No whole WAV is held in RAM.

The shared HTTP transport source landed and the current tree builds/links with it.
The shared protocol now includes EXPIRED; the worker terminates it as expired and
also maps FAILED/AUDIO_EXPIRED to expired. Other protocol parse failures are
UNKNOWN without retry. Root owns final transport verification and the device/paid
tests. Focused host tests, shared tests and a targeted ESP-IDF build pass; device,
real-provider, paid and full-gate results are still not claimed.
