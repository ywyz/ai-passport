<p align="right"><a href="README.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Application Fonts

The suite uses a small declared font for fixed UI and a larger public font for
dynamic task text. The generated font maps define actual coverage.

## Dynamic task text

`main/fonts/suite_tasks_font_16.c` is Noto Sans SC Regular 2.004 at 16 px,
2 bits per pixel, uncompressed, generated with `lv_font_conv` 1.5.3 and fast
class kerning. Public requested ranges are `0x20-0x7e,0x2000-0x206f,0x3000-0x303f,0x4e00-0x9fff,0xff00-0xffef`.
The source OTF SHA-256 is
`faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9`;
the copyright notice is Copyright 2014-2021 Adobe (http://www.adobe.com/).
The [OFL license](OFL.txt) accompanies the derived font. No private task text or
character inventory was used. There are 21,393 generated codepoints; 110 requested
codepoints are absent in the source. This does not cover all Unicode. Its measured
standalone RISC-V read-only size is 1,656,220 bytes with zero writable bytes.
`CONFIG_LV_FONT_FMT_TXT_LARGE=y` is required for bitmap indices above 1 MiB.
Placeholders remain enabled, and device rendering remains a separate check.

## Files

- `suite_charset.txt` — the declared character set (digits, punctuation, and
  the Chinese characters used by suite UI text).
- `../main/fonts/suite_font_16.c` — 16 px LVGL font covering every character
  in `suite_charset.txt` (labels, dates, weather lines, lists).
- `../main/fonts/suite_font_clock.c` — 32 px LVGL font with `0123456789:- `
  only (home-page clock digits).

## Source and license

- Typeface: **Noto Sans SC** (Regular), Copyright 2014-2021 Adobe, licensed under the
  [SIL Open Font License 1.1](https://openfontlicense.org/). OFL permits
  bundling (subsetting) inside firmware; the license text must accompany
  distributions of the font software.
- Download used for generation:
  `https://cdn.jsdelivr.net/gh/notofonts/noto-cjk@main/Sans/SubsetOTF/SC/NotoSansSC-Regular.otf`

## Regenerating

Any change to `suite_charset.txt` **must** be followed by regenerating
`main/fonts/suite_font_16.c` (node ≥ 18 with `npx`):

```bash
# Run from assets/fonts/ ; the -o path must climb two levels to reach main/.
npx lv_font_conv@1.5.3 --size 16 --bpp 4 --format lvgl \
  --font NotoSansSC-Regular.otf \
  --symbols "$(sed 's|//.*||' suite_charset.txt | tr -d '[:space:]')" \
  --range 0x20-0x7e \
  --lv-font-name suite_font_16 --no-compress --force-fast-kern-format \
  -o ../../main/fonts/suite_font_16.c
```

`lv_font_conv` takes symbols from the `--symbols` argument; comment lines in
`suite_charset.txt` are stripped first. `--range 0x20` is required
because `--symbols` treats whitespace as a delimiter and silently drops the
space glyph otherwise (it would render as a placeholder box on device). The
current `0x20-0x7e` range also covers printable ASCII in key names and technical
abbreviations. The 2026-10-20 task controls add 52 declared UI codepoints and
completion/retained-request controls add another 50; fixed
labels use this 20-pixel-line-height font, while dynamic task titles/notes use the
public task font with its 31-pixel line height. Verify both bindings and layout.
`--no-compress` stores glyphs uncompressed so
rendering does not depend on LVGL font-decompression support (kept enabled in
`sdkconfig.defaults` via `CONFIG_LV_USE_FONT_COMPRESSED` as a second belt).
The 32 px clock font is regenerated with `--range 0x20`,
`--symbols "0123456789:- "` and `--lv-font-name suite_font_clock`.

## Missing-glyph checklist

The 2026-10-21 voice-task increment adds 16 public UI codepoints. The fixed
16 px font was regenerated with the same pinned Noto source and converter;
dynamic recognized titles use the existing public task font. The R1-R4 voice
error repair adds U+989D and U+5EA6 for the quota message and regenerates the same
font. Device rendering of the new recording/confirmation/error labels remains
an acceptance check.

2026-10-23 OC-11 adds the local translation page without extending the fixed
font: its labels use characters already declared and covered by `suite_font_16`.
The built-in mock's dynamic English/Chinese source and target text are checked
against the existing public dynamic font in
`tests/test_suite_font_charset.py`; future provider text outside that font
remains an explicit LVGL placeholder rather than being dropped. No font
regeneration was required for OC-11. Device rendering of the new page remains an
acceptance check.

Before adding any Chinese UI text (see
`docs/development/engineering/coding-conventions.md#chinese-fonts-and-missing-glyphs`):

1. Add every new character to `suite_charset.txt`.
2. Regenerate the font files.
3. Verify on device that the new text renders (UTF-8 source and a successful
   build do **not** prove glyph coverage).
