#include "suite_translation_core.h"
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static uint8_t wav[TR_WAV_MAX_BYTES + 8U];
static char text[TR_TEXT_MAX_BYTES + 8U];
static const char *token = "tttttttttttttttttttttttttttttttttttttttttttttttt";

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static size_t make_wav(size_t payload)
{
    memset(wav, 0, 44 + payload);
    memcpy(wav, "RIFF", 4);
    put32(wav + 4, (uint32_t)(36 + payload));
    memcpy(wav + 8, "WAVE", 4);
    memcpy(wav + 12, "fmt ", 4);
    put32(wav + 16, 16);
    put16(wav + 20, 1);
    put16(wav + 22, 1);
    put32(wav + 24, 16000);
    put32(wav + 28, 32000);
    put16(wav + 32, 2);
    put16(wav + 34, 16);
    memcpy(wav + 36, "data", 4);
    put32(wav + 40, (uint32_t)payload);
    for (size_t i = 0; i < payload; ++i)
        wav[44 + i] = (uint8_t)(i * 17U + 3U);
    return 44 + payload;
}

static void operation_id(char out[33], unsigned value)
{
    static const char hex[] = "0123456789abcdef";
    memset(out, '0', 32);
    out[32] = 0;
    for (unsigned i = 0; i < 8; ++i)
        out[31 - i] = hex[(value >> (i * 4U)) & 0xfU];
}

static tr_capability_t capability(void)
{
    tr_capability_t value;
    memset(&value, 0, sizeof(value));
    if (tr_capability_init(&value, TR_SCOPE_TRANSLATION, token, strlen(token)) != TR_OK)
        value.scope = (tr_scope_t)0;
    return value;
}

static tr_error_t admit(tr_core_t *core, const tr_capability_t *cap,
                        const char *operation, tr_direction_t direction,
                        const char *sha, uint32_t bytes, tr_job_t *job)
{
    tr_admission_t request = {
        .operation_id = operation,
        .direction = direction,
        .audio_sha256 = sha,
        .audio_bytes = bytes
    };
    return tr_core_admit(core, cap, &request, 100, job);
}

