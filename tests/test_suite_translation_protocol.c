#include "../main/suite_translation_protocol.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OP "0123456789abcdef0123456789abcdef"
#define JOB "fedcba9876543210fedcba9876543210"
#define SHA "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define AUDIO 9644U

static const suite_translation_protocol_identity_t IDENTITY = {
    .operation_id = OP,
    .job_id = JOB,
    .generation = 7,
    .direction = SUITE_TR_PROTOCOL_DIRECTION_EN_ZH,
    .audio_bytes = AUDIO,
};

static suite_translation_protocol_view_t view_with_buffers(char *source,
                                                           size_t source_cap,
                                                           char *target,
                                                           size_t target_cap)
{
    suite_translation_protocol_view_t view;
    suite_translation_protocol_view_init(&view, source, source_cap, target,
                                         target_cap);
    return view;
}

static void expect_parse_ok(const char *json,
                            const suite_translation_protocol_identity_t *identity,
                            const char *source, const char *target)
{
    char source_buffer[SUITE_TR_PROTOCOL_TEXT_CAP];
    char target_buffer[SUITE_TR_PROTOCOL_TEXT_CAP];
    suite_translation_protocol_view_t view = view_with_buffers(
        source_buffer, sizeof(source_buffer), target_buffer,
        sizeof(target_buffer));
    suite_translation_protocol_error_t error =
        suite_translation_protocol_parse_view((const uint8_t *)json,
                                              strlen(json), identity, &view);
    if (error != SUITE_TR_PROTOCOL_OK) {
        fprintf(stderr, "expected OK, got %s\n",
                suite_translation_protocol_error_name(error));
        assert(0);
    }
    if (source) assert(!strcmp(view.source, source));
    if (target) assert(!strcmp(view.target, target));
}

static void expect_parse_error(const char *json,
                               const suite_translation_protocol_identity_t *identity,
                               suite_translation_protocol_error_t expected)
{
    char source_buffer[SUITE_TR_PROTOCOL_TEXT_CAP];
    char target_buffer[SUITE_TR_PROTOCOL_TEXT_CAP];
    suite_translation_protocol_view_t view = view_with_buffers(
        source_buffer, sizeof(source_buffer), target_buffer,
        sizeof(target_buffer));
    suite_translation_protocol_error_t error =
        suite_translation_protocol_parse_view((const uint8_t *)json,
                                              strlen(json), identity, &view);
    if (error != expected) {
        fprintf(stderr, "expected %s, got %s\n",
                suite_translation_protocol_error_name(expected),
                suite_translation_protocol_error_name(error));
        assert(0);
    }
}

static void expect_parse_len_error(const char *json, size_t length,
                                   const suite_translation_protocol_identity_t *identity,
                                   suite_translation_protocol_error_t expected)
{
    char source_buffer[SUITE_TR_PROTOCOL_TEXT_CAP];
    char target_buffer[SUITE_TR_PROTOCOL_TEXT_CAP];
    suite_translation_protocol_view_t view = view_with_buffers(
        source_buffer, sizeof(source_buffer), target_buffer,
        sizeof(target_buffer));
    suite_translation_protocol_error_t error =
        suite_translation_protocol_parse_view((const uint8_t *)json, length,
                                              identity, &view);
    if (error != expected) {
        fprintf(stderr, "expected %s, got %s\n",
                suite_translation_protocol_error_name(expected),
                suite_translation_protocol_error_name(error));
        assert(0);
    }
}

