<p align="right">
  <a href="passport-translation-provider.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Translation real-provider and HTTP audio contract

Updated: 2026-10-25. This document describes the deployed companion real MiMo T1
adapter, the bounded audio upload, the single-job worker, the independent quota
ledger and the additive deployment path. It does not flash firmware or authorize
further paid traffic.

The reviewed provider/retention decisions remain in the
[translation plan](passport-translation-plan.md); the provider-independent
bookkeeping and mock checkpoints remain in the
[T1 core checkpoint](passport-translation-t1-core.md).

## Status and safety boundary

- Implemented, host-tested and deployed: `companion/translation/`
  provider/config/server modules, the authenticated canonical WAV `PUT`, the
  single bounded worker, cancellation between stages, restart `UNKNOWN`, the
  independent daily quota ledger and the paired documentation.
- Deployment evidence: active behind `/suite/v1/translations*` since 2026-10-23.
  One authorized synthetic EN->zh-CN smoke passed in 7.445 s with one ASR and one
  MT claim, consuming 3 ASR seconds and one MT job from the separate quota.
- Not run: further real or paid calls, billing or invoice verification, and
  device capture/upload. The OC-11 device attempt stopped at readiness before
  admission and created no job or claim; the device now holds an older
  placeholder-only build.
- The user accepted the disclosed uncertainty for the selected plan. The actual
  account region, residency, audio-training treatment and quota policy were not
  independently established, so no region or zero-retention guarantee is claimed
  beyond that recorded acceptance.

## Provider contract

The endpoint is fixed to
`https://api.xiaomimimo.com/v1/chat/completions`. The adapter uses TLS
certificate verification, a server-side `Authorization: Bearer` key and no
redirect following. The key never enters device configuration, request
parameters, logs or errors.

### Speech recognition

Model `mimo-v2.5-asr`, non-streaming. The WAV is validated before any call and
sent as one data URL. `zh-CN` maps to `zh`; `en` maps to `en`.

```json
{
  "model": "mimo-v2.5-asr",
  "stream": false,
  "messages": [{
    "role": "user",
    "content": [{
      "type": "input_audio",
      "input_audio": {
        "data": "data:audio/wav;base64,<canonical WAV>",
        "format": "wav"
      }
    }]
  }],
  "asr_options": {"language": "zh"}
}
```

### Text translation

Model `mimo-v2.6-flash`, non-streaming, thinking disabled, JSON-object output
and a bounded completion. The fixed system instruction says the user text is
untrusted data, not instructions. The source text appears only inside the user
JSON object.

```json
{
  "model": "mimo-v2.6-flash",
  "stream": false,
  "thinking": {"type": "disabled"},
  "response_format": {"type": "json_object"},
  "max_completion_tokens": 1024,
  "messages": [
    {"role": "system", "content": "<fixed JSON-only translation instruction>"},
    {"role": "user", "content": "{\"text\":\"<source>\",\"source_lang\":\"en\",\"target_lang\":\"zh-CN\"}"}
  ]
}
```

The returned content must be exactly one JSON object with exactly these keys:

```json
{"target_text": "<complete translation>", "source_lang": "en", "target_lang": "zh-CN"}
```

`source_lang` and `target_lang` must equal the requested pair. `target_text`
must be a non-empty string of at most 2,048 UTF-8 bytes and 512 Unicode scalar
values. Duplicate keys, non-UTF-8 bytes, unpaired surrogates, extra or missing
keys, wrong languages and truncated content are rejected without silent
truncation or summary substitution.

### Response and error handling

The adapter requires exactly one choice, `finish_reason` `stop` and a string
`content`. `length`, `content_filter`, multiple choices, malformed outer JSON,
malformed inner JSON and protocol surprises are rejected. Reads use a fixed
65,536-byte buffer. The transport uses non-blocking sockets and one absolute
monotonic deadline spanning bounded DNS resolution, TCP connect, TLS handshake,
request send, response headers and response body. A slow header or body
trickle cannot extend the deadline. A redirect, a non-identity
`Content-Encoding`, an unsupported transfer coding, an oversized or truncated
body, or a response after the deadline is rejected. Chunked responses are
decoded within the same bound. DNS uses a bounded same-host singleflight cache
(300-second positive, 5-second negative); at most one resolver thread exists,
it captures no API key or request body, and each caller still uses its own
absolute deadline. There is no retry; a failed paid stage becomes a terminal
failure.

Only sanitized codes cross the boundary:

| Code | Meaning |
| --- | --- |
| `PROVIDER_AUTH` | HTTP 401 or 403 |
| `PROVIDER_QUOTA` | HTTP 429 |
| `PROVIDER_TIMEOUT` | Wall-clock deadline exceeded |
| `PROVIDER_UNAVAILABLE` | DNS/TLS/socket failure or HTTP 5xx |
| `PROVIDER_INVALID_RESPONSE` | Redirect, compression, HTTP 4xx or invalid body |
| `QUOTA` | Local daily translation budget exhausted |

