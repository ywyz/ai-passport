"""Production WSGI factory for the isolated translation service."""

import atexit
import os
import time

from .config import load_api_key, read_config
from .provider import MiMoProvider
from .service import TranslationAPI, TranslationJobs
from .store import TranslationStore


def build(config_path):
    config = read_config(config_path)
    key = load_api_key(config)
    store = TranslationStore(config["database"], clock=time.time,
                             capacity=config["capacity"])
    provider = MiMoProvider(key, asr_timeout=config["asr_timeout"],
                            mt_timeout=config["mt_timeout"])
    jobs = TranslationJobs(
        store, provider, clock=time.time, text_ttl=config["text_ttl"],
        daily_asr_seconds=config["daily_asr_seconds"],
        daily_mt_jobs=config["daily_mt_jobs"],
        audio_ttl=config["audio_ttl"])
    try:
        jobs.start_worker(config["queue_size"])
        jobs.start_expiry(config["cleanup_interval"])
        api = TranslationAPI(jobs, config["translation_token"],
                             config["forbidden_tokens"])
    except Exception:
        jobs.stop()
        raise
    atexit.register(jobs.stop)
    return api


def create_app():
    try:
        return build(os.environ["SUITE_TRANSLATION_CONFIG"])
    except Exception:
        raise RuntimeError(
            "Private translation-service configuration or storage is "
            "unavailable") from None
