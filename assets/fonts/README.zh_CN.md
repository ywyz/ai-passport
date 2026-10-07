<p align="right"><strong>简体中文</strong> · <a href="README.md">English</a></p>

# 应用字体(子集)

AI Passport 五合一应用使用**字集子集字体**渲染中文 UI,无需把完整 CJK 字库
装进固件。本目录即契约:这里声明的字符,就是生成字体能显示的全部字符。

## 文件

- `suite_charset.txt` —— 字集声明(数字、标点与套件 UI 用到的全部汉字)。
- `../main/fonts/suite_font_16.c` —— 16 px LVGL 字体,覆盖
  `suite_charset.txt` 全部字符(标签/日期/天气行/列表)。
- `../main/fonts/suite_font_clock.c` —— 32 px LVGL 字体,仅含
  `0123456789:- `(首页时钟数字)。

## 来源与许可

- 字形:**Noto Sans SC**(Regular),© Google,采用
  [SIL Open Font License 1.1](https://openfontlicense.org/) 授权。OFL 允许
  子集化后随固件分发;再分发字体软件时须附带许可证文本。
- 生成所用下载地址:
  `https://cdn.jsdelivr.net/gh/notofonts/noto-cjk@main/Sans/SubsetOTF/SC/NotoSansSC-Regular.otf`

## 重新生成

修改 `suite_charset.txt` 后**必须**重新生成
`main/fonts/suite_font_16.c`(需 node ≥ 18,可用 `npx`):

```bash
npx lv_font_conv --size 16 --bpp 4 --format lvgl \
  --font NotoSansSC-Regular.otf \
  --symbols "$(sed 's|//.*||' suite_charset.txt | tr -d '[:space:]')" \
  --lv-font-name suite_font_16 --no-compress --force-fast-kern-format \
  -o ../main/fonts/suite_font_16.c
```

`lv_font_conv` 通过 `--symbols` 接收字符;先剔除 `suite_charset.txt` 的注释行,
第 3 行的空格与 ASCII 标点必须保留。`--no-compress` 表示字模不压缩存储,
渲染不依赖 LVGL 字体解压能力(`sdkconfig.defaults` 中仍保留
`CONFIG_LV_USE_FONT_COMPRESSED` 双保险)。32 px 时钟字体用相同命令生成,
参数改为 `--symbols "0123456789:- "` 与 `--lv-font-name suite_font_clock`。

## 缺字检查清单

新增中文 UI 文案前(参见
`docs/development/engineering/coding-conventions.md#chinese-fonts-and-missing-glyphs`):

1. 把所有新字符加入 `suite_charset.txt`。
2. 重新生成字体文件。
3. 在设备上确认新文案正常显示(源码 UTF-8 与构建成功**不能**证明字形覆盖)。