static void test_ready(void)
{
    suite_translation_protocol_ready_t ready;
    const char *good = "{\"schema\":1,\"ready\":true,\"provider\":\"mimo\"}";
    assert(suite_translation_protocol_parse_ready(
               (const uint8_t *)good, strlen(good), &ready) ==
           SUITE_TR_PROTOCOL_OK);
    assert(ready.ready);
    assert(!strcmp(ready.provider, "mimo"));

    const char *not_ready =
        "{\"schema\":1,\"ready\":false,\"provider\":\"mimo\",\"evolving\":[1,2]}";
    assert(suite_translation_protocol_parse_ready(
               (const uint8_t *)not_ready, strlen(not_ready), &ready) ==
           SUITE_TR_PROTOCOL_OK);
    assert(!ready.ready);

    const char *duplicate =
        "{\"schema\":1,\"ready\":true,\"ready\":true,\"provider\":\"mimo\"}";
    assert(suite_translation_protocol_parse_ready(
               (const uint8_t *)duplicate, strlen(duplicate), &ready) ==
           SUITE_TR_PROTOCOL_E_DUPLICATE);

    const char *wrong_provider =
        "{\"schema\":1,\"ready\":true,\"provider\":\"deepseek\"}";
    assert(suite_translation_protocol_parse_ready(
               (const uint8_t *)wrong_provider, strlen(wrong_provider),
               &ready) == SUITE_TR_PROTOCOL_E_PROVIDER);

    const char *wrong_schema =
        "{\"schema\":2,\"ready\":true,\"provider\":\"mimo\"}";
    assert(suite_translation_protocol_parse_ready(
               (const uint8_t *)wrong_schema, strlen(wrong_schema), &ready) ==
           SUITE_TR_PROTOCOL_E_SCHEMA);

    const char *missing = "{\"schema\":1,\"provider\":\"mimo\"}";
    assert(suite_translation_protocol_parse_ready(
               (const uint8_t *)missing, strlen(missing), &ready) ==
           SUITE_TR_PROTOCOL_E_MISSING);
}

static void test_admission(void)
{
    const char *json =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"ADMITTED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,"
        "\"audio_bytes\":9644,\"source_bytes\":0,\"target_bytes\":0,"
        "\"updated_at\":1000,\"error\":null,\"nested\":{\"schema\":1,\"a\":1}}";
    char source[SUITE_TR_PROTOCOL_TEXT_CAP];
    char target[SUITE_TR_PROTOCOL_TEXT_CAP];
    suite_translation_protocol_view_t view = view_with_buffers(
        source, sizeof(source), target, sizeof(target));
    assert(suite_translation_protocol_parse_admission(
               (const uint8_t *)json, strlen(json), OP,
               SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, AUDIO, &view) ==
           SUITE_TR_PROTOCOL_OK);
    assert(!strcmp(view.job_id, JOB));
    assert(view.generation == 7);
    assert(view.state == SUITE_TR_PROTOCOL_STATE_ADMITTED);

    assert(suite_translation_protocol_parse_admission(
               (const uint8_t *)json, strlen(json),
               "11111111111111111111111111111111",
               SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, AUDIO, &view) ==
           SUITE_TR_PROTOCOL_E_IDENTITY);
    assert(suite_translation_protocol_parse_admission(
               (const uint8_t *)json, strlen(json), OP,
               SUITE_TR_PROTOCOL_DIRECTION_ZH_EN, AUDIO, &view) ==
           SUITE_TR_PROTOCOL_E_IDENTITY);
    assert(suite_translation_protocol_parse_admission(
               (const uint8_t *)json, strlen(json), OP,
               SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, AUDIO + 2, &view) ==
           SUITE_TR_PROTOCOL_E_IDENTITY);

    const char *same_ids =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" OP "\","
        "\"state\":\"ADMITTED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":1,\"audio_bytes\":9644,"
        "\"source_bytes\":0,\"target_bytes\":0,\"updated_at\":1,"
        "\"error\":null}";
    assert(suite_translation_protocol_parse_admission(
               (const uint8_t *)same_ids, strlen(same_ids), OP,
               SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, AUDIO, &view) ==
           SUITE_TR_PROTOCOL_E_IDENTITY);

    const char *duplicate =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"nested\":{\"a\":1,"
        "\"a\":2},\"job_id\":\"" JOB "\",\"state\":\"ADMITTED\","
        "\"source_lang\":\"en\",\"target_lang\":\"zh-CN\","
        "\"generation\":1,\"audio_bytes\":9644,\"source_bytes\":0,"
        "\"target_bytes\":0,\"updated_at\":1,\"error\":null}";
    assert(suite_translation_protocol_parse_admission(
               (const uint8_t *)duplicate, strlen(duplicate), OP,
               SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, AUDIO, &view) ==
           SUITE_TR_PROTOCOL_E_DUPLICATE);

    const char *bad_id =
        "{\"schema\":1,\"operation_id\":\"ABCDEF0123456789abcdef0123456789\","
        "\"job_id\":\"" JOB "\",\"state\":\"ADMITTED\","
        "\"source_lang\":\"en\",\"target_lang\":\"zh-CN\","
        "\"generation\":1,\"audio_bytes\":9644,\"source_bytes\":0,"
        "\"target_bytes\":0,\"updated_at\":1,\"error\":null}";
    assert(suite_translation_protocol_parse_admission(
               (const uint8_t *)bad_id, strlen(bad_id), OP,
               SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, AUDIO, &view) ==
           SUITE_TR_PROTOCOL_E_ID);
}

