#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

# Pure-Python dependencies are isolated from ESP-IDF and system packages.
# A content-addressed cache avoids downloading on every static-gate run.
lock_hash="$(cat companion/requirements.lock companion/requirements-server.lock | sha256sum | cut -d ' ' -f 1)"
python_version="$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"
cache_root="${repo_root}/build/tooling/companion/${python_version}"
dep_dir="${cache_root}/${lock_hash}"
if [[ ! -f "${dep_dir}/.installed" ]]; then
    mkdir -p "${cache_root}"
    staging="$(mktemp -d "${cache_root}/install.XXXXXX")"
    trap 'rm -rf -- "${staging}"' EXIT
    python3 -m pip install --disable-pip-version-check --no-compile \
        --require-hashes --only-binary=:all: --target "${staging}" \
        -r companion/requirements-server.lock
    touch "${staging}/.installed"
    # Do not replace a cache another validation process has just completed.
    if [[ ! -d "${dep_dir}" ]]; then
        mv -- "${staging}" "${dep_dir}"
    fi
fi
PYTHONDONTWRITEBYTECODE=1 PYTHONPATH="${dep_dir}" python3 tests/test_companion_tasks.py
PYTHONDONTWRITEBYTECODE=1 PYTHONPATH="${dep_dir}" python3 tests/test_companion_completions.py
PYTHONDONTWRITEBYTECODE=1 PYTHONPATH="${dep_dir}" python3 tests/test_companion_voice.py
PYTHONDONTWRITEBYTECODE=1 PYTHONPATH="${dep_dir}" python3 tests/test_companion_translation.py
PYTHONDONTWRITEBYTECODE=1 PYTHONPATH="${dep_dir}" python3 tests/test_companion_translation_real.py
