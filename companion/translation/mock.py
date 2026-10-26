"""Synthetic provider with no network, credential or persistence dependency."""

from .common import validate_text


class SyntheticProvider:
    """Deterministic in-memory ASR/MT used by the local T1 job path."""

    def __init__(self):
        self.asr_calls = 0
        self.mt_calls = 0
        self.asr_hook = None
        self.mt_hook = None

    def transcribe(self, raw, source_lang):
        if type(raw) is not bytes or not raw:
            raise ValueError("synthetic audio is required")
        self.asr_calls += 1
        if self.asr_hook is not None:
            self.asr_hook()
        if source_lang == "zh-CN":
            return validate_text("合成中文原文。")
        return validate_text("Synthetic English source.")

    def translate(self, text, source_lang, target_lang):
        validate_text(text)
        self.mt_calls += 1
        if self.mt_hook is not None:
            self.mt_hook()
        if target_lang == "zh-CN":
            return validate_text("合成中文译文。")
        return validate_text("Synthetic English translation.")