An ASR failure leaves no source text. An MT failure preserves the valid ASR
source in RAM for the remaining text lifetime.

## HTTP contract

All routes require the independent translation bearer. Task reader, writer and
admin tokens are rejected, and the translation token is not accepted on task
routes. Tokens never appear in URLs or logs.

### Local readiness

`GET /suite/v1/translations/ready` is an exact route checked before the job-ID
pattern. It uses the same token matrix and returns bounded JSON:

```json
{"schema":1,"ready":true,"provider":"mimo"}
```

It is true only when the local translation service is configured and its worker
is running, and false during shutdown. When not ready it returns
`503 Service Unavailable` with `{"error":"NOT_READY"}`. This is local
readiness/authentication only: it does not call MiMo, consume quota, mutate
metadata, or guarantee provider uptime.

### Admission

`POST /suite/v1/translations` with `Content-Type: application/json` and exactly:

```json
{"schema":1,"operation_id":"<32 lowercase hex>","source_lang":"en","target_lang":"zh-CN","format":"wav","audio_sha256":"<64 lowercase hex>","audio_bytes":9644}
```

Only `en` to `zh-CN` and `zh-CN` to `en` are accepted. A matching replay returns
the original admission without another provider claim; a different payload is
`OPERATION_CONFLICT`. Responses are 202 admission/replay, 409
`OPERATION_CONFLICT`, 400, 401, 429 `BUSY`, and 503 `UNAVAILABLE` during
shutdown.

### One bounded audio upload

`PUT /suite/v1/translations/{job_id}/audio`

- `job_id` is exactly 32 lowercase hex characters.
- `Authorization: Bearer <translation token>` is required.
- `Content-Type` must be exactly `audio/wav`; parameters are rejected.
- `Content-Length` is required. A missing length, `Transfer-Encoding`, a
  non-identity `Content-Encoding`, a short body or an over-limit length is
  rejected before any provider claim.
- The body is at most 320,044 bytes: a canonical 44-byte WAV header and 16 kHz,
  signed 16-bit little-endian mono PCM, at least 0.3 seconds and at most
  10 seconds. The full-file SHA-256 and length must match the admission.
- The body is read in bounded streaming blocks; the service never buffers an
  unbounded request.

Responses:

| Status | Meaning |
| --- | --- |
| `202 Accepted` | Validated and accepted by the bounded worker queue |
| `200 OK` | Matching replay or terminal state; no new provider claim |
| `400 Bad Request` | Invalid content type, length, encoding or WAV envelope |
| `401 Unauthorized` | Missing or wrong bearer |
| `404 Not Found` | Unknown job ID |
| `409 Conflict` | Audio conflict, cancelled job or stale generation |
| `413 Payload Too Large` | `audio_bytes` or body above 320,044 |
| `429 Too Many Requests` | Bounded queue or metadata capacity full |
| `503 Service Unavailable` | The local service is shutting down (`UNAVAILABLE`) |

An invalid WAV is rejected before the ASR claim and consumes no quota. A
matching replay never repeats ASR. A `202` acknowledges queue acceptance; it
does not mean recognition or translation finished. If a queued WAV reaches its
300-second lifetime before an ASR claim, the durable row becomes `EXPIRED` with
`AUDIO_EXPIRED`; it remains for deduplication and is never replayed
automatically.

### Status and cancellation

`GET /suite/v1/translations/{job_id}` returns the job view. `DELETE` cancels
idempotently:

```json
{"schema":1,"operation_id":"<hex>","job_id":"<hex>","state":"DONE","source_lang":"en","target_lang":"zh-CN","generation":1,"audio_bytes":9644,"source_bytes":24,"target_bytes":27,"updated_at":1000,"error":null,"source_text":"<recognized source>","target_text":"<translated text>","text_lost":false}
```

States are `ADMITTED`, `RUNNING`, `DONE`, `FAILED`, `CANCELLED`, `EXPIRED`,
`UNKNOWN` and the view-only `TEXT_LOST`. `TEXT_LOST` means the durable row is
`DONE` but the RAM text expired or the process restarted; metadata is not a
recoverable translation. `UNKNOWN` means a stage was claimed before a restart,
so the paid stage is never replayed automatically. `EXPIRED` means the queued
WAV lifetime elapsed before an ASR claim and carries `AUDIO_EXPIRED`. A
controlled shutdown cancels pending and active jobs with `SHUTDOWN`, clears RAM
text/audio and permits no later paid stage; the worker drain is bounded.

