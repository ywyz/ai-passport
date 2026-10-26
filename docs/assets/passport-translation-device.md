[简体中文](passport-translation-device.zh_CN.md) · **English**

# Translation storage, configuration and USB provisioning

This T1 module provides shared ownership of the inactive task-cache slot, a
separate encrypted translation configuration, and a translation-only native USB
Serial/JTAG provisioning window. It is registered in the build and used by the real
T1 worker; microphone capture and the provider call live in the worker, and TTS
remains unimplemented. The existing task cache, journals, encrypted credentials,
five-minute Tasks refresh and partition table are unchanged.

Current status, 2026-10-25:

```text
Build: PASS (registered; 2026-10-24 native complete gate)
Host tests: PASS for the focused cases listed below
Device tests: FAIL for real OC-11 translation (readiness before capture). The USB
             configuration frame was sent and the firmware subsequently loaded a
             valid configuration. The original computer-side matching ACK was
             inconclusive; later host parser fixes have synthetic tests only, so
             full USB provisioning ACK acceptance is NOT established.
Real provider calls: PASS for one authorized synthetic EN->zh-CN smoke on the
                     companion side; no device-side provider call occurred
Unverified: hardware log timing, erase lifetime/wear, real-NVS power loss,
            memory/stack peaks, actual provider retention
```

## Shared inactive-slot ownership

`main/suite_tasks.c` still owns the authoritative `task_cache_t`. The new
`suite_audio_job` module adds the outer lock S over that object and its active
generation. It never opens a second cache and does not add a permanent worker.

| Phase | Ownership |
| --- | --- |
| Tasks refresh staging/commit | S, then N for a bounded request |
| Task voice preflight | N only |
| Task voice capture | S + A; A is released before the upload |
| Task voice upload | S + N |
| Translation capture | S + A |
| Translation upload/download | S + N |
| Scratch cleanup | S only |
| Weather/completion without scratch | N only; must not wait for S/A |

A and N are never held together, and S/A is never acquired while N is held.
Startup recovery and final cleanup erase data sectors before the marker sector.

A lease records the active generation and the qualified inactive slot. Full
Tasks refresh holds S across staging and commit, so the generation cannot change
under a capture or upload owner. A busy or quarantined slot fails closed instead
of reusing uncertain audio. Failed cleanup quarantines only the marked inactive
slot for the rest of the boot; startup recovery retries it before voice or
translation may use scratch.

The existing scratch marker word is retained byte-for-byte, so an interrupted
recording written by an earlier firmware is recognized and cleaned. Owner,
generation, requested span and quarantine are RAM-only and are not persisted in
the marker. Cleanup always erases `TASK_VOICE_ERASE` (`0x5f000`) from the
inactive slot base when the legacy marker is present, even when a translation
stage requested only `0x50000`; that is the actual cleanup span, not a claim
that the whole lifecycle is `0x50000`. Data sectors are erased before the
marker sector. Recovery with a valid cache considers only the inactive slot;
with no valid generation it considers both slots. Active task headers, records,
journals and settings are never erased by recovery. An unreadable marker is not
treated as clean: cleanup fails, the slot stays quarantined and recovery
preserves the marker.

### Public interface used by the capture worker

```c
#include "suite_audio_job.h"

suite_audio_lease_t *lease = NULL;
esp_err_t err = suite_audio_job_acquire(SUITE_AUDIO_OWNER_TRANSLATION, 1000, &lease);
if (err != ESP_OK) { /* BOUNDED: report busy/unavailable */ }

/* Configure the BSP for 16 kHz, 16-bit, mono first. Enforce at least 9,600
   PCM bytes (0.3 s) and at most 320,000 PCM bytes (10 s). */
if (suite_audio_job_stage_begin(lease, SUITE_AUDIO_TRANSLATION_SPAN) != ESP_OK) {
    /* storage */
}
while (capturing) {
    /* PCM16LE mono 16 kHz, at most SUITE_AUDIO_CHUNK_MAX (512) bytes */
    if (suite_audio_job_append(lease, pcm, bytes) != ESP_OK) { /* storage */ }
}
if (suite_audio_job_finalize(lease) != ESP_OK) { /* storage */ }

uint8_t digest[32];
if (suite_audio_job_sha256(lease, digest) != ESP_OK) { /* storage */ }

/* S is already held: acquire N inside S, upload bounded reads, then release N
   before returning to capture/playback. Never acquire S while holding N. */
if (suite_net_take(1000)) {
    uint32_t offset = 0;
    while (offset < suite_audio_job_wav_bytes(lease)) {
        uint8_t chunk[SUITE_AUDIO_CHUNK_MAX];
        size_t n = suite_audio_job_wav_bytes(lease) - offset;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        suite_audio_job_read(lease, offset, chunk, n); /* bounded read */
        /* write chunk to the request body */
        offset += (uint32_t)n;
    }
    suite_net_give();
}

/* Always releases S. Non-OK means cleanup failed and the slot is quarantined. */
esp_err_t cleanup = suite_audio_job_release(lease);
```