static int test_sha256(void)
{
    char digest[TR_SHA256_SIZE];
    CHECK(tr_sha256_hex("", 0, digest));
    CHECK(!strcmp(digest, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK(tr_sha256_hex("abc", 3, digest));
    CHECK(!strcmp(digest, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char *longer = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(tr_sha256_hex(longer, strlen(longer), digest));
    CHECK(!strcmp(digest, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    return 0;
}

static int test_text_limits(void)
{
    uint32_t scalars = 0;
    memset(text, 'a', 512);
    text[512] = 0;
    CHECK(tr_text_check(text, 512, &scalars) == TR_OK);
    CHECK(scalars == 512);
    memset(text, 'a', 513);
    CHECK(tr_text_check(text, 513, NULL) == TR_E_TEXT_SCALARS);

    for (unsigned i = 0; i < 512; ++i) {
        text[i * 4] = (char)0xf0;
        text[i * 4 + 1] = (char)0x9f;
        text[i * 4 + 2] = (char)0x98;
        text[i * 4 + 3] = (char)0x80;
    }
    CHECK(tr_text_check(text, 2048, &scalars) == TR_OK);
    CHECK(scalars == 512);
    text[2048] = (char)0xf0;
    text[2049] = (char)0x9f;
    text[2050] = (char)0x98;
    text[2051] = (char)0x80;
    CHECK(tr_text_check(text, 2052, NULL) == TR_E_TEXT_BYTES);

    CHECK(tr_text_check("\xed\xa0\x80", 3, NULL) == TR_E_TEXT);
    CHECK(tr_text_check("\xc0\xaf", 2, NULL) == TR_E_TEXT);
    CHECK(tr_text_check("\xf4\x90\x80\x80", 4, NULL) == TR_E_TEXT);
    CHECK(tr_text_check("\xe4\xb8", 2, NULL) == TR_E_TEXT);
    CHECK(tr_text_check("a\x00b", 3, NULL) == TR_E_TEXT);
    CHECK(tr_text_check("", 0, NULL) == TR_E_TEXT);
    CHECK(tr_text_check(NULL, 1, NULL) == TR_E_TEXT);
    return 0;
}

static int test_wav_limits(void)
{
    tr_wav_info_t info;
    size_t length = make_wav(9600);
    CHECK(tr_wav_check(wav, length, &info) == TR_OK);
    CHECK(info.bytes == length && info.data_bytes == 9600 && info.seconds == 1);

    length = make_wav(320000);
    CHECK(tr_wav_check(wav, length, &info) == TR_OK);
    CHECK(info.bytes == TR_WAV_MAX_BYTES && info.seconds == 10);

    CHECK(tr_wav_check(wav, make_wav(320002), NULL) == TR_E_AUDIO_BYTES);
    CHECK(tr_wav_check(wav, make_wav(9598), NULL) == TR_E_AUDIO_BYTES);
    CHECK(tr_wav_check(wav, make_wav(9601), NULL) == TR_E_AUDIO_FORMAT);
    CHECK(tr_wav_check(NULL, 44, NULL) == TR_E_AUDIO);
    CHECK(tr_wav_check(wav, 43, NULL) == TR_E_AUDIO);

    length = make_wav(9600);
    wav[0] = 'X';
    CHECK(tr_wav_check(wav, length, NULL) == TR_E_AUDIO_FORMAT);
    length = make_wav(9600);
    put32(wav + 24, 8000);
    CHECK(tr_wav_check(wav, length, NULL) == TR_E_AUDIO_FORMAT);
    length = make_wav(9600);
    put16(wav + 22, 2);
    CHECK(tr_wav_check(wav, length, NULL) == TR_E_AUDIO_FORMAT);
    length = make_wav(9600);
    put16(wav + 34, 8);
    CHECK(tr_wav_check(wav, length, NULL) == TR_E_AUDIO_FORMAT);
    length = make_wav(9600);
    put32(wav + 16, 18);
    CHECK(tr_wav_check(wav, length, NULL) == TR_E_AUDIO_FORMAT);
    length = make_wav(9600);
    put32(wav + 40, 9598);
    CHECK(tr_wav_check(wav, length, NULL) == TR_E_AUDIO_FORMAT);
    return 0;
}

static int test_capability(void)
{
    tr_capability_t value;
    CHECK(tr_capability_init(&value, TR_SCOPE_TRANSLATION, token, strlen(token)) == TR_OK);
    CHECK(value.scope == TR_SCOPE_TRANSLATION);
    CHECK(tr_capability_init(&value, TR_SCOPE_TASKS_READER, token, strlen(token)) == TR_E_SCOPE);
    CHECK(tr_capability_init(&value, TR_SCOPE_TASKS_WRITER, token, strlen(token)) == TR_E_SCOPE);
    CHECK(tr_capability_init(&value, TR_SCOPE_TASKS_ADMIN, token, strlen(token)) == TR_E_SCOPE);
    CHECK(tr_capability_init(&value, TR_SCOPE_TRANSLATION, "short", 5) == TR_E_TOKEN);
    CHECK(tr_capability_init(&value, TR_SCOPE_TRANSLATION, token, 31) == TR_E_TOKEN);
    CHECK(tr_capability_init(&value, TR_SCOPE_TRANSLATION, token, 129) == TR_E_TOKEN);
    CHECK(tr_capability_init(&value, TR_SCOPE_TRANSLATION, "token with space", 15) == TR_E_TOKEN);
    CHECK(tr_capability_init(NULL, TR_SCOPE_TRANSLATION, token, strlen(token)) == TR_E_ARGUMENT);
    return 0;
}

static int test_exact_ids_and_hashes(void)
{
    tr_core_t core;
    tr_capability_t cap = capability();
    tr_job_t job;
    char op[40];
    char sha[80];
    size_t length = make_wav(9600);
    char digest[TR_SHA256_SIZE];
    CHECK(tr_sha256_hex(wav, length, digest));

    tr_core_init(&core);
    operation_id(op, 7);
    CHECK(admit(&core, &cap, op, TR_DIRECTION_EN_ZH, digest,
                (uint32_t)length, &job) == TR_OK);
    CHECK(tr_core_count(&core) == 1 && core.generation == 1);
    CHECK(admit(&core, &cap, op, TR_DIRECTION_EN_ZH, digest,
                (uint32_t)length, &job) == TR_OK);
    CHECK(tr_core_count(&core) == 1 && core.generation == 1);

    tr_core_init(&core);
    memset(op, 'a', 31);
    op[31] = 0;
    CHECK(admit(&core, &cap, op, TR_DIRECTION_EN_ZH, digest,
                (uint32_t)length, NULL) == TR_E_ID);
    CHECK(tr_core_count(&core) == 0 && core.generation == 0);
    memset(op, 'a', 32);
    op[32] = 0;
    CHECK(admit(&core, &cap, op, TR_DIRECTION_EN_ZH, digest,
                (uint32_t)length, &job) == TR_OK);
    CHECK(tr_core_count(&core) == 1 && core.generation == 1);
    memset(op, 'a', 33);
    op[33] = 0;
    CHECK(admit(&core, &cap, op, TR_DIRECTION_EN_ZH, digest,
                (uint32_t)length, NULL) == TR_E_ID);
    CHECK(tr_core_count(&core) == 1 && core.generation == 1);
    memset(op, 'a', 32);
    op[32] = 'g';
    op[33] = 0;
    CHECK(admit(&core, &cap, op, TR_DIRECTION_EN_ZH, digest,
                (uint32_t)length, NULL) == TR_E_ID);
    CHECK(tr_core_count(&core) == 1 && core.generation == 1);
    memset(op, 'a', 31);
    op[31] = 'g';
    op[32] = 0;
    CHECK(admit(&core, &cap, op, TR_DIRECTION_EN_ZH, digest,
                (uint32_t)length, NULL) == TR_E_ID);
    CHECK(tr_core_count(&core) == 1 && core.generation == 1);
    CHECK(tr_core_get(&core, op, &job) == TR_E_ID);

    tr_core_init(&core);
    operation_id(op, 9);
    memset(sha, 'b', 63);
    sha[63] = 0;
    CHECK(admit(&core, &cap, op, TR_DIRECTION_EN_ZH, sha,
                (uint32_t)length, NULL) == TR_E_SHA256);
    CHECK(tr_core_count(&core) == 0 && core.generation == 0);
    memset(sha, 'b', 64);
    sha[64] = 0;
    CHECK(admit(&core, &cap, op, TR_DIRECTION_EN_ZH, sha,
                (uint32_t)length, &job) == TR_OK);
    CHECK(tr_core_count(&core) == 1 && core.generation == 1);
    memset(sha, 'b', 65);
    sha[65] = 0;
    CHECK(admit(&core, &cap, op, TR_DIRECTION_EN_ZH, sha,
                (uint32_t)length, NULL) == TR_E_SHA256);
    CHECK(tr_core_count(&core) == 1 && core.generation == 1);
    memset(sha, 'b', 64);
    sha[64] = 'g';
    sha[65] = 0;
    CHECK(admit(&core, &cap, op, TR_DIRECTION_EN_ZH, sha,
                (uint32_t)length, NULL) == TR_E_SHA256);
    CHECK(tr_core_count(&core) == 1 && core.generation == 1);
    memset(sha, 'b', 63);
    sha[63] = 'g';
    sha[64] = 0;
    CHECK(admit(&core, &cap, op, TR_DIRECTION_EN_ZH, sha,
                (uint32_t)length, NULL) == TR_E_SHA256);
    CHECK(tr_core_count(&core) == 1 && core.generation == 1);
    return 0;
}

static int test_admission_replay_conflict(void)
{
    tr_core_t core;
    tr_capability_t cap = capability();
    tr_job_t first, replay;
    char sha[TR_SHA256_SIZE];
    char op[33];
    size_t length = make_wav(9600);
    operation_id(op, 1);
    CHECK(tr_sha256_hex(wav, length, sha));
    tr_core_init(&core);
    CHECK(admit(&core, &cap, op, TR_DIRECTION_EN_ZH, sha, (uint32_t)length, &first) == TR_OK);
    CHECK(first.state == TR_STATE_ADMITTED);
    CHECK(strlen(first.operation_id) == 32 && strlen(first.job_id) == 32);
    CHECK(first.generation == 1 && first.created_at == 100);
    CHECK(strcmp(first.operation_id, op) == 0);

    CHECK(admit(&core, &cap, op, TR_DIRECTION_EN_ZH, sha, (uint32_t)length, &replay) == TR_OK);
    CHECK(!strcmp(first.job_id, replay.job_id) && first.generation == replay.generation);
    CHECK(tr_core_count(&core) == 1);
    CHECK(tr_core_event_count(&core) == 2);
    CHECK(tr_core_event_at(&core, 1) == TR_EVENT_REPLAYED);

    CHECK(admit(&core, &cap, op, TR_DIRECTION_ZH_EN, sha, (uint32_t)length, NULL) == TR_E_CONFLICT);
    CHECK(admit(&core, &cap, op, TR_DIRECTION_EN_ZH, sha, (uint32_t)length + 2U, NULL) == TR_E_CONFLICT);
    char other[TR_SHA256_SIZE];
    memset(other, '0', 64);
    other[64] = 0;
    CHECK(admit(&core, &cap, op, TR_DIRECTION_EN_ZH, other, (uint32_t)length, NULL) == TR_E_CONFLICT);
    CHECK(tr_core_count(&core) == 1);
    return 0;
}

static int test_capacity(void)
{
    tr_core_t core;
    tr_capability_t cap = capability();
    tr_job_t job;
    char sha[TR_SHA256_SIZE];
    char op[33];
    size_t length = make_wav(9600);
    CHECK(tr_sha256_hex(wav, length, sha));
    tr_core_init(&core);
    for (unsigned i = 1; i <= TR_METADATA_SLOTS; ++i) {
        operation_id(op, i);
        CHECK(admit(&core, &cap, op, TR_DIRECTION_EN_ZH, sha, (uint32_t)length, &job) == TR_OK);
    }
    CHECK(tr_core_count(&core) == TR_METADATA_SLOTS);
    operation_id(op, TR_METADATA_SLOTS + 1);
    CHECK(admit(&core, &cap, op, TR_DIRECTION_EN_ZH, sha, (uint32_t)length, NULL) == TR_E_BUSY);
    CHECK(tr_core_count(&core) == TR_METADATA_SLOTS);
    operation_id(op, 1);
    CHECK(admit(&core, &cap, op, TR_DIRECTION_EN_ZH, sha, (uint32_t)length, &job) == TR_OK);
    CHECK(tr_core_count(&core) == TR_METADATA_SLOTS);
    return 0;
}

static int prepare(tr_core_t *core, tr_capability_t *cap, tr_job_t *job,
                   unsigned number, size_t payload)
{
    char sha[TR_SHA256_SIZE];
    char op[33];
    size_t length = make_wav(payload);
    operation_id(op, number);
    CHECK(tr_sha256_hex(wav, length, sha));
    return admit(core, cap, op, TR_DIRECTION_EN_ZH, sha, (uint32_t)length, job) == TR_OK ? 0 : 1;
}

static int test_upload_identity(void)
{
    tr_core_t core;
    tr_capability_t cap = capability();
    tr_job_t job, current;
    tr_core_init(&core);
    CHECK(prepare(&core, &cap, &job, 1, 9600) == 0);
    size_t length = make_wav(9600);
    CHECK(tr_core_upload(&core, &cap, job.operation_id, "00000000000000000000000000000000",
                         job.generation, wav, length, 101) == TR_E_JOB);
    CHECK(tr_core_upload(&core, &cap, job.operation_id, job.job_id,
                         job.generation + 1, wav, length, 101) == TR_E_GENERATION);
    CHECK(tr_core_upload(&core, &cap, job.operation_id, job.job_id,
                         job.generation, wav, make_wav(9602), 101) == TR_E_AUDIO_BYTES);
    length = make_wav(9600);
    CHECK(tr_core_upload(&core, &cap, job.operation_id, job.job_id,
                         job.generation, wav, length, 101) == TR_OK);
    CHECK(tr_core_get(&core, job.operation_id, &current) == TR_OK);
    CHECK(current.state == TR_STATE_UPLOADED);
    CHECK(tr_core_running(&core, &cap, job.operation_id, job.job_id,
                          job.generation, 101) == TR_OK);
    CHECK(tr_core_running(&core, &cap, job.operation_id, job.job_id,
                          job.generation, 101) == TR_OK);
    CHECK(tr_core_get(&core, job.operation_id, &current) == TR_OK);
    CHECK(current.state == TR_STATE_RUNNING);

    CHECK(tr_core_upload(&core, &cap, job.operation_id, job.job_id,
                         job.generation, wav, length, 102) == TR_OK);
    CHECK(tr_core_get(&core, job.operation_id, &current) == TR_OK);
    CHECK(current.state == TR_STATE_RUNNING);

    wav[44] ^= 1;
    CHECK(tr_core_upload(&core, &cap, job.operation_id, job.job_id,
                         job.generation, wav, length, 103) == TR_E_AUDIO_CONFLICT);
    CHECK(tr_core_get(&core, job.operation_id, &current) == TR_OK);
    CHECK(current.state == TR_STATE_RUNNING);
    return 0;
}

static int test_results(void)
{
    tr_core_t core;
    tr_capability_t cap = capability();
    tr_job_t job, current;
    tr_core_init(&core);
    CHECK(prepare(&core, &cap, &job, 1, 9600) == 0);
    size_t length = make_wav(9600);
    CHECK(tr_core_upload(&core, &cap, job.operation_id, job.job_id,
                         job.generation, wav, length, 101) == TR_OK);
    CHECK(tr_core_running(&core, &cap, job.operation_id, job.job_id,
                          job.generation, 101) == TR_OK);

    CHECK(tr_core_result(&core, &cap, job.operation_id, job.job_id, job.generation,
                         "source", 6, NULL, 0, TR_STATE_FAILED, 102) == TR_OK);
    CHECK(tr_core_get(&core, job.operation_id, &current) == TR_OK);
    CHECK(current.state == TR_STATE_FAILED && current.has_source == 1 && current.has_target == 0);
    CHECK(current.source_bytes == 6 && current.target_bytes == 0);
    CHECK(tr_core_result(&core, &cap, job.operation_id, job.job_id, job.generation,
                         "source", 6, "target", 6, TR_STATE_DONE, 103) == TR_E_STATE);
    CHECK(tr_core_get(&core, job.operation_id, &current) == TR_OK);
    CHECK(current.state == TR_STATE_FAILED);

    tr_core_init(&core);
    CHECK(prepare(&core, &cap, &job, 2, 9600) == 0);
    length = make_wav(9600);
    CHECK(tr_core_upload(&core, &cap, job.operation_id, job.job_id,
                         job.generation, wav, length, 101) == TR_OK);
    CHECK(tr_core_result(&core, &cap, job.operation_id, job.job_id, job.generation,
                         "source", 6, NULL, 0, TR_STATE_DONE, 102) == TR_E_ARGUMENT);
    CHECK(tr_core_result(&core, &cap, job.operation_id, job.job_id, job.generation,
                         NULL, 0, "target", 6, TR_STATE_DONE, 102) == TR_E_ARGUMENT);
    CHECK(tr_core_result(&core, &cap, job.operation_id, job.job_id, job.generation,
                         "source", 6, "target", 6, TR_STATE_FAILED, 102) == TR_E_ARGUMENT);
    CHECK(tr_core_result(&core, &cap, job.operation_id, job.job_id, job.generation,
                         "\xc0\xaf", 2, "target", 6, TR_STATE_DONE, 102) == TR_E_TEXT);
    CHECK(tr_core_get(&core, job.operation_id, &current) == TR_OK);
    CHECK(current.state == TR_STATE_UPLOADED);
    CHECK(tr_core_result(&core, &cap, job.operation_id, job.job_id, job.generation + 1,
                         "source", 6, "target", 6, TR_STATE_DONE, 102) == TR_E_GENERATION);
    CHECK(tr_core_result(&core, &cap, job.operation_id, job.job_id, job.generation,
                         "source", 6, "target", 6, TR_STATE_DONE, 102) == TR_OK);
    CHECK(tr_core_get(&core, job.operation_id, &current) == TR_OK);
    CHECK(current.state == TR_STATE_DONE && current.has_source == 1 && current.has_target == 1);
    CHECK(current.source_bytes == 6 && current.target_bytes == 6);

    tr_core_init(&core);
    CHECK(prepare(&core, &cap, &job, 3, 9600) == 0);
    length = make_wav(9600);
    CHECK(tr_core_upload(&core, &cap, job.operation_id, job.job_id,
                         job.generation, wav, length, 101) == TR_OK);
    CHECK(tr_core_result(&core, &cap, job.operation_id, job.job_id, job.generation,
                         NULL, 0, NULL, 0, TR_STATE_UNKNOWN, 102) == TR_OK);
    CHECK(tr_core_get(&core, job.operation_id, &current) == TR_OK);
    CHECK(current.state == TR_STATE_UNKNOWN && current.has_source == 0 && current.has_target == 0);
    return 0;
}

static int test_cancel_and_generation(void)
{
    tr_core_t core;
    tr_capability_t cap = capability();
    tr_job_t job, current;
    tr_core_init(&core);
    CHECK(prepare(&core, &cap, &job, 1, 9600) == 0);
    size_t length = make_wav(9600);
    CHECK(tr_core_upload(&core, &cap, job.operation_id, job.job_id,
                         job.generation, wav, length, 101) == TR_OK);
    CHECK(tr_core_running(&core, &cap, job.operation_id, job.job_id,
                          job.generation, 101) == TR_OK);
    CHECK(tr_core_cancel(&core, &cap, job.operation_id, job.job_id,
                         job.generation, 102) == TR_OK);
    CHECK(tr_core_get(&core, job.operation_id, &current) == TR_OK);
    CHECK(current.state == TR_STATE_CANCELLED && current.has_source == 0 && current.has_target == 0);
    CHECK(tr_core_cancel(&core, &cap, job.operation_id, job.job_id,
                         job.generation, 103) == TR_OK);
    CHECK(tr_core_result(&core, &cap, job.operation_id, job.job_id, job.generation,
                         "source", 6, "target", 6, TR_STATE_DONE, 104) == TR_E_CANCELLED);
    CHECK(tr_core_upload(&core, &cap, job.operation_id, job.job_id,
                         job.generation, wav, length, 105) == TR_E_CANCELLED);
    CHECK(tr_core_get(&core, job.operation_id, &current) == TR_OK);
    CHECK(current.state == TR_STATE_CANCELLED);

    tr_core_init(&core);
    CHECK(prepare(&core, &cap, &job, 2, 9600) == 0);
    length = make_wav(9600);
    CHECK(tr_core_upload(&core, &cap, job.operation_id, job.job_id,
                         job.generation, wav, length, 101) == TR_OK);
    CHECK(tr_core_result(&core, &cap, job.operation_id, job.job_id, job.generation,
                         "source", 6, "target", 6, TR_STATE_DONE, 102) == TR_OK);
    CHECK(tr_core_cancel(&core, &cap, job.operation_id, job.job_id,
                         job.generation, 103) == TR_E_STATE);

    tr_core_init(&core);
    tr_job_t one, two;
    CHECK(prepare(&core, &cap, &one, 3, 9600) == 0);
    CHECK(prepare(&core, &cap, &two, 4, 9600) == 0);
    uint32_t before = core.generation;
    CHECK(tr_core_invalidate(&core, 110) == 2);
    CHECK(core.generation == before + 1);
    CHECK(tr_core_get(&core, one.operation_id, &current) == TR_OK);
    CHECK(current.state == TR_STATE_CANCELLED);
    CHECK(tr_core_get(&core, two.operation_id, &current) == TR_OK);
    CHECK(current.state == TR_STATE_CANCELLED);
    CHECK(tr_core_result(&core, &cap, one.operation_id, one.job_id, one.generation,
                         "source", 6, "target", 6, TR_STATE_DONE, 111) == TR_E_CANCELLED);
    return 0;
}

static int test_metadata_is_bounded(void)
{
    CHECK(sizeof(tr_job_t) < 200);
    CHECK(TR_METADATA_SLOTS == 8);
    CHECK(TR_TEXT_MAX_BYTES == 2048);
    CHECK(TR_TEXT_MAX_SCALARS == 512);
    CHECK(TR_WAV_MAX_BYTES == 320044);
    return 0;
}

int main(void)
{
    if (test_sha256()) return 1;
    if (test_text_limits()) return 1;
    if (test_wav_limits()) return 1;
    if (test_capability()) return 1;
    if (test_exact_ids_and_hashes()) return 1;
    if (test_admission_replay_conflict()) return 1;
    if (test_capacity()) return 1;
    if (test_upload_identity()) return 1;
    if (test_results()) return 1;
    if (test_cancel_and_generation()) return 1;
    if (test_metadata_is_bounded()) return 1;
    puts("suite_translation_core: PASS");
    return 0;
}
