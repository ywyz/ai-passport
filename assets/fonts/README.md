<p align="right"><a href="README.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Application Fonts (subset)

The AI Passport Suite renders Chinese UI text with a **glyph-subset** font so a
full CJK font never has to fit in firmware. This directory is the contract:
whatever characters appear here are exactly what the generated font can draw.

## Files

- `suite_charset.txt` — the declared character set (digits, punctuation, and
  the Chinese characters used by suite UI text).
- `../main/fonts/suite_font_16.c` — 16 px LVGL font covering every character
  in `suite_charset.txt` (labels, dates, weather lines, lists).
- `../main/fonts/suite_font_clock.c` — 32 px LVGL font with `0123456789:- `
  only (home-page clock digits).

## Source and license

- Typeface: **Noto Sans SC** (Regular), © Google, licensed under the
  [SIL Open Font License 1.1](https://openfontlicense.org/). OFL permits
  bundling (subsetting) inside firmware; the license text must accompany
  distributions of the font software.
- Download used for generation:
  `https://cdn.jsdelivr.net/gh/notofonts/noto-cjk@main/Sans/SubsetOTF/SC/NotoSansSC-Regular.otf`

## Regenerating

Any change to `suite_charset.txt` **must** be followed by regenerating
`main/fonts/suite_font_16.c` (node ≥ 18 with `npx`):

```bash
npx lv_font_conv --size 16 --bpp 4 --format lvgl \
  --font NotoSansSC-Regular.otf \
  --symbols "$(sed 's|//.*||' suite_charset.txt | tr -d '[:space:]')" \
  --lv-font-name suite_font_16 --no-compress --force-fast-kern-format \
  -o ../main/fonts/suite_font_16.c
```

`lv_font_conv` takes symbols from the `--symbols` argument; comment lines in
`suite_charset.txt` are stripped first, and the space plus ASCII punctuation
on line 3 must be preserved. `--no-compress` stores glyphs uncompressed so
rendering does not depend on LVGL font-decompression support (kept enabled in
`sdkconfig.defaults` via `CONFIG_LV_USE_FONT_COMPRESSED` as a second belt).
The 32 px clock font is regenerated the same way with
`--symbols "0123456789:- "` and `--lv-font-name suite_font_clock`.

## Missing-glyph checklist

Before adding any Chinese UI text (see
`docs/development/engineering/coding-conventions.md#chinese-fonts-and-missing-glyphs`):

1. Add every new character to `suite_charset.txt`.
2. Regenerate the font files.
3. Verify on device that the new text renders (UTF-8 source and a successful
   build do **not** prove glyph coverage).