A finalized canonical WAV is at most `SUITE_AUDIO_WAV_MAX` (320,044) bytes,
including the 44-byte header. Audio is never held as a whole clip in RAM.
`stage_begin` accepts a sector-aligned span up to `SUITE_AUDIO_SPAN_MAX`
(`TASK_VOICE_ERASE`). The translation caller stages
`SUITE_AUDIO_TRANSLATION_SPAN` (`0x50000`), which covers the maximum translation
WAV and its 4 KiB marker. The caller must configure 16 kHz/16-bit/mono, enforce
`SUITE_AUDIO_TRANSLATION_PCM_MIN` (9,600 bytes) and
`SUITE_AUDIO_TRANSLATION_PCM_MAX` (320,000 bytes), and append at most
`SUITE_AUDIO_CHUNK_MAX` (512) bytes at a time.
Cancellation is advisory: the worker calls `suite_audio_job_cancel()` and still
calls `suite_audio_job_release()` (or `suite_audio_job_abort()`) so that S and
scratch cleanup are released by the owning worker.

## Encrypted translation configuration

`suite_translation_config` stores one versioned blob in the existing encrypted
`taskcfg` partition, namespace `translation`, key `config`:

| Field | Meaning |
| --- | --- |
| `magic`, `version` | Blob layout identity |
| `schema` | Translation protocol schema; currently `1` |
| `origin` | HTTPS origin only, no path, query, fragment, user info or port |
| `token` | Separate capability token, 32-128 URL-safe characters |
| `crc` | CRC over all preceding fields |

Reading first locates `taskkeys`, calls `nvs_flash_read_security_cfg()`, then
`nvs_flash_secure_init_partition("taskcfg", &key)`. Applying also initializes
that encrypted partition before it inspects the Tasks token namespace, so a
fresh boot where Tasks has not run yet can still perform its first write. It
never erases a partition, deinitializes NVS or generates keys. A missing
namespace is `UNCONFIGURED`; an invalid or unreadable blob fails closed.

Applying a configuration validates the origin and token, proves the token
differs from the existing Tasks `token` and `write_token`, writes the blob, and
commits it. It then reopens the namespace and compares the readback. Results are
explicit:

| Result | Meaning |
| --- | --- |
| `APPLY_OK` | New blob committed and verified |
| `APPLY_UNCHANGED` | Same configuration already stored; no write |
| `APPLY_INVALID` | Validation or token-distinctness failure; no write |
| `APPLY_STORAGE_NO_WRITE` | Storage failed and the previous blob is still authoritative |
| `APPLY_VERIFY_UNKNOWN` | Commit/readback is ambiguous; an idempotent retry is safe |

Capacity or commit failure never replaces other namespaces: Tasks origin, read
and write tokens, all eight completion rows, the voice intent and every other
key remain byte-identical. Reapplying the identical configuration does not
write NVS again. No token or origin is logged.

The worker should call `suite_translation_config_get()` into a stack structure,
use it, and zeroize it with `mbedtls_platform_zeroize()`.

## Translation-only USB provisioning

The device has no other runtime serial command handler. Provisioning uses the
native USB Serial/JTAG port on GPIO18/19 and never UART0, whose default TX
conflicts with the GPIO21 backlight.

