#include "suite_translation_core.h"
#include <string.h>

static bool lower_hex(const char *value, size_t length)
{
    if (!value || length == 0) return false;
    for (size_t i = 0; i < length; ++i) {
        char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    /* Reject a valid prefix followed by any extra character. */
    return value[length] == '\0';
}

static bool token_ok(const char *value, size_t length)
{
    if (!value || length < TR_TOKEN_MIN || length > TR_TOKEN_MAX) return false;
    for (size_t i = 0; i < length; ++i) {
        char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || c == '_' || c == '-'))
            return false;
    }
    return true;
}

static bool capability_ok(const tr_capability_t *capability)
{
    return capability && capability->scope == TR_SCOPE_TRANSLATION &&
           capability->token_length >= TR_TOKEN_MIN &&
           capability->token_length <= TR_TOKEN_MAX &&
           token_ok(capability->token, capability->token_length);
}

tr_error_t tr_capability_init(tr_capability_t *capability, tr_scope_t scope,
                              const char *token, size_t token_length)
{
    if (!capability) return TR_E_ARGUMENT;
    memset(capability, 0, sizeof(*capability));
    if (scope != TR_SCOPE_TRANSLATION) return TR_E_SCOPE;
    if (!token_ok(token, token_length)) return TR_E_TOKEN;
    capability->scope = scope;
    capability->token_length = (uint8_t)token_length;
    memcpy(capability->token, token, token_length);
    capability->token[token_length] = 0;
    return TR_OK;
}

tr_error_t tr_text_check(const char *text, size_t length, uint32_t *scalars)
{
    if (!text || length == 0) return TR_E_TEXT;
    if (length > TR_TEXT_MAX_BYTES) return TR_E_TEXT_BYTES;

    uint32_t count = 0;
    size_t i = 0;
    while (i < length) {
        unsigned char c = (unsigned char)text[i];
        uint32_t cp;
        size_t extra;
        if (c < 0x80) {
            if (c == 0) return TR_E_TEXT;
            cp = c;
            extra = 0;
        } else if (c >= 0xc2 && c <= 0xdf) {
            cp = c & 0x1fU;
            extra = 1;
        } else if (c >= 0xe0 && c <= 0xef) {
            cp = c & 0x0fU;
            extra = 2;
        } else if (c >= 0xf0 && c <= 0xf4) {
            cp = c & 0x07U;
            extra = 3;
        } else {
            return TR_E_TEXT;
        }
        if (i + extra >= length) return TR_E_TEXT;
        for (size_t n = 1; n <= extra; ++n) {
            unsigned char d = (unsigned char)text[i + n];
            if (d < 0x80 || d > 0xbf) return TR_E_TEXT;
            cp = (cp << 6) | (d & 0x3fU);
        }
        if (extra == 1 && cp < 0x80) return TR_E_TEXT;
        if (extra == 2 && cp < 0x800) return TR_E_TEXT;
        if (extra == 3 && cp < 0x10000) return TR_E_TEXT;
        if (cp > 0x10ffffU || (cp >= 0xd800U && cp <= 0xdfffU))
            return TR_E_TEXT;
        ++count;
        if (count > TR_TEXT_MAX_SCALARS) return TR_E_TEXT_SCALARS;
        i += extra + 1;
    }
    if (scalars) *scalars = count;
    return TR_OK;
}

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

tr_error_t tr_wav_check(const uint8_t *data, size_t length, tr_wav_info_t *info)
{
    if (!data || length < 44) return TR_E_AUDIO;
    if (length > TR_WAV_MAX_BYTES) return TR_E_AUDIO_BYTES;
    if (memcmp(data, "RIFF", 4) || memcmp(data + 8, "WAVE", 4) ||
        memcmp(data + 12, "fmt ", 4) || memcmp(data + 36, "data", 4))
        return TR_E_AUDIO_FORMAT;
    if (read_u32(data + 4) != length - 8 || read_u32(data + 16) != 16 ||
        read_u16(data + 20) != 1 || read_u16(data + 22) != 1 ||
        read_u32(data + 24) != 16000 || read_u32(data + 28) != 32000 ||
        read_u16(data + 32) != 2 || read_u16(data + 34) != 16)
        return TR_E_AUDIO_FORMAT;

    size_t payload = length - 44;
    if (read_u32(data + 40) != payload) return TR_E_AUDIO_FORMAT;
    if (payload < TR_WAV_MIN_DATA_BYTES) return TR_E_AUDIO_BYTES;
    if (payload & 1U) return TR_E_AUDIO_FORMAT;
    if (info) {
        info->bytes = (uint32_t)length;
        info->data_bytes = (uint32_t)payload;
        info->seconds = (uint32_t)((payload + 31999U) / 32000U);
    }
    return TR_OK;
}