static void test_done(void)
{
    const char *json =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"DONE\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":5,\"target_bytes\":6,\"updated_at\":1000,"
        "\"error\":null,\"text_lost\":false,\"source_text\":\"hello\","
        "\"target_text\":\"\\u4f60\\u597d\"}";
    expect_parse_ok(json, &IDENTITY, "hello", "\xe4\xbd\xa0\xe5\xa5\xbd");

    expect_parse_error(json, &(suite_translation_protocol_identity_t){
        .operation_id = OP, .job_id = JOB, .generation = 8,
        .direction = SUITE_TR_PROTOCOL_DIRECTION_EN_ZH,
        .audio_bytes = AUDIO}, SUITE_TR_PROTOCOL_E_IDENTITY);
    expect_parse_error(json, &(suite_translation_protocol_identity_t){
        .operation_id = JOB, .job_id = JOB, .generation = 7,
        .direction = SUITE_TR_PROTOCOL_DIRECTION_EN_ZH,
        .audio_bytes = AUDIO}, SUITE_TR_PROTOCOL_E_IDENTITY);
    expect_parse_error(json, &(suite_translation_protocol_identity_t){
        .operation_id = OP, .job_id = JOB, .generation = 7,
        .direction = SUITE_TR_PROTOCOL_DIRECTION_ZH_EN,
        .audio_bytes = AUDIO}, SUITE_TR_PROTOCOL_E_IDENTITY);
    expect_parse_error(json, &(suite_translation_protocol_identity_t){
        .operation_id = OP, .job_id = JOB, .generation = 7,
        .direction = SUITE_TR_PROTOCOL_DIRECTION_EN_ZH,
        .audio_bytes = AUDIO + 1}, SUITE_TR_PROTOCOL_E_IDENTITY);

    const char *mismatch =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"DONE\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":6,\"target_bytes\":6,\"updated_at\":1000,"
        "\"error\":null,\"text_lost\":false,\"source_text\":\"hello\","
        "\"target_text\":\"\\u4f60\\u597d\"}";
    expect_parse_error(mismatch, &IDENTITY, SUITE_TR_PROTOCOL_E_VALUE);

    const char *missing_target =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"DONE\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":5,\"target_bytes\":6,\"updated_at\":1000,"
        "\"error\":null,\"text_lost\":false,\"source_text\":\"hello\"}";
    expect_parse_error(missing_target, &IDENTITY, SUITE_TR_PROTOCOL_E_TEXT);

    const char *error_on_done =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"DONE\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":5,\"target_bytes\":6,\"updated_at\":1000,"
        "\"error\":\"UNAVAILABLE\",\"text_lost\":false,"
        "\"source_text\":\"hello\",\"target_text\":\"world!\"}";
    expect_parse_error(error_on_done, &IDENTITY, SUITE_TR_PROTOCOL_E_STATE);

    const char *spaced =
        "{ \"schema\" : 1 , \"operation_id\" : \"" OP "\" ,"
        " \"job_id\" : \"" JOB "\" , \"state\" : \"DONE\" ,"
        " \"source_lang\" : \"en\" , \"target_lang\" : \"zh-CN\" ,"
        " \"generation\" : 7 , \"audio_bytes\" : 9644 ,"
        " \"source_bytes\" : 5 , \"target_bytes\" : 6 ,"
        " \"updated_at\" : 1000 , \"error\" : null ,"
        " \"text_lost\" : false , \"source_text\" : \"hello\" ,"
        " \"target_text\" : \"\\u4f60\\u597d\" }";
    expect_parse_ok(spaced, &IDENTITY, "hello", "\xe4\xbd\xa0\xe5\xa5\xbd");
}

