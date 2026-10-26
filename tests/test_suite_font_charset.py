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
    sources = list(main.glob("suite_*.c")) + list(main.glob("suite_*.h"))
    # Translation mock output is dynamic provider text, checked against the
    # public dynamic font by test_translation_mock_dynamic_text_covered. It is
    # not a fixed static label and must not force glyphs into the small font.
    return sorted(path for path in sources
                  if path.name != "suite_translation_mock.c")


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
    def font_coverage(self, path: str) -> set[int]:
        font = (ROOT / path).read_text(encoding="utf-8")
        arrays = {}
        for match in re.finditer(r'static const uint(?:8|16)_t (\w+)\[\] = \{(.*?)\};', font, re.S):
            if not match[1].startswith(("unicode_list", "glyph_id_ofs_list")):
                continue
            body = re.sub(r'/\*.*?\*/', '', match[2], flags=re.S)
            arrays[match[1]] = [int(v.strip(), 0) for v in body.split(',') if v.strip()]
        maps = re.search(r'cmaps\[\]\s*=\s*\{(.*?)\n\};', font, re.S)[1]
        covered = set()
        for match in re.finditer(r'\{([^{}]+)\}', maps):
            props = dict(re.findall(r'\.(\w+)\s*=\s*(\w+)', match[1]))
            start, length = int(props['range_start']), int(props['range_length'])
            if props['type'] == 'LV_FONT_FMT_TXT_CMAP_SPARSE_TINY':
                covered.update(start + n for n in arrays[props['unicode_list']])
            elif props['type'] == 'LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY':
                covered.update(range(start, start + length))
            elif props['type'] == 'LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL':
                covered.update(start + i for i, n in enumerate(arrays[props['glyph_id_ofs_list']]) if n or not i)
            else:
                self.fail("Unreviewed task font cmap")
        return covered

    def test_dynamic_task_font_public_maps(self) -> None:
        covered = self.font_coverage("main/fonts/suite_tasks_font_16.c")
        self.assertEqual(len(covered), 21393)
        self.assertTrue(set(range(32, 127)) <= covered)
        self.assertTrue({ord(c) for c in "中文待办备注长按返回"} <= covered)
        self.assertNotIn(0x1F600, covered)
        self.assertIn("CONFIG_LV_FONT_FMT_TXT_LARGE=y", (ROOT / "sdkconfig.defaults").read_text())

    def test_static_font_covers_declared_ui_and_printable_ascii(self) -> None:
        covered = self.font_coverage("main/fonts/suite_font_16.c")
        self.assertTrue({ord(c) for c in declared_charset() if not c.isspace()} <= covered)
        self.assertTrue(set(range(32, 127)) <= covered)
        self.assertTrue({ord(c) for c in "刷新待办上下选择取消下载离线缓存"} <= covered)
        self.assertNotIn(0x1F600, covered)
        font = (ROOT / "main/fonts/suite_font_16.c").read_text(encoding="utf-8")
        height = int(re.search(r'\.line_height\s*=\s*(\d+)', font)[1])
        self.assertLessEqual(height, 20, "Task toolbar/receipt/footer layout budget")

    def test_translation_mock_dynamic_text_covered(self) -> None:
        """Synthetic source/target text must exist in the public dynamic font.

        Arbitrary future provider text is not bounded by this set; missing
        glyphs stay visible through LVGL's enabled placeholder rather than
        being silently dropped. This check keeps the built-in mock honest.
        """
        covered = self.font_coverage("main/fonts/suite_tasks_font_16.c")
        source = (ROOT / "main" / "suite_translation_mock.c").read_text(
            encoding="utf-8")
        codepoints: set[int] = set()
        for line in source.splitlines():
            for match in STRING_LITERAL.finditer(code_part(line)):
                codepoints.update(ord(c) for c in match.group(1) if ord(c) >= 0x80)
        self.assertTrue(codepoints, "translation mock text not found")
        self.assertTrue(codepoints <= covered,
                        "translation mock text exceeds the dynamic font: "
                        + ", ".join(f"U+{cp:04X}" for cp in sorted(codepoints - covered)))
        self.assertNotIn(0x1F600, covered)

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