static uint32_t rotr32(uint32_t value, unsigned bits)
{
    return (value >> bits) | (value << (32U - bits));
}

static const uint32_t sha256_k[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
    0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
    0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
    0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
    0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
    0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
    0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
    0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static void sha256_block(uint32_t hash[8], const uint8_t block[64])
{
    uint32_t words[64];
    for (unsigned i = 0; i < 16; ++i) {
        words[i] = ((uint32_t)block[i * 4] << 24) |
                   ((uint32_t)block[i * 4 + 1] << 16) |
                   ((uint32_t)block[i * 4 + 2] << 8) |
                   (uint32_t)block[i * 4 + 3];
    }
    for (unsigned i = 16; i < 64; ++i) {
        uint32_t s0 = rotr32(words[i - 15], 7) ^
                      rotr32(words[i - 15], 18) ^ (words[i - 15] >> 3);
        uint32_t s1 = rotr32(words[i - 2], 17) ^
                      rotr32(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }
    uint32_t a = hash[0], b = hash[1], c = hash[2], d = hash[3];
    uint32_t e = hash[4], f = hash[5], g = hash[6], h = hash[7];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + ch + sha256_k[i] + words[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
    hash[5] += f;
    hash[6] += g;
    hash[7] += h;
}

bool tr_sha256_hex(const void *data, size_t length, char out[TR_SHA256_SIZE])
{
    if (!out || (!data && length)) return false;
    uint32_t hash[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };
    const uint8_t *bytes = data;
    size_t remaining = length;
    while (remaining >= 64) {
        sha256_block(hash, bytes);
        bytes += 64;
        remaining -= 64;
    }
    uint8_t tail[128];
    memset(tail, 0, sizeof(tail));
    if (remaining) memcpy(tail, bytes, remaining);
    tail[remaining] = 0x80;
    size_t used = remaining < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)length * 8U;
    for (unsigned i = 0; i < 8; ++i)
        tail[used - 1 - i] = (uint8_t)(bits >> (i * 8U));
    sha256_block(hash, tail);
    if (used == 128) sha256_block(hash, tail + 64);

    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 8; ++i) {
        for (unsigned j = 0; j < 8; ++j)
            out[i * 8 + j] = hex[(hash[i] >> (28U - j * 4U)) & 0xfU];
    }
    out[64] = 0;
    return true;
}

static bool terminal_state(tr_state_t state)
{
    return state == TR_STATE_DONE || state == TR_STATE_FAILED ||
           state == TR_STATE_CANCELLED || state == TR_STATE_UNKNOWN;
}

static bool active_state(tr_state_t state)
{
    return state == TR_STATE_ADMITTED || state == TR_STATE_UPLOADED ||
           state == TR_STATE_RUNNING;
}

static void record_event(tr_core_t *core, tr_event_t event)
{
    uint32_t slot;
    if (core->event_count < TR_EVENT_LOG) {
        slot = (uint32_t)(core->event_head + core->event_count) % TR_EVENT_LOG;
        ++core->event_count;
    } else {
        slot = core->event_head;
        core->event_head = (uint8_t)((core->event_head + 1U) % TR_EVENT_LOG);
    }
    core->events[slot] = (uint8_t)event;
}

static int find_operation(const tr_core_t *core, const char *operation_id)
{
    if (!operation_id) return -1;
    for (uint32_t i = 0; i < core->count; ++i) {
        if (!strcmp(core->jobs[i].operation_id, operation_id)) return (int)i;
    }
    return -1;
}

static void make_job_id(char out[TR_ID_SIZE], const char *operation_id,
                        uint32_t generation, uint32_t slot)
{
    uint32_t hash = 2166136261U;
    for (const char *p = operation_id; *p; ++p) {
        hash ^= (uint8_t)*p;
        hash *= 16777619U;
    }
    uint32_t words[4] = {
        hash, generation, slot ^ 0x9e3779b9U, hash ^ generation ^ 0x85ebca6bU
    };
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 4; ++i) {
        for (unsigned j = 0; j < 8; ++j)
            out[i * 8 + j] = hex[(words[i] >> (28U - j * 4U)) & 0xfU];
    }
    out[32] = 0;
}