static void test_states(void)
{
    const char *failed =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"FAILED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":5,\"target_bytes\":0,\"updated_at\":1000,"
        "\"error\":\"UNAVAILABLE\",\"source_text\":\"hello\"}";
    expect_parse_ok(failed, &IDENTITY, "hello", NULL);

    const char *failed_target =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"FAILED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":5,\"target_bytes\":6,\"updated_at\":1000,"
        "\"error\":\"UNAVAILABLE\",\"source_text\":\"hello\","
        "\"target_text\":\"world!\"}";
    expect_parse_error(failed_target, &IDENTITY, SUITE_TR_PROTOCOL_E_STATE);

    const char *failed_no_error =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"FAILED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":0,\"target_bytes\":0,\"updated_at\":1000,"
        "\"error\":null}";
    expect_parse_error(failed_no_error, &IDENTITY, SUITE_TR_PROTOCOL_E_STATE);

    const char *text_lost =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"TEXT_LOST\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":5,\"target_bytes\":6,\"updated_at\":1000,"
        "\"error\":null,\"text_lost\":true}";
    expect_parse_ok(text_lost, &IDENTITY, NULL, NULL);

    const char *text_lost_false =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"TEXT_LOST\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":0,\"target_bytes\":0,\"updated_at\":1000,"
        "\"error\":null,\"text_lost\":false}";
    expect_parse_error(text_lost_false, &IDENTITY, SUITE_TR_PROTOCOL_E_STATE);

    const char *cancelled =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"CANCELLED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":0,\"target_bytes\":0,\"updated_at\":1000,"
        "\"error\":null}";
    expect_parse_ok(cancelled, &IDENTITY, NULL, NULL);

    const char *cancelled_text =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"CANCELLED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":5,\"target_bytes\":0,\"updated_at\":1000,"
        "\"error\":null,\"source_text\":\"hello\"}";
    expect_parse_error(cancelled_text, &IDENTITY, SUITE_TR_PROTOCOL_E_STATE);

    const char *unknown =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"UNKNOWN\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":5,\"target_bytes\":0,\"updated_at\":1000,"
        "\"error\":null}";
    expect_parse_ok(unknown, &IDENTITY, NULL, NULL);

    const char *expired =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"EXPIRED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":0,\"target_bytes\":0,\"updated_at\":1000,"
        "\"error\":\"AUDIO_EXPIRED\"}";
    expect_parse_ok(expired, &IDENTITY, NULL, NULL);

    const char *expired_text =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"EXPIRED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":5,\"target_bytes\":0,\"updated_at\":1000,"
        "\"error\":\"AUDIO_EXPIRED\",\"source_text\":\"hello\"}";
    expect_parse_error(expired_text, &IDENTITY, SUITE_TR_PROTOCOL_E_STATE);

    const char *shutdown =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"CANCELLED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":8,\"audio_bytes\":9644,"
        "\"source_bytes\":0,\"target_bytes\":0,\"updated_at\":1000,"
        "\"error\":\"SHUTDOWN\"}";
    char source[SUITE_TR_PROTOCOL_TEXT_CAP];
    char target[SUITE_TR_PROTOCOL_TEXT_CAP];
    suite_translation_protocol_view_t view = view_with_buffers(
        source, sizeof(source), target, sizeof(target));
    assert(suite_translation_protocol_parse_cancel_view(
               (const uint8_t *)shutdown, strlen(shutdown), &IDENTITY,
               &view) == SUITE_TR_PROTOCOL_OK);
    assert(view.state == SUITE_TR_PROTOCOL_STATE_CANCELLED);
    assert(!strcmp(view.error, "SHUTDOWN"));
}

static void test_cancel(void)
{
    const char *json =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"CANCELLED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":8,\"audio_bytes\":9644,"
        "\"source_bytes\":0,\"target_bytes\":0,\"updated_at\":1000,"
        "\"error\":null}";
    char source[SUITE_TR_PROTOCOL_TEXT_CAP];
    char target[SUITE_TR_PROTOCOL_TEXT_CAP];
    suite_translation_protocol_view_t view = view_with_buffers(
        source, sizeof(source), target, sizeof(target));
    assert(suite_translation_protocol_parse_cancel_view(
               (const uint8_t *)json, strlen(json), &IDENTITY, &view) ==
           SUITE_TR_PROTOCOL_OK);

    const char *too_far =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"CANCELLED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":9,\"audio_bytes\":9644,"
        "\"source_bytes\":0,\"target_bytes\":0,\"updated_at\":1000,"
        "\"error\":null}";
    assert(suite_translation_protocol_parse_cancel_view(
               (const uint8_t *)too_far, strlen(too_far), &IDENTITY, &view) ==
           SUITE_TR_PROTOCOL_E_IDENTITY);

    const char *done =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"DONE\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":5,\"target_bytes\":5,\"updated_at\":1000,"
        "\"error\":null,\"source_text\":\"hello\",\"target_text\":\"world\"}";
    assert(suite_translation_protocol_parse_cancel_view(
               (const uint8_t *)done, strlen(done), &IDENTITY, &view) ==
           SUITE_TR_PROTOCOL_E_STATE);
}

