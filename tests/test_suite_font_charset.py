#!/usr/bin/env python3
"""Host test: suite UI text must stay inside the declared font charset.

Every Chinese character rendered by suite screens must be declared in
assets/fonts/suite_charset.txt; after any charset change the LVGL font
main/fonts/suite_font_16.c must be regenerated (see assets/fonts/README.md).
A missing declaration renders as a placeholder box on device even though the
source is valid UTF-8 and the build succeeds. ESP_LOG strings are excluded:
they are serial-only and never rendered.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

STRING_LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def code_part(line: str) -> str:
    """Drop // comments, honoring string literals so URLs survive."""
    out: list[str] = []
    in_string = False
    i = 0
    while i < len(line):
        ch = line[i]
        if in_string:
            if ch == "\\" and i + 1 < len(line):
                out.append(line[i:i + 2])
                i += 2
                continue
            if ch == '"':
                in_string = False
            out.append(ch)
        elif ch == '"':
            in_string = True
            out.append(ch)
        elif ch == "/" and line[i:i + 2] == "//":
            break
        else:
            out.append(ch)
        i += 1
    return "".join(out)


def rendered_sources() -> list[Path]:
    main = ROOT / "main"
    return sorted(list(main.glob("suite_*.c")) + list(main.glob("suite_*.h")))


def rendered_cjk_chars() -> set[str]:
    chars: set[str] = set()
    for path in rendered_sources():
        for line in path.read_text(encoding="utf-8").splitlines():
            if "ESP_LOG" in line:
                continue  # serial log text, never rendered on screen
            for match in STRING_LITERAL.finditer(code_part(line)):
                chars.update(c for c in match.group(1) if ord(c) >= 0x80)
    return chars


def declared_charset() -> set[str]:
    text = (ROOT / "assets" / "fonts" / "suite_charset.txt").read_text(
        encoding="utf-8")
    return set(text)


class SuiteFontCharsetTest(unittest.TestCase):
    def test_rendered_ui_chars_are_declared(self) -> None:
        sources = rendered_sources()
        self.assertTrue(sources, "suite sources not found under main/")
        missing = sorted(rendered_cjk_chars() - declared_charset())
        self.assertEqual(
            missing, [],
            "UI characters missing from assets/fonts/suite_charset.txt; "
            "add them and regenerate main/fonts/suite_font_16.c "
            "(see assets/fonts/README.md): "
            + ", ".join(f"U+{ord(c):04X}({c})" for c in missing),
        )

    def test_generated_font_matches_contract(self) -> None:
        # Contract: glyphs stored uncompressed and the ASCII space glyph is
        # included via --range 0x20 (--symbols alone would drop it).
        font = (ROOT / "main" / "fonts" / "suite_font_16.c").read_text(
            encoding="utf-8", errors="ignore")
        self.assertNotIn("compressed", font.lower())
        self.assertIn("0x20", font)


if __name__ == "__main__":
    unittest.main()