void tr_core_init(tr_core_t *core)
{
    if (core) memset(core, 0, sizeof(*core));
}

tr_error_t tr_core_admit(tr_core_t *core, const tr_capability_t *capability,
                         const tr_admission_t *request, int64_t now,
                         tr_job_t *result)
{
    if (!core || !request || !capability_ok(capability)) return TR_E_ARGUMENT;
    if (!lower_hex(request->operation_id, 32)) return TR_E_ID;
    if (!lower_hex(request->audio_sha256, 64)) return TR_E_SHA256;
    if (request->direction != TR_DIRECTION_EN_ZH &&
        request->direction != TR_DIRECTION_ZH_EN)
        return TR_E_DIRECTION;
    if (request->audio_bytes < 44U + TR_WAV_MIN_DATA_BYTES ||
        request->audio_bytes > TR_WAV_MAX_BYTES)
        return TR_E_AUDIO_BYTES;

    int index = find_operation(core, request->operation_id);
    if (index >= 0) {
        tr_job_t *job = &core->jobs[index];
        if (job->direction == (uint8_t)request->direction &&
            job->audio_bytes == request->audio_bytes &&
            !strcmp(job->audio_sha256, request->audio_sha256)) {
            record_event(core, TR_EVENT_REPLAYED);
            if (result) *result = *job;
            return TR_OK;
        }
        record_event(core, TR_EVENT_REJECTED);
        return TR_E_CONFLICT;
    }
    if (core->count >= TR_METADATA_SLOTS) {
        record_event(core, TR_EVENT_REJECTED);
        return TR_E_BUSY;
    }

    tr_job_t *job = &core->jobs[core->count];
    memset(job, 0, sizeof(*job));
    memcpy(job->operation_id, request->operation_id, 32);
    job->operation_id[32] = 0;
    memcpy(job->audio_sha256, request->audio_sha256, 64);
    job->audio_sha256[64] = 0;
    job->direction = (uint8_t)request->direction;
    job->audio_bytes = request->audio_bytes;
    job->generation = core->generation + 1U;
    core->generation = job->generation;
    job->state = TR_STATE_ADMITTED;
    job->created_at = now;
    job->updated_at = now;
    make_job_id(job->job_id, job->operation_id, job->generation, core->count);
    ++core->count;
    record_event(core, TR_EVENT_ADMITTED);
    if (result) *result = *job;
    return TR_OK;
}

static tr_error_t job_for_call(tr_core_t *core, const char *operation_id,
                               const char *job_id, uint32_t generation,
                               tr_job_t **job)
{
    if (!core || !operation_id || !job_id) return TR_E_ARGUMENT;
    if (!lower_hex(operation_id, 32) || !lower_hex(job_id, 32))
        return TR_E_ID;
    int index = find_operation(core, operation_id);
    if (index < 0) return TR_E_NOT_FOUND;
    if (strcmp(core->jobs[index].job_id, job_id)) return TR_E_JOB;
    if (core->jobs[index].generation != generation) return TR_E_GENERATION;
    *job = &core->jobs[index];
    return TR_OK;
}

tr_error_t tr_core_upload(tr_core_t *core, const tr_capability_t *capability,
                          const char *operation_id, const char *job_id,
                          uint32_t generation, const uint8_t *wav,
                          size_t length, int64_t now)
{
    if (!capability_ok(capability)) return TR_E_ARGUMENT;
    tr_job_t *job = NULL;
    tr_error_t error = job_for_call(core, operation_id, job_id, generation, &job);
    if (error != TR_OK) return error;
    if (job->state == TR_STATE_CANCELLED) return TR_E_CANCELLED;

    tr_wav_info_t info;
    error = tr_wav_check(wav, length, &info);
    if (error != TR_OK) return error;
    if (info.bytes != job->audio_bytes) return TR_E_AUDIO_BYTES;
    char digest[TR_SHA256_SIZE];
    if (!tr_sha256_hex(wav, length, digest)) return TR_E_ARGUMENT;
    if (strcmp(digest, job->audio_sha256)) return TR_E_AUDIO_CONFLICT;

    if (job->state == TR_STATE_ADMITTED) {
        job->state = TR_STATE_UPLOADED;
        job->updated_at = now;
        record_event(core, TR_EVENT_UPLOADED);
        return TR_OK;
    }
    if (job->state == TR_STATE_UPLOADED || job->state == TR_STATE_RUNNING ||
        terminal_state((tr_state_t)job->state))
        return TR_OK;
    return TR_E_STATE;
}

