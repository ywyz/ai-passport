[简体中文](passport-device-tasks.zh_CN.md) · **English**

# Device tasks

This increment replaces the task placeholder with a list, details,
manual snapshot download and persistent offline viewing. The existing home,
Wi-Fi and city flows remain. Collection discovery and explicit selection stay
on the [companion side](passport-nextcloud.md). The device combines the selected
collections in server order; details retain the collection ID. The companion
snapshot exposes only unfinished (`NEEDS-ACTION` / `IN-PROCESS`) tasks. A manual
Refresh replaces an older all-task device cache atomically with this active view;
completed/cancelled history remains on the server. The current source also
implements explicitly confirmed completion and durable offline replay for
eligible ordinary Tasks. Collection-name browsing remains later work.

## Current status, 2026-10-25

The accepted Tasks subset stands: reading/details, completion, offline cache and
normal-reboot replay, plus voice preview/default cancel/create replay. Do not
restart that acceptance or add defensive test rounds.

The device currently holds the older Windows build `4b0187a0` (application
`ecbbdc44`, ELF `06e78957`) from the mistaken 2026-10-25 flash; its launcher has
only a translation placeholder, so it cannot currently reach translation readiness.
Recovering the approved OC-11 artifact is the next step; follow
`passport-device-test` and check the target, scope and applicable existing authorization
for that image rather than inferring consent from documentation. Identities are in
[suite-plan section 0](passport-suite-plan.md#artifact-identities-2026-10-25). The
five-minute server refresh timer remains deployed. Task settings, cache, tokens,
journals and translation configuration live in non-application partitions and were
preserved by the segmented writes.

## Dated increment, 2026-10-20

The user removed Deck. Read-only source inspection found one remaining selected
Tasks collection, 150 source records and two unfinished tasks, with no blank
active titles. The live companion selection/snapshot was replaced atomically
to remove the missing Deck collection; no Nextcloud task or credential changed.
Refresh on the installed firmware can download this corrected two-task view.
Unused final-page card slots are hidden in the new UI.

Before this rollout, the installed image was `447198a1c412a70ad209ef3646b732fc20fdbb5fd8d0728367fcd4f6882c2a6f`.
The operator reported the other controls working, with Deck entries and missing
completion as the remaining issues. This is scoped screen feedback; physical
offline/reboot and completion acceptance have not passed. The user subsequently
approved the completion deployment and exact-image flash recorded below; later
operator completion and offline acceptance passed, as recorded in the sections
below and in "Current status, 2026-10-25" above.

Details OK opens an operation menu, initially selecting Return to list. Choosing
Mark complete opens a second confirmation with Cancel selected. Long OK returns
to the list. The selected task ID and ETag are captured before confirmation.
Only the worker writes storage or performs networking. Pending appears after
the intent is committed, and remote success only after a matching server receipt.

At most eight intents live in the encrypted `taskcfg` partition's separate
`taskops` namespace. Each retains its operation ID, sequence, original ETag and
CRC across reboot. Repeated polling does not rewrite unchanged states. Queue
full/storage corruption fails visibly without erasing settings or intents.
Snapshot replacement never clears unresolved operations. Conflict, read-only
and missing-resource requests may be discarded only through explicit confirmation;
there is no active-request cancellation or remote reopening. Refresh/reconnect
can resume paused attempts; automatic success downloads cannot restart them.
An intent whose task disappears from the active snapshot stays reachable as a
clearly labeled retained-request row, including when there are no active tasks.
The toolbar counts tasks and retained requests separately. Its detail can show
the result and confirm discard of a terminal failure without changing Nextcloud.

The task worker stack grows from 6 KiB to 8 KiB for the added bounded credential
and JSON frames; the preceding reader measured about 1.5 KiB spare. New real heap,
stack and Flash-write behavior remain unverified on hardware. Host journal,
runtime and UI tests cover reboot replay, failed commits, capacity, polling wear,
confirmation defaults, captured identity and hidden padding rows.
The missing-source request and explicit discard path is also covered; navigation
allows 200 cached tasks plus eight retained requests without changing cache limits.

## Layout and preservation

`sdkconfig.defaults` selects `partitions.suite.csv`. The upstream minimal
`partitions.csv` remains available and unchanged. This application uses:

| Region | Offset | Size |
| --- | ---: | ---: |
| Existing NVS | `0x9000` | `0x6000` |
| Existing PHY | `0xf000` | `0x1000` |
| Factory application | `0x10000` | `0x5b0000` |
| Encrypted task configuration | `0x5c0000` | `0x3000` |
| Task configuration keys | `0x5c3000` | `0x1000` |
| Task cache, two slots | `0x5d0000` | `0x230000` |

Each cache slot is `0x118000` bytes: a header sector, up to 200 records/1 MiB
of record data, and an independent 32 KiB page scratch region. Inactive-slot
erase/write never touches the active slot or either NVS partition. Response
chunks stream to scratch; TLS is released before record parsing. Pages must
agree on revision, selection revision, source timestamp, count and cursor.
Schema, read-only markers, UTF-8 and field limits are checked. Full payload
CRC/readback precedes the final header write. At restart, header and payload
CRCs select the newest valid generation; interrupted or corrupt candidates fall
back to the other valid slot. CRC detects corruption, not malicious modification.
The header write is the commit point; interrupted acknowledgement can leave a
complete new generation recoverable after reboot.

Default NVS initialization now fails without automatically erasing Wi-Fi/city
settings. Task storage errors never format default NVS. Before an approved
upgrade, verify that the running application's actual end precedes the new
configuration/cache regions. Use segmented bootloader/table/application writes
plus the separately reviewed configuration images. Never use a padded merged
image or full-chip erase to preserve existing NVS. Device preservation and real
power-loss behavior still require physical acceptance.

## Private token setup

Use the activated ESP-IDF 5.5.3 Python:

```text
python tools/configure-task-device.py --origin https://tasks.example.test --companion-config <private-config.json> --output <new-private-directory> --probe
```

The tool copies `device_token` and optional `completion_token`, checks all are
distinct from one another and the admin token,
checks the HTTPS origin and token syntax, optionally verifies the read API, and
prepares AES-XTS encrypted NVS plus a fresh cryptographically random key image.
Without `--companion-config`, it uses a hidden read-token prompt. The tool never
flashes a device, replaces an existing output directory or places tokens in
command arguments. It restricts the new directory to the operator/system/admins
on Windows, or uses 0700/0600 on POSIX, and removes its temporary plaintext CSV.
The optional writer is stored as `write_token`; the reader stays read-only.
The manifest records offsets and image hashes without credentials. Keep both
images private, separate from firmware/debug archives, and review them before
the separately approved USB write. The settings website, QR and ordinary BLE
commands cannot configure this token. Nextcloud credentials stay on the server.

These images replace the entire task configuration and key partitions. The
initial completion upgrade is from the installed reader with no completion
journal: preserve the existing origin/read token while adding the writer. Future
credential rotations must preserve any pending `taskops` records; this generator
is not an in-place queue migration tool. Wi-Fi/city NVS and task cache are outside
these image ranges, and the partition table is unchanged.

This is a physically controlled development provisioning boundary. The key is
stored on Flash; hardware Flash encryption, secure boot, device/owner binding,
physical extraction resistance and signed trusted time are **not** implemented.
Encrypted NVS alone does not provide those properties. The task cache itself is
plaintext. No eFuses are changed. Do not use this path for OTP seeds.

HTTPS requires the existing trusted-time state and CA bundle. Redirects are
disabled, credentials use the Authorization header only, each request has a
10-second timeout, and a download has a 90-second deadline. Weather and tasks
share a TLS ownership mutex. Authentication rejection preserves cached tasks and
halts further requests until configuration is repaired and the device restarts.
Cancellation and errors retain the previous active generation. Workers never
reference UI objects, and page timers are deleted before their screens.

## Pages and freshness

Refresh is pinned above the task cards and focused on each new list visit,
whether the cache is empty or populated. UP/DOWN cycles through Refresh and all
tasks; UP from the first task selects Refresh, and DOWN from the last task wraps
to Refresh. OK on Refresh downloads the committed server snapshot; OK on a task
opens details. Returning from details retains the selected task. Fixed actions,
states, errors and detail labels use Simplified Chinese, with explicit key hints.
During download, OK requests cancellation. Long
OK returns to the launcher. Details show title, status, due kind, normalized and
original due value, original timezone, collection ID, notes and source/device
timestamps. UP/DOWN scrolls and moves through UTF-8-safe note pages; OK opens
operations and long OK returns. Recurrence, subtasks and source note truncation
are labeled. Completion is limited to owner-writable, simple nonrecurring tasks
with a strong ETag. Old snapshots without eligibility metadata remain readable
and require Refresh before completion. Refresh downloads the server's current committed cache;
it does not start a new CalDAV job. The companion refreshes the snapshot from
Nextcloud five minutes after each completed job, and the operator can still start an
on-demand server refresh as described in the companion guide.

Source fetch time and device commit time are separate. Offline, stale after
24 hours, uncertain time, empty, unconfigured, authentication, cancellation and
storage failures remain visible without discarding valid cached tasks.
LVGL uses its C-library allocator so page objects share the ESP-IDF heap with
networking instead of reserving an independent 48 KiB static pool. The page
stack deletes the previous screen before creating the next. Weather and task
TLS owners remain serialized; request/result logs expose only heap sizes,
cached counts, result codes and task stack margins. Heap fragmentation and
UI/network coexistence still require on-device checks, including the large
city picker and long task details.
Dynamic task text uses a public Noto Sans SC 16 px, 2-bit font with 21,393
codepoints. It includes ASCII, punctuation, CJK Unified Ideographs and fullwidth
forms; it does not cover all Unicode. Missing glyph placeholders remain enabled.
The public ranges and source/license are in the [font record](../../assets/fonts/README.md).

## Acceptance boundary

Host checks exercise the real parser/cache, interrupted headers, corrupt-slot
fallback, revision/cursor/size errors, UTF-8 and note limits, HTTPS/time/offline
gates, redirect rejection, incomplete/oversized responses, cancellation, auth
halt and resource cleanup. Synthetic tests do not establish hardware acceptance.
Private HTTPS probing and real-snapshot host replay are separately recorded in
ignored build output; no private text or character inventory enters source.
Device tests must verify dynamic Chinese, navigation, heap/stack margins with
weather/BLE, Wi-Fi/city preservation, reboot recovery and interrupted writes on
the exact validated and explicitly approved firmware.

## Verified build, 2026-10-19

- Build: PASS; complete `./tools/validate.sh` exited 0 with ESP-IDF 5.5.3.
- Host tests: PASS, including cache/network/NVS failures and companion regressions.
- Device tests: FAIL for weather HTTPS on this build; segmented flash, startup,
  saved Wi-Fi reconnection and SNTP synchronization passed. Task acceptance awaits
  operator actions and screen observations.
- Unverified: physical task download/rendering, task stack margins, offline/reboot
  cache recovery, power interruption and the saved city shown on screen.
- Private real-data host replay: 154 records/five pages; integrated font missing count 0.
  The private token images also passed encryption/decryption comparison.
- Application: 3,507,344 bytes in a 5,963,776-byte factory partition;
  merged image: 3,572,880 bytes. Version: `21c5110-dirty`.
- Full-image SHA-256: `6ada8c477b8f742547fea042823425323e4d4140a50c0b0bf5e77df61593d94f`.
- Matching ELF SHA-256: `da0842543c54872152f4a75406c891af445fffc0f4e36d0bc8e24b1eb7b18f44`.
- Verified archive: `build/firmware/6ada8c477b8f742547fea042823425323e4d4140a50c0b0bf5e77df61593d94f/`.
- Owned-source inventory SHA-256: `5d230a944843a527b92b9db5d6a8703800cc00e8fbf7f11153505bbf39bf869c`; this covers application/BSP
  sources, selected layout, defaults and dependency/build inputs, not the toolchain.

The user approved and completed the local write plan
`build/acceptance/20261019-device-tasks/flash-plan.json` on COM3. All five segmented
bootloader/table/application and separate private configuration/key images passed
the tool's data-hash verification. Erased/written sectors excluded `0x9000` to
`0x10000` (existing NVS/PHY); no merged image or full erase was used.
Compiler cache settings temporarily used for validation were restored afterward.

A bounded 45-second reset/startup observation matched the ELF hash prefix,
reached application readiness, reconnected saved Wi-Fi and synchronized SNTP,
without panic markers. Weather HTTPS failed during certificate verification:
`0x4290` combines Mbed TLS RSA public-operation failure (`0x4280`) with MPI
allocation failure (`0x0010`). Before the request, free heap was 31,136 bytes and
the largest internal block was 14,848 bytes. This is an observed memory failure,
not successful weather acceptance. Raw serial evidence remains in ignored local
build output. The monitor released COM3 after the observation; no replacement
firmware has been flashed.

The operator reported a correct home screen, weather waiting for refresh and
task `Refresh failed`. One additional 40-second observation captured the manual
task retry: the same certificate `0x4290` allocation failure, result 7, zero
cached tasks and 1,560 bytes of remaining task stack. No offline snapshot was
created, so detail/offline acceptance cannot proceed yet. The subsequent
allocator repair requires a separately verified build and fresh flash consent.

## Memory repair validation, 2026-10-19

- Build: PASS; complete `./tools/validate.sh` exited 0 with ESP-IDF 5.5.3.
- Host tests: PASS, including TLS-open allocation failure retaining the committed
  cache and releasing the HTTP client/network owner.
- Device tests: FAIL for task download; segmented flash, matching-ELF startup,
  saved Wi-Fi reconnection, SNTP and weather HTTPS update passed. Failure details
  are recorded below; list/details/offline acceptance is pending.
- Unverified: task download/list/details, offline/reboot cache, task heap/stack
  margins and city-picker/UI/network coexistence.
- Generated configuration selects `LV_STDLIB_CLIB`; the linked allocator comes
  from `lv_mem_core_clib.c`. ELF `.dram0.bss` fell from 90,080 to 40,904 bytes
  (49,176 bytes reclaimed), and the 49,152-byte built-in pool symbol is absent.
  These are link-time measurements, not runtime free-heap measurements.
- Application: 3,505,152 bytes; merged image: 3,570,688 bytes.
- Full-image SHA-256: `8d985f0c7636abb5fc0d7b1c0f3ab5b80f7e6a06a176395b0abcaf4dcfe71891`.
- Matching ELF SHA-256: `17b92eac48d47bc2f33d0edc62019ba240222d3f235b8069e5148087a21d8d70`.
- Verified archive: `build/firmware/8d985f0c7636abb5fc0d7b1c0f3ab5b80f7e6a06a176395b0abcaf4dcfe71891/`.
- Source-inventory SHA-256: `7b3b7bb06ff5414dcc380ca120e26635aa02a628fac69ce2e3f4d288e22cf755`.

The user approved and completed the COM3 plan
`build/acceptance/20261019-device-tasks-memory/flash-plan.json`. It wrote only
the verified bootloader/table/application components; the partition table is
byte-identical to the installed table. Existing NVS/PHY, task configuration,
keys and cache sectors are excluded. Private credential literal scanning passed.
No new token images were needed. All three data hashes verified. The bounded
45-second startup observation matched ELF prefix `17b92eac4`, reconnected saved
Wi-Fi, synchronized time and recorded a successful weather update with no TLS
errors or panic markers. Before weather HTTPS, free heap was 61,540 bytes and
the largest internal block was 49,152 bytes (previously 14,848). Task download,
details and offline persistence still require the pending operator actions.

The operator then confirmed displayed weather but another failed task refresh.
Three fully captured retries passed certificate verification and failed TLS
reads with `-0x7100`, result 7 and zero cached tasks; task stack margin was
1,548 bytes. Complete samples before download showed about 69 KiB free with a
26,624-byte largest internal block. Device tests for this image are therefore
FAIL for task download; the earlier weather/startup PASS remains scoped.
A private, certificate-verified TLS 1.2 probe observed an 8,277-byte encrypted
application record in a 32,143-byte HTTP response. The former 4 KiB TLS input
limit cannot accept these records, regardless of the application's 512-byte
reads. The next repair restores the standard 16 KiB content limit and enables
ESP-IDF's dynamic record buffers; HTTP/page/cache bounds remain unchanged.

## TLS record repair build, 2026-10-19

- Build: PASS; complete `./tools/validate.sh` exited 0 with ESP-IDF 5.5.3 in a
  byte-verified local Linux snapshot. Original workspace build inputs matched
  afterward; the copied debug archive also passed verification.
- Host tests: PASS, including partial-response transport errors preserving the
  committed cache and releasing the client/network owner.
- Device tests: PASS for the approved three-component flash, matching-ELF
  startup, saved Wi-Fi reconnection, SNTP, weather HTTPS and the operator-confirmed
  manual download/list of 154 tasks. The list included completed history and needs
  the companion active-view correction; detail/offline acceptance remains pending.
- Unverified: corrected task list/details, offline/reboot
  cache, task heap/stack margins and UI/network coexistence.
- Generated configuration: 16,384-byte TLS input content limit, dynamic TX/RX
  buffers, full certificate bundle and `LV_STDLIB_CLIB`; automatic freeing of
  certificate/configuration data is not enabled. HTTP bounds remain unchanged.
- Static `.dram0.bss`: 40,904 bytes; no built-in LVGL pool symbol.
- Application: 3,509,440 bytes; merged image: 3,574,976 bytes.
- Full-image SHA-256: `6235486f2da35bf145b59294efb8d31838d55b270ac723446d722d4862ee11a8`.
- Matching ELF SHA-256: `58470d34fe6fe7df7c4647900ade890f6580c48371a720a895ad925b4c831233`.
- Verified archive: `build/firmware/6235486f2da35bf145b59294efb8d31838d55b270ac723446d722d4862ee11a8/`.
- Source-inventory SHA-256: `f556c291c52884423c7c7c683aeaeb3725cad2855daa14d81c738c7148c85bf2`.

The user approved and completed
`build/acceptance/20261019-device-tasks-tls/flash-plan.json` on COM3 on 2026-10-20
(Asia/Shanghai): bootloader/table/application only, with all three data hashes
verified. The table is byte-identical; NVS/PHY, task token configuration, keys
and cache sectors were excluded.

A bounded 45-second startup observation matched ELF prefix `58470d34f`, reached
application readiness, reconnected saved Wi-Fi, synchronized time and updated
weather over HTTPS without TLS errors or panic markers. Before weather, free
heap was 61,376 bytes and the largest internal block was 49,152 bytes. Task
download subsequently succeeded: the operator reported 154 displayed tasks,
including completed history. The stored source snapshot contains 148 `COMPLETED`
and six `NEEDS-ACTION` records. This supports pending-only companion filtering;
it does not establish detail rendering or offline persistence. The silent 60-second
windows contain no task resource measurements. Every observation
releases COM3 afterward; raw logs remain in ignored local build output.

## Active task view correction, 2026-10-20

- Build: PASS; complete gate and copied firmware archive verification passed.
  Firmware sources were unchanged; the rebuilt gate artifact was not flashed.
- Host tests: PASS, including 30 companion tests, filtered multi-page cursors,
  terminal-only empty state, unchanged original history, old cursor rejection
  and real six-item parser/cache replay with reopen and record reads.
- Device tests: NOT RUN for the corrected view. The existing TLS-repair firmware
  remains installed; its operator-confirmed 154-item download passed.
- Unverified: refresh to six active items, Chinese detail/return rendering,
  offline refresh retention and reboot recovery, live task heap/stack margins.

The companion update now returns six unfinished tasks in one `READY` page over
verified public HTTPS. Server database/configuration/proxy/provisioning hashes
were unchanged. Details and offline tests require operator actions; no additional
firmware or token write is needed for this correction.

## Refresh controls and Simplified Chinese, 2026-10-20

- Build: PASS; complete gate with ESP-IDF 5.5.3, copied archive verification,
  unchanged partition table and private credential literal scan passed.
- Host tests: PASS; the actual page handlers cover empty, 1/6/154/200-item lists,
  initial Refresh focus, both navigation directions, detail return/scrolling,
  cancellation, asynchronous count replacement and page/timer cleanup.
  Static font maps cover all declared text and printable ASCII; known missing
  emoji remain a negative check. Physical rendering is separate.
- Device tests: PASS for the approved COM3 three-component write, matching-ELF
  startup, saved Wi-Fi reconnection, SNTP and weather HTTPS. The task controls,
  detail rendering and offline acceptance still await operator observations.
- Unverified: populated-cache refresh, Chinese list/detail/status/error layout,
  home/weather/city UI with the expanded font, offline/reboot cache and live
  heap/stack margins. Existing Wi-Fi/city/token/key/cache regions are excluded
  from the completed writes.
- Application: 3,523,264 bytes; merged image: 3,588,800 bytes, 13,824 bytes larger
  than the installed TLS repair. No cache schema or credential layout changed.
- Full-image SHA-256: `447198a1c412a70ad209ef3646b732fc20fdbb5fd8d0728367fcd4f6882c2a6f`.
- Matching ELF SHA-256: `f3cff97f29f40bea889947de884be682fcda54107fd47e45fe7505b74c48776d`.
- Verified archive: `build/firmware/447198a1c412a70ad209ef3646b732fc20fdbb5fd8d0728367fcd4f6882c2a6f/`.

The fixed Refresh card is now above the three task cards and initially selected
on new visits. Detail return preserves its selected task. Fixed UI labels use
the regenerated 16 px static font (20 px line height); dynamic task text retains
the public task font (31 px line height). The pinned toolbar, timestamps and
footer use separate layout budgets. PRD SU-16 records the confirmed requirement
to prefer Simplified Chinese throughout current and future user-facing UI.

After the user's exact-build approval, all three image hashes verified on COM3
on 2026-10-20. The unchanged partition table and rounded erase ranges exclude
NVS/PHY, task configuration, keys and cache. A bounded 45-second normal-reset
observation matched ELF prefix `f3cff97f2`, reached application readiness,
reconnected saved Wi-Fi, synchronized SNTP and updated weather over HTTPS.
It contained no panic markers or TLS errors. Before weather, free heap was
61,516 bytes and the largest internal block was 49,152 bytes. No task refresh
was captured in that startup window. The serial port was released afterward;
private raw logs remain local.

## Completion candidate validation, 2026-10-20

- Build: PASS; final complete `./tools/validate.sh` with ESP-IDF 5.5.3,
  independent archive verification, unchanged partition table and source/binary
  private-literal checks. A preceding candidate is superseded by this final
  build, which also keeps missing-source requests reachable.
- Host tests: PASS; 42 companion tests on Linux without skips, device journal,
  parser/cache, HTTP/NVS runtime and actual UI handlers, including capacity,
  default-cancel confirmation, orphan requests and 208-row navigation. Static
  font coverage/layout passed after adding 50 completion UI codepoints. The real
  two-task projection with eligibility/ETags passed the device parser, commit,
  reopen and record reads; no real completion was performed.
- Device tests: PASS for the approved five-segment COM3 write and matching-ELF
  startup only: saved Wi-Fi reconnection, SNTP and weather HTTPS. Physical task
  reading/completion and offline acceptance of this build are pending.
- Unverified: physical complete/confirm UI and Nextcloud write, offline reboot/
  durable replay, concurrent website-edit conflict, live heap/stack with
  weather/BLE, physical power cuts and rendering of the added labels.
- Application: 3,545,456 bytes in 5,963,776 bytes; merged image: 3,610,992 bytes.
  Compared with the installed reader, application grows 22,192 bytes, static BSS
  grows 4,504 bytes and the worker stack grows 2,048 bytes. IRAM text/static data
  are unchanged; these linked sizes are not a runtime free-heap measurement.
- Full-image SHA-256: `d5779c7e83ce1d9727c8044614e249fbf60398a7616832e5d40556481f397ad0`.
- Matching ELF SHA-256: `4c71f3582adba9c35f5b314954f14f3e6167c315c1a4d4b778c5807e81f5ac68`.
- Verified archive: `build/firmware/d5779c7e83ce1d9727c8044614e249fbf60398a7616832e5d40556481f397ad0/`.
- Companion candidate SHA-256: `cfb330b42e5e56fd49e532b01775d8383d17b25a961c2d0da120bc74ce3ae211`.
  Packaged sources match the gate; all existing credentials remain unchanged,
  with one new independent completion token. Server clock synchronization and
  original source/overlay/proxy/provisioning preconditions passed inspection.

After exact-build consent, the completion API was enabled and all five segmented
COM3 writes verified: bootloader, partition table, application, encrypted task
configuration and new key image. Origin/read token values remain unchanged;
Wi-Fi/city NVS, PHY and cache sectors were excluded. The former reader had no
completion journal to migrate. Both encrypted images passed actual decryption
comparison; temporary plaintext was removed. No merged/full-chip write, eFuse
change, commit, push or publication occurred. Real task completion is chosen
and confirmed by the operator on the device.

A bounded 45-second normal-reset observation matched ELF prefix `4c71f3582`,
reached readiness, reconnected saved Wi-Fi, synchronized time and updated weather
over HTTPS without TLS/panic/error markers. Before weather, free heap was 54,704
bytes and largest internal block 43,008 bytes. No task refresh/completion occurred
in that window. A following 60-second operator-action window captured zero bytes;
silence does not establish task success. Both windows released COM3 afterward.

Loopback and verified external HTTPS passed read/admin completion rejection,
writer snapshot rejection and authenticated operation lookup. Deployment retained
all existing credential values, selected Tasks collection, proxy and provisioning
files. Baseline SQL audit showed two active tasks, 150 retained source records,
no completion intents and no auth halt; measured service memory was 36,913,152
bytes, not a load-test peak. Local restricted configuration now matches the
deployed candidate. Online screen/complete feedback is requested before offline
reboot/replay and concurrent-edit testing; no real task was changed by the tools.

## Cached completion-read repair, 2026-10-20

The operator reported the other functions normal, but confirmation displayed
conflict. Read-only evidence found two receipts for the same task. Fresh CalDAV
REPORT showed completed/100 percent/UTC and a new ETag; GET returned the previous
unfinished resource even with no-cache headers (Age 546–547 seconds). One submitted
resource hash exactly matched the live completed data. The task had been completed;
the stale verification read produced a false conflict. This is a failed original
completion-result test, not a reason to complete the task again.

The [companion fix](passport-nextcloud.md#deployment-facts-and-boundaries) reads
each resource/ETag through calendar-multiget REPORT and restores canonical CRLF
for exact hash recovery. Its deployed `caldav.py` SHA-256 is
`cc710991d9a5fe8f617a71ae6ba3802f1120c3ea0cce0fd781e50e68f67c6df8`,
an overlay on bundle `cfb330b4`. Conditional PUT, credentials, proxy/provisioning,
Wi-Fi/city and device journal are unchanged. A read-only reconciliation corrected
only the exact-hash-proven receipt to SUCCEEDED; the other CONFLICT was retained.
Both receipts remain, and the refreshed snapshot contains one active task.
No further Nextcloud mutation, Flash write or intent deletion occurred.

- Build: PASS; full ESP-IDF 5.5.3 gate and archive verification. The gate-only
  archive `build/firmware/54eaf8ac151527af8edcb172b29b5021ebd56d4aee3e78abd4fb5199f6cc23a2/`
  retains matching ELF `b9bc17e9936f3ef35670f7483668df7cf9370ba480d001c253bfb3d730ebca84`;
  it was not flashed because firmware inputs are unchanged.
- Host tests: PASS; 45 companion cases on Linux, including stale GET and normalized
  line-ending/lost-response recovery, plus complete repository regressions.
  Live multiget, loopback and public HTTPS read/scopes checks passed.
- Device tests: NOT RUN for repaired completion. The prior online test exposed
  the false conflict; the existing `d5779c7e` firmware/ELF stays on COM3.
- Unverified: clearing the retained old device conflict, post-repair completion,
  offline cache/reboot/replay, genuine concurrent-edit conflict and live stack margins.

The user chose the remaining completable task for the next acceptance round.
First Refresh and explicitly discard the old terminal request through its
default-cancel operation menu; this changes only the local journal. Keep the
remaining task unfinished for offline/reboot validation before another real
completion. Device/UI observations are pending user feedback.

## Offline reboot and replay acceptance, 2026-10-21

The operator confirmed the obsolete conflict cleared and one ordinary task left.
With the original AP disabled, cached list/details stayed readable and Refresh
showed offline/cache without deleting the item. Explicit completion showed the
pending badge. A COM3 normal reset retained the readable cache and pending intent.
The 45-second observation matched ELF prefix `4c71f3582`, reached readiness,
remained offline and had no panic/NVS initialization failure; COM3 was released.
The device was not reflashed and settings/configuration/cache were not rewritten.

After the user restored the original AP, that request completed automatically.
The user confirmed no conflict, zero remaining tasks after Refresh, and the same
completion on the Nextcloud website. Read-only server audits showed active tasks
1 to 0 and SUCCEEDED receipts 1 to 2, with the old CONFLICT receipt retained.
This passes the installed firmware's cache, normal reboot and durable replay
subset. A serial window ended before reconnection and captured no completion;
live heap/stack, physical power cuts and genuine concurrent-edit conflict remain
unverified. Voice creation (SU-17/SC-06.1) is a new implementation, not covered by
this completion acceptance.

## Voice creation code validation, 2026-10-21

The SU-17 candidate now implements the persistent top voice action, bounded
capture, Chinese transcription preview, default-cancel creation confirmation,
one durable encrypted creation intent and idempotent server replay. Enter the
voice action directly with UP from Refresh, including when the task list is
empty. Click OK starts/stops capture; preview OK opens confirmation, UP/DOWN
select, OK confirms; long OK cancels unconfirmed work and returns. A confirmed
request survives page exit/reboot; only explicit success acknowledgement or
proven terminal rejection permits a new capture. Existing Refresh stays the
default one-click action. The worker owns audio/Flash/network, with no UI pointer.

- Build: PASS; complete ESP-IDF 5.5.3 gate and independent archive verification.
  Full image SHA-256 `10f8cb212ed2209916a6cc315dd10d77f5da57e3435559a051c54d06666ef3f5`;
  matching ELF SHA-256 `d7654fa9d54cc759db7c5307372563cf64f7911f0d2ae568c79fe48a5a501c7d`.
  Application 3,623,072 bytes / factory 5,963,776 bytes; merged 3,688,608 bytes.
  Archive `build/firmware/10f8cb212ed2209916a6cc315dd10d77f5da57e3435559a051c54d06666ef3f5/` retains the matching images, ELF and MAP.
  The earlier `6e83bcea` candidate is superseded and was never flashed.
- Host tests: PASS; complete repository gate, 56 companion tests on Linux with
  zero skips, actual voice worker/UI, cancellation before and during capture,
  damaged NVS preservation, cache isolation/cleanup, commit failure and reboot
  replay. ASan/UBSan voice-core and actual-worker checks passed. Sixteen public
  fixed-font codepoints were added/regenerated; both font bindings were checked.
- Device tests: NOT RUN for this new voice build. The previous installed
  completion/cache/reboot/replay subset above passed; it is not voice acceptance.
- Unverified: actual microphone/stop, MiMo key/TLS/recognition/billing, Chinese
  voice-page rendering, genuine Nextcloud creation and offline creation replay,
  integrated memory/stack/BLE/weather, physical power cuts and genuine edit conflict.

The partition table is byte-identical to the installed d5779c7e layout. Planned
COM3 writes contain only bootloader, table and application; rounded erase ranges
exclude Wi-Fi/city NVS, PHY, task credentials, key partition, cache and operation
journals. No device-configuration rewrite is required. Static IRAM text grows
2,420 bytes, DRAM data 16 bytes and BSS 2,552 bytes; worker stack remains 8 KiB.
These are linked sizes, not measured runtime heap/stack. COM3 identity was
enumerated read-only. New service package SHA-256 is `6a711c64a64427659552173439b7658724b17a4c8c4f3b2450bdb14b050e1166`.
Sources and artifacts passed private-literal scans, and the bundle manifest
matches the gate. Neither the new package nor firmware has been deployed/flashed;
no real provider call or real task creation was performed. A server-only MiMo key
and explicit exact-candidate deployment/flash approval are pending.

## Approved voice deployment and startup, 2026-10-21

After the user's exact-candidate approval, the MiMo key was entered in a local
hidden-input console with owner-only staging permissions, installed as the
service-owned 0600 speech config, and excluded from source/images. Local and
server upload staging copies were removed after verification. The active key
exists only in the private server config. It was never printed or flashed.

Service bundle 6a711c64 was deployed with additive creation/usage tables and
rollback code backup. Existing credentials, the one selected collection, all
three completion receipts and provisioning files were preserved. The narrow
audio proxy route passed Caddy validation/reload. Loopback and public verified
HTTPS passed read/admin rejection, writer read rejection, readiness, maximum-size
invalid WAV and oversized rejection checks. No provider call, billed audio or
task creation occurred during those deployment probes.

COM3 then received the approved 10f8cb21 firmware in three verified segments:
bootloader sectors 0x0..0x5fff, table 0x8000..0x8fff, application
0x10000..0x384fff. Other partitions and their keys/journals were excluded.
A 45-second normal-reset window matched ELF prefix d7654fa9d, reached readiness,
reconnected saved Wi-Fi, synchronized time and updated weather over HTTPS with
no panic, TLS or error markers. Before weather, free heap was 49,424 bytes and
largest internal block 36,864 bytes. COM3 was released. This is PASS for flashing
and that startup subset only; voice capture/ASR/preview/confirmation/creation
and creation reboot replay are pending operator feedback and subsequent evidence.

## Voice preview acceptance, 2026-10-21

The operator confirmed actual recording/recognition returned a legible Chinese
test title, the page state and text were normal, and default Cancel worked.
No task was created. A read-only audit independently found zero creation intents
and zero active tasks, with five audio seconds charged by the service's admission
budget. This establishes actual microphone-to-MiMo-to-preview and default-cancel
acceptance; it is not a provider invoice or a measured audio/heap peak. The
three existing completion receipts remain unchanged. The next step retains the
preview, confirms creation offline, checks normal reboot persistence, then
restores connectivity for conditional/idempotent creation replay.

## Submission after recognition, 2026-10-21

The operator confirmed the recognized draft was saved pending synchronization
after disconnecting and confirming creation. A read-only server audit still
showed no creation intent and zero active tasks. The operator correctly noted
that this is not offline speech recognition: MiMo needs a network connection.
The acceptance scope is preservation/replay when connectivity is lost after
recognition, during submission. Local offline recording/transcription is not a
feature of this increment. A normal COM3 reset is being used only to finish the
already queued request check; no additional firmware change is involved.

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

## Reliability candidate flash and startup, 2026-10-22

The user approved the exact verified `7c292cfc/e8b6ae41` firmware and a segmented
flash preserving configuration and task data. The unique matching Espressif USB
Serial/JTAG device was COM3; ROM probes confirmed ESP32-C3 revision v1.1 and
8 MB Flash. Project checks and artifact preparation ran in the independent
WSL-native checkout. Because USB forwarding was unavailable, only the physical
serial operations used the host's ESP-IDF esptool 4.12.0 and verified temporary
copies of the three component images; no rebuild was performed.

The complete current 4 KiB partition sector matched the verified candidate table
plus erased padding byte for byte. The application partition starts at 0x10000;
its existing layout and every data partition remain unchanged. Actual sector
erasure, three component writes, and a separate `verify_flash` matched:

| Component | Offset | Bytes | Erased sectors, inclusive |
| --- | --- | --- | --- |
| Bootloader | 0x0 | 21,024 | 0x0..0x5fff |
| Partition table | 0x8000 | 3,072 | 0x8000..0x8fff |
| Application | 0x10000 | 3,624,704 | 0x10000..0x384fff |

These ranges exclude Wi-Fi/city NVS, PHY, task configuration, task keys, cache and
operation journals. No data image, merged image, whole-chip erase or eFuse write
was used. The table read was for compatibility, not a firmware backup.

- Approved full-image SHA-256: `7c292cfc48ed58de50ffa71d3fb5dafb0c88cddb21f5d2289588780f51819d51`.
- Matching ELF SHA-256: `e8b6ae41f0bd32690107aafb73bb3990836d688a0349700432f5ddc8fdb3521a`.
- Verified bundle: `build/firmware/7c292cfc48ed58de50ffa71d3fb5dafb0c88cddb21f5d2289588780f51819d51/`.
- Local plan, layout audit, write/verification logs and private startup log:
  `build/acceptance/20261022-oc-device/`.

A 90.52-second normal-reset window reported matching ELF prefix `e8b6ae41f`, one
boot, application readiness, connection using saved Wi-Fi and a successful HTTPS
weather update. No error-level log, crash or reboot loop was observed. Before
weather, free heap was 49,280 bytes, largest internal block 36,864 bytes, DMA free
41,524 bytes and largest DMA block 36,864 bytes. These are samples, not measured
peaks. COM3 was released after the bounded window; raw logs remain local.

Build: PASS (previous complete gate; the exact archive was reverified).
Host tests: PASS (28-group default/optimized suite, pure voice tests and complete
gate with 64 companion cases and zero skips).
Device tests: PASS only for the approved three-segment flash, independent digest
verification and the startup/network/weather subset above.
Unverified: operator confirmation of retained task data, recording/Chinese
preview/Cancel, new error-label rendering, measured stop/cancel latency,
heap/DMA/stack peaks, physical power cuts and mixed operation. The operator has
been asked to check retained tasks and recording/preview/Cancel without
confirming a new task; no new task creation is claimed.

## Source freshness diagnosis, 2026-10-22

The operator confirmed original tasks remain visible. The first refresh-failed/
keep-cache event was not captured, so its cause remains unknown. A captured
retry returned `Refresh result=2, cached=1`, without errors, proving the one-task
snapshot downloaded and activated. Before a one-time source read, the server
snapshot was about 22.4 hours old; afterward its API returned six current active
tasks. The operator subsequently confirmed that the six latest tasks display
correctly on the device. The later 120-second passive window captured no further
refresh; no reset was requested and the serial port was released.

See the [source-sync diagnosis](passport-nextcloud.md#source-freshness-diagnosis-2026-10-22)
for server counts, the OpenCode Go review, Codex verification and policy limits.
Raw serial logs remain in ignored `build/acceptance/20261022-task-refresh/`.
Firmware remains `7c292cfc/e8b6ae41`; this diagnosis did not reflash, rewrite
configuration or confirm a new task.

Build: PASS. Host tests: PASS, from the unchanged firmware's complete gate.
Device tests: PASS for visible original cache, the captured one-task download and
the operator-confirmed six-task display.
Unverified: first failure cause, recording/preview/Cancel, error-label rendering,
measured latency/memory peaks, power cuts and integrated operation.

### Five-minute server timer deployment, 2026-10-22

The user-selected server read timer is deployed and boot-enabled. Two automatic
reads succeeded with 304 seconds between snapshot publications; the latest server
API returned seven active tasks. Configuration, selection, authentication and
business receipt hashes were preserved. See the
[companion deployment record](passport-nextcloud.md#five-minute-timer-deployment-2026-10-22)
for the scope and isolated scheduling fixture. Device Refresh still manually
downloads the committed snapshot; the operator confirmed the earlier six latest
tasks display correctly. No new flash or device configuration rewrite occurred.
Scheduling does not explain the unlocated first sporadic refresh failure.

Build: PASS. Host tests: PASS (complete gate, 64 companion cases, zero skips).
Server synchronization: PASS for two automatic reads, effective configuration and
state preservation.
Device tests: PASS for the earlier flash/startup subset and operator-confirmed
six-task download/display.
Unverified: initial sporadic failure, recording/preview/Cancel, error-label
rendering, measured latency/memory peaks, power cuts and integrated operation.


## OC-04 current-firmware acceptance, 2026-10-22

This round tested the installed `7c292cfc/e8b6ae41` firmware. No new firmware,
configuration or data image was flashed. Source/build/test operations and all
evidence files stayed in the independent WSL-native checkout; only USB/serial
access used a host hardware bridge. The actual OpenCode CLI was 2.0.22.

| Check | Observed result |
| --- | --- |
| Manual Refresh and initial voice page | Operator confirmed normal behavior and a ready-to-record page. Later operator confirmation explicitly established seven displayed tasks. |
| Capture cancel and immediate re-entry | Operator confirmed long-press cancellation during capture, successful immediate re-entry and a usable next recording. |
| Chinese preview and default Cancel | Operator confirmed both actions normal using fictional test speech. Cancel closed the confirmation while preserving its preview, as implemented; it did not confirm creation. |
| Leave preview and reopen | Operator confirmed the preview cleared on page exit/re-entry. |
| Repeated navigation | Approximately ten voice-page entry/exit cycles and the final manual Refresh were operator-confirmed normal; seven tasks remained displayed. |
| Offline cache and reconnect | Operator disabled the hotspot, observed the offline/use-cache status, then restored the hotspot and confirmed successful Refresh. Existing device Wi-Fi configuration was not edited. |
| Server data preservation | Read-only SQLite checks matched the earlier timer checkpoint for configuration bytes/mode, selected collection/revision, auth state and complete completion/creation receipt hashes (four/two rows). No new receipt resulted from preview Cancel. Speech usage changed following recognition; no billing/refund claim is made. Timer remained active and source publication advanced. |

COM3 disappeared during the handoff; the operator confirmed unplugging USB and
then reconnecting it. Read-only discovery found the same Espressif instance. The
failed port-open attempt was not a firmware defect. A subsequent 115.51-second
window included one startup sequence reporting `USB_UART_CHIP_RESET` and matching
ELF prefix `e8b6ae41f`. That window is not counted as reset-free acceptance, and
the observation does not isolate whether cable reconnection or the host serial
transition caused the reset. No reset command was issued; this was not a planned
power-cut-during-write experiment. No panic or error-level line was captured.

That window captured a current `Refresh result=2, cached=7`. Device timestamps
82,664 to 91,919 ms bound that downloader interval to 9,255 ms. Before-download
internal free/largest samples were 55,556/19,456 bytes; post-download samples were
55,060/17,408, with stack watermark 3,264. These are samples and downloader timing,
not minimum heap, DMA peaks, UI latency or microphone/ASR/cancel timing. The first
window included pre-existing UART output with nonmatching intervals; those lines
were not paired for latency. Later captures separated initial buffered output
and retained local reception times. Voice operations completed outside their
bounded log window, so their PASS relies on the operator, not invented logs.

The final 115.51-second online-recovery window again captured
`Refresh result=2, cached=7`, at device timestamps 249,399 to 256,065 ms
(6,666 ms for that downloader interval). End samples were internal free/largest
55,264/17,408 bytes and stack watermark 3,248. It had no boot, panic or error-level
line and released COM3 normally. This is not voice timing or a lifetime peak.

Raw logs remain private in
`/home/ywyz/code/ai-passport-oc-20261022/oc-040506-20261022/`; only numeric/enum
summaries and public firmware identity were used in review. No task text, audio,
credentials or private logs were sent to OpenCode. Every capture was bounded
below 120 seconds; the port was released after observation.

The old intermittent refresh failure was not reproduced in these operator checks;
its root cause remains unproven. New error labels/malformed/late replies were
checked with synthetic host I/O, without revoking production tokens or exhausting
quota. Cancellation while awaiting a potentially billable recognition response,
minimum internal heap/DMA peaks and precise voice latency remain unverified.
The installed firmware lacks the required start/stage/minimum/DMA telemetry;
full resource/latency acceptance is blocked on a separately reviewed measurement
increment. Physical power cuts and mixed long-duration operation remain open.

Build: PASS (complete gate; new validation-only bundle `8e5bfcdf/221c9998`, not flashed).
Host tests: PASS (29 worker groups default/optimized, independent mutation proof,
64 companion cases with zero skips, complete gate).
Device tests: PASS for the exact operator-confirmed subset above, not all suite acceptance.
Unverified: recognition-wait cancel, physical fault-label rendering, voice latency,
minimum heap/DMA peaks, physical write-time power cuts, long-duration/mixed use
and the first intermittent refresh failure's root cause.


## OC-07/08 instrumented candidate handoff, 2026-10-22

The reviewed instrumented image is `a353524e/dc655e73`; full hashes and validation
are in the [reliability checkpoint](passport-reliability-review.md#oc-070508-instrumentation-review-2026-10-22).
It is not installed. Prior operator acceptance remains attached exclusively to
`7c292cfc/e8b6ae41`. Read-only host enumeration found the same COM3 Espressif
`303A/1001/MI00` target; WSL has no native tty for it. All source, artifacts,
plans and private logs remain on native WSL storage. Host Python is used only
for the hardware bridge; bridge/observer self-tests have not opened COM3.

The concrete local `flash-plan.json` awaits firmware-specific consent. It permits
only the verified 3,631,120-byte application at `0x10000`, with erase range
`[0x10000,0x387000)`. First re-read the current 4 KiB table sector at `0x8000`;
require the expected 3,072-byte partition table SHA-256
`34899c032800849c9fc300bc27ff086499ffd90faf3b76d2ab6dc8092750c5bf` and the
expected padded-sector digest. On mismatch, stop before a firmware write.
The compatible installed bootloader and table are excluded from writing, as
are NVS, PHY, taskcfg, taskkeys, task cache and both journals. No merged-image
write, full-chip erase, eFuse operation or rebuild-and-flash is allowed. Native
pinned esptool retains its bounded protocol recovery; there is no outer automatic
retry. Normal ROM entry/restart is expected during the approved flash procedure.
No firmware readback backup is required.

After an approved write and digest verification, establish startup identity and
the real serial method before measuring. Keep one passive port opening, discard
old buffered lines, bind windows to firmware/sequence and stop any reset-sensitive
case on a reboot. DTR/RTS settings and clean simulated logs do not prove a
reset-free physical connection. Prepare the operator before each bounded window
of at most 115 seconds. Use fictional speech, preview/default Cancel, capture
stop/cancel/re-entry, recognition-wait cancel, offline no-HTTP Refresh and online
TLS Refresh. Restore the AP at the AP; do not rewrite device Wi-Fi settings.
Allow at most two potentially billable recognition attempts (preview and
wait-cancel), stop on the first ambiguous paid error and never automatically
resend. No task creation, completion or real-journal discard is part of this run.

Report observed stop/cancel edges, audio-release and cleanup intervals separately;
valid bits distinguish an absent edge from a valid negative post-capture cancel.
Report only observed heap/DMA samples and worker stack watermarks. Missing events
mean insufficient evidence. Measurements do not establish true peak headroom,
UI latency, provider admission or billing/refund. Normal refresh/recording may
update the inactive cache generation through existing firmware behavior; this is
not journal clearing or authorization to change task data. Private UART content
stays local; OpenCode receives only whitelisted numbers/enums and operator facts.

Build: PASS. Host tests: PASS (44 runtime groups default/optimized and complete gate).
Device tests: NOT RUN for `a353524e/dc655e73`; no port opening or firmware write yet.
Unverified: exact-new-image consent, real serial/startup and instrumented cases,
true resource peaks, physical error rendering, write-time power cuts and long-run
acceptance. The original intermittent Refresh cause and translation decisions
remain separately open.


## Instrumented application flash, 2026-10-23

The user explicitly approved COM3 and the exact `a353524e/dc655e73` candidate,
app-only erase/write range `[0x10000,0x387000)`. Fresh native archive/source
checks and read-only PnP discovery matched the same physical ESP32-C3. The
actual 4,096-byte partition sector read at `0x8000` matched SHA-256
`ba72802878f6cf77acb0a37612e2fb7f9cf830133889bec9438cdd7f8c8fbdab`.
Only the verified 3,631,120-byte app was written at `0x10000`. Pinned esptool's
post-write device MD5 comparison passed; the immutable archive SHA-256 had been
independently verified. Both metadata-read and write connections exited with
confirmed serial/client/listener closure. There was no outer retry or whole-chip
erase. Bootloader/table/NVS/PHY/taskcfg/taskkeys/cache/journals were excluded
from the write ranges; normal firmware activity may still update its cache.

This supersedes the earlier not-installed status for this exact candidate.
A single passive observation was then opened without sending device bytes.
Its startup ELF banner has not yet been observed; image identity is currently
bound by the verified write record, not a serial prefix observation. Operator
confirmation of retained tasks and instrumented interactions is pending. No
forced reboot was used merely to obtain a banner. Do not relabel old-image
operator acceptance as evidence for this image.

Private flash/consent/observer evidence remains under
`/home/ywyz/code/ai-passport-oc-20261022/oc-0708-20261022/`.

Build: PASS (unchanged exact reviewed archive).
Host tests: PASS (previous final-source default/optimized and complete gates).
Device tests: PASS only for matching target/table, app-only write and device
digest verification; instrumented interactions are NOT RUN.
Unverified: serial startup/reset-free operation, operator-visible retained data,
stop/cancel/release/cleanup intervals, real resource samples/peaks and the
remaining previously listed acceptance.

The passive session was then stopped normally while operator readiness remained
pending. Serial closure and host-process exit were confirmed, with zero observer
bytes sent, no active measurement window and no new UART bytes received. The ELF
banner remains unobserved; this is insufficient startup evidence, not a firmware
failure. OC-08 reviewed only the sanitized flash/idle summary and accepted the
flash/digest subset, without requesting an unsupported repair. COM3 is released.
The next action requires the operator at the device; the existing exact flash
consent remains valid and need not be requested again.


### Measured voice subset and closeout, 2026-10-23

The operator confirmed retained original tasks and an accessible voice page, then
confirmed cancellation/re-entry and the next recording worked normally. A new
single-open passive session captured two complete sequences in one 110-second
window, without an observed reset/panic trigger. Each sequence had exactly one
final and reached cleanup. The operator clarified the second action was a short
stop followed by long-press cancel; its different capture result is consistent
with that action, not evidence of a button defect.

| Observed sequence | Capture endpoint | Audio release call | Worker cleanup interval |
| --- | --- | --- | --- |
| 1: capture cancel, result 4 / cap 3 | cancel to capture end 117.741 ms | 22.102 ms | 354.930 ms |
| 2: short stop then cancel, result 5 / cap 2 | stop to capture end 19.503 ms | 42.857 ms | 453.232 ms |

For sequence 1, cancel to audio-release return was 141.534 ms and cancel to
cleanup-done was 498.006 ms. In sequence 2, cancellation arrived 3,303.652 ms
after capture had ended; it is not a negative microphone-stop latency. Cancel to
cleanup-done was 1,469.577 ms. HTTP open was recorded, but payload-sent and
fetch-return markers were absent, so recognition-response-wait cancellation was
not measured. This counted conservatively as one potentially billable attempt;
no provider admission/billing/refund conclusion is available.

Recorded sample minima were internal free 42,544 bytes, largest internal block
23,552 bytes, DMA free 34,788 bytes, largest DMA block 23,552 bytes and worker
stack watermark 2,308 bytes. Internal/DMA sets overlap. These are observed
samples and a stack watermark, not true transient peaks. The internal samples
showed no breach of the proposed 40 KiB/16 KiB margins; those proposals do not
establish a product PASS threshold or prove peak headroom.

The operator then stated the function works normally and further defensive
testing is unnecessary. Codex ended the next preview window with zero events
and stopped the observer normally, confirming serial closure and host-process
exit. There was no further recognition attempt, offline/AP manipulation, fault
injection, task creation or completion. The empty preview window is not accepted
as a new-image preview measurement. Serial ELF banner identity remains unobserved;
events are procedurally tied to the same physical device and exact approved
app write/device digest. No lines were joined across openings or windows.

Numeric-only offline review agreed with the actions and found no demonstrated
repair issue. The measured subset is closed per the operator's latest direction;
no further OC-05 change or repeated device round is indicated. Raw UART remains
private in native WSL; OpenCode receives numeric/enum summaries and operator facts.
Final evidence: `diagnostics-1792748765497896863-137251.summary.json`,
`20261023-operator-ledger.json` and `review-measurements/cancel-window-review.txt`
under the private evidence directory above.

Build: PASS (exact installed `a353524e/dc655e73`, production bytes unchanged).
Host tests: PASS (44 groups default/optimized and complete gate already passed).
Device tests: PASS for app-only flash/digest, operator-confirmed retained tasks,
usable voice page/cancel/re-entry and the two measured action variants above.
Unverified: serial boot ELF prefix, new-image preview timing/default-Cancel trace,
remote recognition-wait cancellation, true resource peaks and other previously
separate acceptance. These gaps are recorded without extending the test scope.