static void test_document_errors(void)
{
    const char *duplicate =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"CANCELLED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":0,\"target_bytes\":0,\"updated_at\":1000,"
        "\"error\":null,\"error\":null}";
    expect_parse_error(duplicate, &IDENTITY, SUITE_TR_PROTOCOL_E_DUPLICATE);

    const char *escaped_nul =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"CANCELLED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":0,\"target_bytes\":0,\"updated_at\":1000,"
        "\"error\":\"A\\u0000B\"}";
    expect_parse_error(escaped_nul, &IDENTITY, SUITE_TR_PROTOCOL_E_NUL);

    const char *surrogate =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"CANCELLED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":0,\"target_bytes\":0,\"updated_at\":1000,"
        "\"error\":\"\\ud800\"}";
    expect_parse_error(surrogate, &IDENTITY, SUITE_TR_PROTOCOL_E_SURROGATE);

    const char *trailing =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"CANCELLED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":0,\"target_bytes\":0,\"updated_at\":1000,"
        "\"error\":null}x";
    expect_parse_error(trailing, &IDENTITY, SUITE_TR_PROTOCOL_E_TRAILING);

    const char *wrong_type =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"CANCELLED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":\"7\","
        "\"audio_bytes\":9644,\"source_bytes\":0,\"target_bytes\":0,"
        "\"updated_at\":1000,\"error\":null}";
    expect_parse_error(wrong_type, &IDENTITY, SUITE_TR_PROTOCOL_E_TYPE);

    const char *overflow =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"CANCELLED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":0,\"target_bytes\":0,"
        "\"updated_at\":999999999999999999999999999999,"
        "\"error\":null}";
    expect_parse_error(overflow, &IDENTITY, SUITE_TR_PROTOCOL_E_OVERFLOW);

    const char *depth =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"CANCELLED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":0,\"target_bytes\":0,\"updated_at\":1000,"
        "\"error\":null,\"x\":[[[[[[[[1]]]]]]]]}";
    expect_parse_error(depth, &IDENTITY, SUITE_TR_PROTOCOL_E_DEPTH);

    const char invalid[] =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"CANCELLED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":0,\"target_bytes\":0,\"updated_at\":1000,"
        "\"error\":\"A\xff\"}";
    expect_parse_len_error(invalid, sizeof(invalid) - 1, &IDENTITY,
                           SUITE_TR_PROTOCOL_E_UTF8);

    const char *bad_direction =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"CANCELLED\",\"source_lang\":\"zh-CN\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":0,\"target_bytes\":0,\"updated_at\":1000,"
        "\"error\":null}";
    expect_parse_error(bad_direction, &IDENTITY, SUITE_TR_PROTOCOL_E_DIRECTION);

    const char *zero_generation =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"CANCELLED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":0,\"audio_bytes\":9644,"
        "\"source_bytes\":0,\"target_bytes\":0,\"updated_at\":1000,"
        "\"error\":null}";
    expect_parse_error(zero_generation, &IDENTITY, SUITE_TR_PROTOCOL_E_VALUE);

    const char *small_audio =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"CANCELLED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":100,"
        "\"source_bytes\":0,\"target_bytes\":0,\"updated_at\":1000,"
        "\"error\":null}";
    expect_parse_error(small_audio, &IDENTITY, SUITE_TR_PROTOCOL_E_VALUE);

    const char *array_root =
        "[\"" OP "\",\"" JOB "\"]";
    expect_parse_error(array_root, &IDENTITY, SUITE_TR_PROTOCOL_E_JSON);

    const char *done_lost =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"DONE\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":5,\"target_bytes\":5,\"updated_at\":1000,"
        "\"error\":null,\"text_lost\":true,\"source_text\":\"hello\","
        "\"target_text\":\"world\"}";
    expect_parse_error(done_lost, &IDENTITY, SUITE_TR_PROTOCOL_E_STATE);

    const char *escaped_duplicate =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"CANCELLED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":0,\"target_bytes\":0,\"updated_at\":1000,"
        "\"error\":null,\"\\u0073chema\":1}";
    expect_parse_error(escaped_duplicate, &IDENTITY,
                       SUITE_TR_PROTOCOL_E_DUPLICATE);
}