tr_error_t tr_core_running(tr_core_t *core, const tr_capability_t *capability,
                           const char *operation_id, const char *job_id,
                           uint32_t generation, int64_t now)
{
    if (!capability_ok(capability)) return TR_E_ARGUMENT;
    tr_job_t *job = NULL;
    tr_error_t error = job_for_call(core, operation_id, job_id, generation, &job);
    if (error != TR_OK) return error;
    if (job->state == TR_STATE_RUNNING) return TR_OK;
    if (job->state != TR_STATE_UPLOADED) return TR_E_STATE;
    job->state = TR_STATE_RUNNING;
    job->updated_at = now;
    record_event(core, TR_EVENT_RUNNING);
    return TR_OK;
}

static tr_event_t state_event(tr_state_t state)
{
    if (state == TR_STATE_DONE) return TR_EVENT_DONE;
    if (state == TR_STATE_FAILED) return TR_EVENT_FAILED;
    if (state == TR_STATE_UNKNOWN) return TR_EVENT_UNKNOWN;
    return TR_EVENT_REJECTED;
}

tr_error_t tr_core_result(tr_core_t *core, const tr_capability_t *capability,
                          const char *operation_id, const char *job_id,
                          uint32_t generation, const char *source_text,
                          size_t source_length, const char *target_text,
                          size_t target_length, tr_state_t terminal,
                          int64_t now)
{
    if (!capability_ok(capability)) return TR_E_ARGUMENT;
    if (terminal != TR_STATE_DONE && terminal != TR_STATE_FAILED &&
        terminal != TR_STATE_UNKNOWN)
        return TR_E_ARGUMENT;
    tr_job_t *job = NULL;
    tr_error_t error = job_for_call(core, operation_id, job_id, generation, &job);
    if (error != TR_OK) return error;
    if (job->state == TR_STATE_CANCELLED) return TR_E_CANCELLED;
    if (terminal_state((tr_state_t)job->state)) return TR_E_STATE;
    if (job->state == TR_STATE_ADMITTED) return TR_E_STATE;

    uint32_t source_bytes = 0;
    bool has_source = false;
    if (source_text != NULL) {
        error = tr_text_check(source_text, source_length, NULL);
        if (error != TR_OK) return error;
        source_bytes = (uint32_t)source_length;
        has_source = true;
    } else if (source_length != 0) {
        return TR_E_ARGUMENT;
    }
    if (terminal == TR_STATE_DONE && !has_source) return TR_E_ARGUMENT;

    uint32_t target_bytes = 0;
    bool has_target = false;
    if (terminal == TR_STATE_DONE) {
        if (!target_text) return TR_E_ARGUMENT;
        error = tr_text_check(target_text, target_length, NULL);
        if (error != TR_OK) return error;
        target_bytes = (uint32_t)target_length;
        has_target = true;
    } else if (target_text != NULL || target_length != 0) {
        return TR_E_ARGUMENT;
    }

    job->state = (uint8_t)terminal;
    job->source_bytes = source_bytes;
    job->target_bytes = target_bytes;
    job->has_source = has_source ? 1U : 0U;
    job->has_target = has_target ? 1U : 0U;
    job->updated_at = now;
    record_event(core, state_event(terminal));
    return TR_OK;
}

tr_error_t tr_core_cancel(tr_core_t *core, const tr_capability_t *capability,
                          const char *operation_id, const char *job_id,
                          uint32_t generation, int64_t now)
{
    if (!capability_ok(capability)) return TR_E_ARGUMENT;
    tr_job_t *job = NULL;
    tr_error_t error = job_for_call(core, operation_id, job_id, generation, &job);
    if (error != TR_OK) return error;
    if (job->state == TR_STATE_CANCELLED) return TR_OK;
    if (!active_state((tr_state_t)job->state)) return TR_E_STATE;
    job->state = TR_STATE_CANCELLED;
    job->source_bytes = 0;
    job->target_bytes = 0;
    job->has_source = 0;
    job->has_target = 0;
    job->updated_at = now;
    record_event(core, TR_EVENT_CANCELLED);
    return TR_OK;
}