An explicit user selection opens a window of at most 60 seconds. The window
refuses to open while a shared scratch lease is held (capture/upload) or while
the optional translation busy predicate reports active work. It installs the
ESP-IDF `esp_driver_usb_serial_jtag` driver, routes the existing console VFS
through that driver so normal logs continue, runs exactly one reader, and
uninstalls the driver when the window closes. Outside the window there is no
reader and no installed driver. Task exit is the only cleanup path; it cannot
leave a stale writer.

One bounded request ID is accepted per window. Frames are strict UTF-8 JSON
lines of at most 1,024 bytes:

```json
{"schema":1,"operation":"set_translation_config","request_id":"<32 lowercase hex>","origin":"https://...","token":"<capability token>"}
```

The parser rejects duplicate keys, extra or missing fields, wrong types, a
schema other than `1`, malformed request IDs, invalid origins/tokens, raw or
escaped NULs, control characters, trailing bytes and invalid UTF-8. Oversized
lines are discarded without a write. Generic serial noise cannot satisfy the
frame shape. Input is never echoed. At most one configuration write occurs.
A frame that cannot be parsed but whose unambiguous compact request ID is still
known is answered with `INVALID` rather than a lost/empty-ID error. A rejected
frame with a known ID consumes the window's one attempt; unrelated console log
lines carry no request ID and do not consume it. Duplicate matching frames get
the same answer, and conflicting or second requests get a bounded failure
without a write.

Acknowledgements contain only the matched request ID, a status and the
verification flag:

```json
{"request_id":"<same ID>","status":"OK","verified":true}
```

Statuses are `OK`, `INVALID`, `BUSY`, `STORAGE` and `UNVERIFIED`. `STORAGE` means
the previous configuration is still authoritative; `UNVERIFIED` means the
commit/readback is ambiguous and should be retried idempotently. Neither the
origin nor token is repeated.

### Host tool

`tools/configure-translation-device.py` reads the origin and token from a
separate private JSON file or hidden prompts, never from command-line values.
The file has mode `0600` on POSIX and contains only:

```json
{"origin": "https://translate.example.test", "token": "<capability token>"}
```

```bash
python3 tools/configure-translation-device.py \
  --config /private/translation.json --port COM7 --timeout 15
python3 tools/configure-translation-device.py \
  --config /private/translation.json --port rfc2217://host:4000
```

The tool opens the port with DTR/RTS inactive and waits for one strict matching
acknowledgement within an absolute bounded time. It filters normal startup,
Tasks and weather log lines without printing them, ignores stale request IDs,
rejects a malformed acknowledgement that carries its own request ID, and never
extends the deadline for unrelated log traffic. It prints neither the hostname,
token nor raw serial data. It does not flash, erase, run esptool, or create task
configuration images/keys.

## Tested scope

Focused host tests compile the actual ownership, configuration and protocol
sources:

- `tests/test_suite_audio_job.c`: real cache binding, bounded acquisition,
  canonical WAV, bounded append/read/hash, data-before-marker erase, marker read
  fault, erase failure/quarantine and old-marker compatibility.
- `tests/test_suite_tasks_runtime.c`: existing Tasks refresh/voice regressions
  plus translation lease blocking refresh before N/erase, cancellation release
  and unchanged active generation.
- `tests/test_suite_translation_config.c`: exact blob readback, first-use
  encrypted initialization before the Tasks lookup, idempotence, token
  distinctness, injected key/set/commit/readback failures, and byte-level
  preservation of Tasks rows, voice intent and other keys.
- `tests/test_suite_translation_usb_proto.c`: fragmented/oversize/invalid/
  duplicate frames, escaped-NUL/control rejection, known-ID error replies,
  strict ack shape, cancel/expiry during a read, window bounds and no input echo.
- `tests/test_translation_device_config.py`: private-file handling, RFC2217
  opening, inactive DTR/RTS, console-log filtering without deadline extension,
  stale IDs and strict malformed-ack rejection.

These tests use an in-memory NVS model and scripted transport. They do not prove
real-NVS power-loss behavior, Flash erase lifetime, USB timing, console/log
interaction, memory peaks, hardware capture, provider retention or a successful
device flash.
