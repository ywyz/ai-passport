#include "suite_translation_provider.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

/*
 * Synthetic in-firmware provider used by the translation UI mock.
 *
 * This is not the Python companion, an HTTP client or a production ASR/MT
 * service. It performs no network, storage, credential or payment action. It
 * only returns fictional text for both supported directions after a short
 * bounded delay so the processing state is visible on the device.
 */

static const char *k_english_source =
    "Good morning. The train to the city leaves at nine o'clock. "
    "Please take your bag with you.";
static const char *k_chinese_source =
    "早上好。去城里的火车九点出发。请带上你的包。";

static bool copy_text(char *out, size_t cap, const char *text)
{
    if (!out || !cap || !text) return false;
    size_t length = strlen(text);
    if (length + 1U > cap) return false;
    memcpy(out, text, length + 1U);
    return true;
}

static suite_translation_provider_result_t mock_run(tr_direction_t direction,
                                                    char *source,
                                                    size_t source_cap,
                                                    char *target,
                                                    size_t target_cap)
{
    const char *source_text = direction == TR_DIRECTION_ZH_EN
                                  ? k_chinese_source
                                  : k_english_source;
    const char *target_text = direction == TR_DIRECTION_ZH_EN
                                  ? k_english_source
                                  : k_chinese_source;
    if (!copy_text(source, source_cap, source_text) ||
        !copy_text(target, target_cap, target_text))
        return SUITE_TR_PROVIDER_TRANSLATE_FAILED;

    /* Bounded synthetic processing; this provider is replaceable. */
    vTaskDelay(pdMS_TO_TICKS(250));
    return SUITE_TR_PROVIDER_OK;
}

const suite_translation_provider_t suite_translation_mock_provider = {
    .name = "synthetic-mock",
    .run = mock_run,
};