uint32_t tr_core_invalidate(tr_core_t *core, int64_t now)
{
    if (!core) return 0;
    uint32_t cancelled = 0;
    for (uint32_t i = 0; i < core->count; ++i) {
        tr_job_t *job = &core->jobs[i];
        if (!active_state((tr_state_t)job->state)) continue;
        job->state = TR_STATE_CANCELLED;
        job->source_bytes = 0;
        job->target_bytes = 0;
        job->has_source = 0;
        job->has_target = 0;
        job->updated_at = now;
        record_event(core, TR_EVENT_CANCELLED);
        ++cancelled;
    }
    ++core->generation;
    return cancelled;
}

tr_error_t tr_core_get(const tr_core_t *core, const char *operation_id,
                       tr_job_t *result)
{
    if (!core || !operation_id || !result) return TR_E_ARGUMENT;
    if (!lower_hex(operation_id, 32)) return TR_E_ID;
    int index = find_operation(core, operation_id);
    if (index < 0) return TR_E_NOT_FOUND;
    *result = core->jobs[index];
    return TR_OK;
}

uint32_t tr_core_count(const tr_core_t *core)
{
    return core ? core->count : 0;
}

uint32_t tr_core_event_count(const tr_core_t *core)
{
    return core ? core->event_count : 0;
}

tr_event_t tr_core_event_at(const tr_core_t *core, uint32_t index)
{
    if (!core || index >= core->event_count) return TR_EVENT_NONE;
    uint32_t slot = (uint32_t)(core->event_head + index) % TR_EVENT_LOG;
    return (tr_event_t)core->events[slot];
}

const char *tr_error_name(tr_error_t error)
{
    switch (error) {
    case TR_OK: return "OK";
    case TR_E_ARGUMENT: return "ARGUMENT";
    case TR_E_SCOPE: return "SCOPE";
    case TR_E_TOKEN: return "TOKEN";
    case TR_E_DIRECTION: return "DIRECTION";
    case TR_E_ID: return "ID";
    case TR_E_SHA256: return "SHA256";
    case TR_E_GENERATION: return "GENERATION";
    case TR_E_TEXT: return "TEXT";
    case TR_E_TEXT_BYTES: return "TEXT_BYTES";
    case TR_E_TEXT_SCALARS: return "TEXT_SCALARS";
    case TR_E_AUDIO: return "AUDIO";
    case TR_E_AUDIO_BYTES: return "AUDIO_BYTES";
    case TR_E_AUDIO_FORMAT: return "AUDIO_FORMAT";
    case TR_E_AUDIO_CONFLICT: return "AUDIO_CONFLICT";
    case TR_E_STATE: return "STATE";
    case TR_E_CONFLICT: return "CONFLICT";
    case TR_E_JOB: return "JOB";
    case TR_E_NOT_FOUND: return "NOT_FOUND";
    case TR_E_BUSY: return "BUSY";
    case TR_E_CANCELLED: return "CANCELLED";
    case TR_E_LATE: return "LATE";
    }
    return "UNKNOWN";
}

const char *tr_state_name(tr_state_t state)
{
    switch (state) {
    case TR_STATE_EMPTY: return "EMPTY";
    case TR_STATE_ADMITTED: return "ADMITTED";
    case TR_STATE_UPLOADED: return "UPLOADED";
    case TR_STATE_RUNNING: return "RUNNING";
    case TR_STATE_DONE: return "DONE";
    case TR_STATE_FAILED: return "FAILED";
    case TR_STATE_CANCELLED: return "CANCELLED";
    case TR_STATE_UNKNOWN: return "UNKNOWN";
    }
    return "UNKNOWN";
}

const char *tr_event_name(tr_event_t event)
{
    switch (event) {
    case TR_EVENT_NONE: return "NONE";
    case TR_EVENT_ADMITTED: return "ADMITTED";
    case TR_EVENT_REPLAYED: return "REPLAYED";
    case TR_EVENT_UPLOADED: return "UPLOADED";
    case TR_EVENT_RUNNING: return "RUNNING";
    case TR_EVENT_DONE: return "DONE";
    case TR_EVENT_FAILED: return "FAILED";
    case TR_EVENT_UNKNOWN: return "UNKNOWN";
    case TR_EVENT_CANCELLED: return "CANCELLED";
    case TR_EVENT_REJECTED: return "REJECTED";
    }
    return "UNKNOWN";
}