static void test_text_limits(void)
{
    static char json[SUITE_TR_PROTOCOL_JSON_MAX + 1];
    static char source[2060];
    static char target[2060];

    for (size_t i = 0; i < 512; i++)
        memcpy(source + i * 4, "\xf0\x9f\x98\x80", 4);
    source[2048] = 0;
    for (size_t i = 0; i < 512; i++)
        memcpy(target + i * 4, "\xf0\x9f\x98\x81", 4);
    target[2048] = 0;
    int n = snprintf(json, sizeof(json),
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"DONE\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":2048,\"target_bytes\":2048,\"updated_at\":1000,"
        "\"error\":null,\"source_text\":\"%s\",\"target_text\":\"%s\"}",
        source, target);
    assert(n > 0 && (size_t)n < sizeof(json));
    expect_parse_ok(json, &IDENTITY, source, target);

    for (size_t i = 0; i < 513; i++)
        memcpy(source + i * 4, "\xf0\x9f\x98\x80", 4);
    source[2052] = 0;
    n = snprintf(json, sizeof(json),
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"DONE\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":2048,\"target_bytes\":2048,\"updated_at\":1000,"
        "\"error\":null,\"source_text\":\"%s\",\"target_text\":\"%s\"}",
        source, target);
    expect_parse_error(json, &IDENTITY, SUITE_TR_PROTOCOL_E_TEXT_BYTES);

    memset(source, 'a', 513);
    source[513] = 0;
    n = snprintf(json, sizeof(json),
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"DONE\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":513,\"target_bytes\":2048,\"updated_at\":1000,"
        "\"error\":null,\"source_text\":\"%s\",\"target_text\":\"%s\"}",
        source, target);
    expect_parse_error(json, &IDENTITY, SUITE_TR_PROTOCOL_E_TEXT_SCALARS);

    for (size_t i = 0; i < 512; i++)
        memcpy(source + i * 4, "\xf0\x9f\x98\x80", 4);
    source[2048] = 0;
    for (size_t i = 0; i < 512; i++)
        memcpy(target + i * 4, "\xf0\x9f\x98\x81", 4);
    target[2048] = 0;
    n = snprintf(json, sizeof(json),
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"DONE\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":2048,\"target_bytes\":2048,\"updated_at\":1000,"
        "\"error\":null,\"source_text\":\"%s\",\"target_text\":\"%s\"}",
        source, target);
    expect_parse_ok(json, &IDENTITY, source, target);
}

static void test_error_and_builder(void)
{
    char code[SUITE_TR_PROTOCOL_ERROR_CODE_MAX + 1];
    const char *error = "{\"error\":\"QUOTA\",\"extra\":true}";
    assert(suite_translation_protocol_parse_error(
               (const uint8_t *)error, strlen(error), code, sizeof(code)) ==
           SUITE_TR_PROTOCOL_OK);
    assert(!strcmp(code, "QUOTA"));

    const char *bad = "{\"error\":\"quota\"}";
    assert(suite_translation_protocol_parse_error(
               (const uint8_t *)bad, strlen(bad), code, sizeof(code)) ==
           SUITE_TR_PROTOCOL_E_VALUE);

    char body[320];
    size_t length = 0;
    assert(suite_translation_protocol_build_admission(
               body, sizeof(body), OP,
               SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA, AUDIO, &length) ==
           SUITE_TR_PROTOCOL_OK);
    const char *expected =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"format\":\"wav\",\"audio_sha256\":\""
        SHA "\",\"audio_bytes\":9644}";
    assert(length == strlen(expected));
    assert(!strcmp(body, expected));

    assert(suite_translation_protocol_build_admission(
               body, 10, OP, SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA, AUDIO,
               &length) == SUITE_TR_PROTOCOL_E_LIMIT);
    assert(suite_translation_protocol_build_admission(
               body, sizeof(body), "ABCDEF0123456789abcdef0123456789",
               SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA, AUDIO, &length) ==
           SUITE_TR_PROTOCOL_E_ARGUMENT);
}

int main(void)
{
    test_ready();
    test_admission();
    test_done();
    test_states();
    test_cancel();
    test_document_errors();
    test_text_limits();
    test_error_and_builder();
    puts("translation protocol tests: PASS");
    return 0;
}