One worker processes jobs in admission order. A `PUT` returns after the queue
accepts the job; SQLite reads/writes are short and no request-wide lock is held
across ASR or MT. Status and cancellation therefore remain serviceable while a
provider call runs, including through the threaded WSGI server and the
`gthread` deployment configuration.

## Configuration and deployment

The service reads one private JSON file, for example
`/etc/passport-translation/config.json`:

```json
{
  "schema": 1,
  "translation_token": "<independent 32-128 character token>",
  "database": "/var/lib/passport-translation/translation.sqlite3",
  "provider_key_file": "/etc/passport-translation/provider.json",
  "daily_asr_seconds": 60,
  "daily_mt_jobs": 6
}
```

Optional keys: `forbidden_tokens`, `text_ttl`, `audio_ttl`, `queue_size`,
`cleanup_interval`, `asr_timeout`, `mt_timeout`, `capacity`.

`provider_key_file` is a private JSON file containing `api_key`. It may reference
the existing `/etc/passport-tasks/speech.json` when the translation service
account can read it, or a separate key file. The Tasks file is never modified;
`daily_seconds` there is not reused. The translation ledger has its own daily
budget: 60 ASR seconds and 6 MT jobs by default.

Create the config additively; the generator refuses an existing output, refuses
to write inside `/etc/passport-tasks`, takes no key argument and prints no key:

```bash
python3 tools/configure-translation.py \
  --output /etc/passport-translation/config.json \
  --provider-key-file /etc/passport-translation/provider.json
```

The config and key files must be mode `0600`; the parent directory should be
mode `0700`. The database is created mode `0600`. `read_config` rejects
symlinks, group/world-readable files, unknown keys and relative paths.

Deployment artifacts are additive:

- `companion/deploy/passport-translation.service` runs one process as
  `passport-translation`, with `StateDirectory=passport-translation` and
  `ReadWritePaths=/var/lib/passport-translation`.
- `companion/deploy/gunicorn-translation.conf.py` binds loopback
  `127.0.0.1:8788`, uses one worker and four request threads.
- `companion/deploy/caddy-translation-route.example` routes
  `/suite/v1/translations*` to `8788` before the existing Tasks catch-all.

Local inspection uses the threaded standard-library server:

```bash
python3 -m companion.translation --config /etc/passport-translation/config.json --port 8788
```

The config generator does not write Tasks configuration, NVS, key partitions or
the existing speech file. Adding translation does not disable Tasks, weather or
provisioning.

## Bounds and retention

| Resource | Bound |
| --- | --- |
| Canonical WAV | 320,044 bytes; 16 kHz PCM16 mono; 0.3-10 seconds |
| Authenticated body read | 8,192-byte streaming blocks |
| Worker queue | 8 jobs by default |
| Metadata rows | 64 by default; new admission returns `BUSY`, no eviction |
| RAM source/target text | 120 seconds; scheduler sweep independent of requests |
| RAM queued audio | 300 seconds; `EXPIRED`/`AUDIO_EXPIRED` if unclaimed, removed after the stage or cancellation |
| Daily ASR budget | 60 seconds by default |
| Daily MT budget | 6 jobs by default |
| Provider response buffer | 65,536 bytes |

Source text, translation text and audio never enter SQLite, the task journals,
request logs or backups. The metadata ledger stores identifiers, hashes, byte
counts, states, generation and timing only. Expiry removes RAM text/audio but
retains the durable deduplication row; it never triggers an automatic paid
replay. Quota rows are one row per UTC day and older rows are pruned.

## Verification

```bash
PYTHONDONTWRITEBYTECODE=1 python3 tests/test_companion_translation.py
PYTHONDONTWRITEBYTECODE=1 python3 tests/test_companion_translation_real.py
./tools/test-companion.sh
```

The real-adapter suite executes the service, worker, provider, WSGI route,
threaded server and config generator with only the external HTTP transport and
clock injected. It covers both request shapes, invalid-audio rejection before
claim, 401/403/429/5xx/redirect, timeout/slow reads/oversized/compressed
responses, malformed outer and inner JSON, duplicate keys, unpaired Unicode,
`length`/`content_filter`, source-on-MT-failure, cancellation before claim and
between ASR/MT, restart `UNKNOWN`, matching replay with zero calls, independent
auth and quotas, scheduler expiry without API requests, and absence of the key
or source sentinel in errors.

```text
Build: NOT RUN (server-only service; no firmware change)
Host tests: PASS (18 preserved + 43 provider/service/config tests)
Device tests: FAIL for real translation readiness; no device job or provider claim
Deployment: PASS (active since 2026-10-23)
Real provider: PASS for one authorized synthetic EN->zh-CN smoke; paid billing and
               invoice verification NOT RUN
Unverified: account terms/region/retention, additional paid calls, device capture/
            upload and firmware integration, serial and flashing
```
