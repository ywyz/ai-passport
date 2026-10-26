#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

run_static_checks() {
    local actionlint_bin
    local test_dir

    python3 tools/check_repo.py

    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$(./tools/install-actionlint.sh)"
    fi
    "${actionlint_bin}" -color .github/workflows/*.yml

    test_dir="$(mktemp -d /tmp/ai-passport-host-tests.XXXXXX)"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_ui_pixel_math.c main/ui_pixel_math.c \
        -o "${test_dir}/test_ui_pixel_math"
    "${test_dir}/test_ui_pixel_math"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_demo_navigation.c main/demo_navigation.c \
        -o "${test_dir}/test_demo_navigation"
    "${test_dir}/test_demo_navigation"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 -Imain \
        tests/test_suite_tasks_core.c main/suite_tasks_core.c main/suite_task_ops.c \
        -o "${test_dir}/test_suite_tasks_core"
    "${test_dir}/test_suite_tasks_core"
    for opt in default o2; do
        opt_flags=""
        if [[ "${opt}" == "o2" ]]; then
            opt_flags="-O2"
        fi
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${opt_flags} -Imain \
            tests/test_suite_translation_core.c main/suite_translation_core.c \
            -o "${test_dir}/test_suite_translation_core_${opt}"
        "${test_dir}/test_suite_translation_core_${opt}"
    done
    # Real translation worker/core with real host threads. The provider boundary,
    # task creation and time are external stubs; cancellation, late results and
    # worker cleanup use the changed implementation itself, not a second model.
    for opt in default o2; do
        opt_flags=""
        if [[ "${opt}" == "o2" ]]; then
            opt_flags="-O2"
        fi
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${opt_flags} -pthread \
            -DSUITE_TRANSLATION_TEST -Itests/translation_stubs -Imain \
            -Icomponents/bsp/include \
            tests/test_suite_translation_worker.c main/suite_translation_core.c \
            main/suite_translation_mock.c main/suite_translation_protocol.c \
            tests/translation_stubs/translation_real_stubs.c \
            -o "${test_dir}/test_suite_translation_worker_${opt}"
        "${test_dir}/test_suite_translation_worker_${opt}"
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${opt_flags} -pthread \
            -DSUITE_TRANSLATION_TEST -Itests/translation_stubs -Imain \
            -Icomponents/bsp/include \
            tests/test_suite_translation_real_worker.c main/suite_translation.c \
            main/suite_translation_core.c main/suite_translation_mock.c \
            main/suite_translation_protocol.c \
            tests/translation_stubs/translation_real_stubs.c \
            -o "${test_dir}/test_suite_translation_real_worker_${opt}"
        "${test_dir}/test_suite_translation_real_worker_${opt}"
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${opt_flags} \
            -Itests/tasks_ui_stubs -Itests/translation_stubs -Imain \
            -Icomponents/bsp/include \
            tests/test_suite_translation_ui.c \
            -o "${test_dir}/test_suite_translation_ui_${opt}"
        "${test_dir}/test_suite_translation_ui_${opt}"
    done
    # Shared scratch/config/USB prerequisites (owned by the shared-device
    # session; registered here without changing their sources).
    for opt in default o2; do
        opt_flags=""
        if [[ "${opt}" == "o2" ]]; then
            opt_flags="-O2"
        fi
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${opt_flags} -pthread \
            -DSUITE_AUDIO_JOB_TEST -Itests/tasks_stubs -Itests/weather_stubs \
            -Itests/ble_stubs -Imain tests/test_suite_audio_job.c \
            main/suite_audio_job.c main/suite_tasks_core.c main/suite_task_voice.c \
            main/suite_task_ops.c tests/ble_stubs/cJSON.c -lm \
            -o "${test_dir}/test_suite_audio_job_${opt}"
        "${test_dir}/test_suite_audio_job_${opt}"
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${opt_flags} \
            -Itests/tasks_stubs -Itests/weather_stubs -Imain \
            tests/test_suite_translation_config.c main/suite_translation_config.c \
            main/suite_tasks_core.c main/suite_task_ops.c \
            -o "${test_dir}/test_suite_translation_config_${opt}"
        "${test_dir}/test_suite_translation_config_${opt}"
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${opt_flags} \
            -Itests/tasks_stubs -Itests/weather_stubs -Itests/ble_stubs -Imain \
            tests/test_suite_translation_usb_proto.c main/suite_translation_usb_proto.c \
            main/suite_translation_config.c main/suite_tasks_core.c main/suite_task_ops.c \
            tests/ble_stubs/cJSON.c -lm \
            -o "${test_dir}/test_suite_translation_usb_proto_${opt}"
        "${test_dir}/test_suite_translation_usb_proto_${opt}"
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${opt_flags} \
            -Itests/translation_usb_stubs -Itests/tasks_stubs -Itests/weather_stubs -Imain \
            tests/test_suite_translation_usb_window.c main/suite_translation_usb.c \
            -o "${test_dir}/test_suite_translation_usb_window_${opt}"
        "${test_dir}/test_suite_translation_usb_window_${opt}"
    done
    for opt in default o2; do
        opt_flags=""
        if [[ "${opt}" == "o2" ]]; then
            opt_flags="-O2"
        fi
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${opt_flags} -Imain \
            tests/test_suite_translation_protocol.c main/suite_translation_protocol.c \
            -o "${test_dir}/test_suite_translation_protocol_${opt}"
        "${test_dir}/test_suite_translation_protocol_${opt}"
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${opt_flags} \
            -Itests/translation_http_stubs -Imain \
            -include tests/translation_http_stubs/suite_transport_test_io.h \
            tests/test_suite_translation_http.c main/suite_translation_http.c \
            main/suite_translation_protocol.c \
            -o "${test_dir}/test_suite_translation_http_${opt}"
        "${test_dir}/test_suite_translation_http_${opt}"
    done
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_translation_device_config.py
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 -Imain \
        tests/test_suite_task_ops.c main/suite_task_ops.c main/suite_tasks_core.c \
        -o "${test_dir}/test_suite_task_ops"
    "${test_dir}/test_suite_task_ops"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 -Imain -Itests/ble_stubs \
        tests/test_suite_task_voice.c main/suite_task_voice.c main/suite_tasks_core.c main/suite_task_ops.c \
        tests/ble_stubs/cJSON.c -lm -o "${test_dir}/test_suite_task_voice"
    "${test_dir}/test_suite_task_voice"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 \
        -Itests/tasks_ui_stubs -Itests/weather_stubs -Imain \
        tests/test_suite_tasks_ui.c main/suite_tasks_nav.c main/suite_clock.c \
        -o "${test_dir}/test_suite_tasks_ui"
    "${test_dir}/test_suite_tasks_ui"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 \
        -ffunction-sections -fdata-sections -Itests/tasks_stubs -Itests/weather_stubs -Imain -Itests/ble_stubs \
        tests/test_suite_tasks_runtime.c main/suite_tasks_core.c main/suite_task_ops.c main/suite_clock.c main/suite_task_voice.c tests/ble_stubs/cJSON.c -lm \
        -Wl,--gc-sections -o "${test_dir}/test_suite_tasks_runtime"
    "${test_dir}/test_suite_tasks_runtime"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 \
        -ffunction-sections -fdata-sections -Itests/tasks_stubs -Itests/weather_stubs -Imain \
        tests/test_suite_storage_preservation.c -Wl,--gc-sections \
        -o "${test_dir}/test_suite_storage_preservation"
    "${test_dir}/test_suite_storage_preservation"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_display_rounding.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_display_rounding"
    "${test_dir}/test_bsp_display_rounding"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_es8311_sleep_check.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_es8311_sleep_check"
    "${test_dir}/test_bsp_es8311_sleep_check"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_button.c -o "${test_dir}/test_bsp_button"
    "${test_dir}/test_bsp_button"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_lvgl_init.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_lvgl_init"
    "${test_dir}/test_bsp_lvgl_init"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/audio_stubs -Icomponents/bsp/include -Icomponents/bsp/src \
        tests/test_bsp_audio_recovery.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_audio_recovery"
    "${test_dir}/test_bsp_audio_recovery"
    for demo in audio low_power ble wifi; do
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
            -ffunction-sections -fdata-sections -Itests/demo_stubs -Imain \
            "tests/test_demo_${demo}_runtime.c" -Wl,--gc-sections \
            -o "${test_dir}/test_demo_${demo}_runtime"
        "${test_dir}/test_demo_${demo}_runtime"
    done
    for suite in input clock conn_fsm weather_logic scene city_data prov_proto \
                 provision_qr; do
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
            "tests/test_suite_${suite}.c" main/suite_"${suite}".c -lm \
            -o "${test_dir}/test_suite_${suite}"
        "${test_dir}/test_suite_${suite}"
    done
    # 三体固定步积分/扫掠碰撞在优化构建中也验证,包含高速擦碰和长期运行。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 -Imain \
        tests/test_suite_scene.c main/suite_scene.c -lm \
        -o "${test_dir}/test_suite_scene_o2"
    "${test_dir}/test_suite_scene_o2"
    # 天气运行时测试用真实线程(-pthread);默认与 -O2 都必须通过,
    # 防止回到跨失效栈帧的 longjmp 类未定义行为(只在 -O2 暴露)。
    # 同轮编译天气任务桩的调用线程身份回归:主线程的有界超时取用不得被
    # 误判为任务停靠。
    for opt in default o2; do
        opt_flags=""
        if [[ "${opt}" == "o2" ]]; then
            opt_flags="-O2"
        fi
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${opt_flags} -pthread \
            -Itests/weather_stubs -Imain \
            tests/test_suite_weather_runtime.c \
            main/suite_weather_logic.c main/suite_clock.c -lm \
            -o "${test_dir}/test_suite_weather_runtime_${opt}"
        "${test_dir}/test_suite_weather_runtime_${opt}"
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${opt_flags} -pthread \
            -Itests/weather_stubs \
            tests/test_suite_weather_sem_context.c \
            -o "${test_dir}/test_suite_weather_sem_context_${opt}"
        "${test_dir}/test_suite_weather_sem_context_${opt}"
    done
    # 连接任务与配网 BLE 运行时:线程化 RTOS 桩 + 网络栈逐步故障注入、
    # 独立 host 线程与提交闸门交错;默认与 -O2 都必须通过(锁语义与
    # 顺序断言不依赖优化级别)。
    for opt in default o2; do
        opt_flags=""
        if [[ "${opt}" == "o2" ]]; then
            opt_flags="-O2"
        fi
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${opt_flags} -pthread \
            -Itests/conn_stubs -Imain \
            tests/test_suite_connection_runtime.c \
            main/suite_conn_fsm.c main/suite_clock.c -lm \
            -o "${test_dir}/test_suite_connection_runtime_${opt}"
        "${test_dir}/test_suite_connection_runtime_${opt}"
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${opt_flags} -pthread \
            -Itests/ble_stubs -Imain \
            tests/test_suite_prov_ble_runtime.c \
            main/suite_prov_proto.c main/suite_clock.c -lm \
            -o "${test_dir}/test_suite_prov_ble_runtime_${opt}"
        "${test_dir}/test_suite_prov_ble_runtime_${opt}"
    done
    # 配网页契约:用 Node 运行 provisioning/web/app.js 的真实处理逻辑,
    # 覆盖扫描应答渲染、选择/提交凭据、空列表/错误呈现与省市应答。
    node_bin="${NODE_BIN:-$(command -v node || command -v node.exe || true)}"
    if [[ -z "${node_bin}" ]]; then
        echo "ERROR: node is required for the provisioning web contract tests." >&2
        exit 1
    fi
    "${node_bin}" tests/test_prov_web_contract.mjs
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_deep_sleep_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_check_repo.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_suite_font_charset.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_task_device_config.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_verify_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_archive_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_install_passport_skills.py
    bash tools/test-companion.sh
    rm -rf "${test_dir}"
    echo "Host tests: PASS"
}

run_firmware_checks() (
    local validation_build_dir

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    validation_build_dir="$(mktemp -d /tmp/ai-passport-firmware.XXXXXX)"
    trap 'case "${validation_build_dir}" in /tmp/ai-passport-firmware.*) rm -rf -- "${validation_build_dir}" ;; esac' EXIT

    SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${validation_build_dir}" \
        -D "SDKCONFIG=${validation_build_dir}/sdkconfig" build
    idf.py -B "${validation_build_dir}" merge-bin \
        -o "${validation_build_dir}/FoloToy-AI-Passport-full.bin"
    python3 tools/verify_firmware.py "${validation_build_dir}"
    PYTHONDONTWRITEBYTECODE=1 python3 tools/archive_firmware.py create \
        "${validation_build_dir}" --archive-root "${repo_root}/build/firmware"
    mkdir -p "${repo_root}/build"
    install -m 0644 \
        "${validation_build_dir}/FoloToy-AI-Passport-full.bin" \
        "${repo_root}/build/FoloToy-AI-Passport-full.bin"
    echo "Firmware build: PASS"
)

cd "${repo_root}"
case "${mode}" in
    --all)
        run_static_checks
        run_firmware_checks
        ;;
    --static)
        run_static_checks
        ;;
    --firmware)
        run_firmware_checks
        ;;
    *)
        usage
        exit 2
        ;;
esac
