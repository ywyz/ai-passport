<p align="right"><strong>简体中文</strong> · <a href="README.md">English</a></p>

# 应用字体

套件使用小型声明字集渲染固定 UI，使用较大的公开字库渲染动态待办文本。
生成字体的映射表决定实际覆盖范围。

## 动态待办文本

`main/fonts/suite_tasks_font_16.c` 来自 Noto Sans SC Regular 2.004，16 px、2 位
灰度、无压缩，由 `lv_font_conv` 1.5.3 使用快速类字距生成。公开请求范围为
`0x20-0x7e,0x2000-0x206f,0x3000-0x303f,0x4e00-0x9fff,0xff00-0xffef`。
原 OTF 的 SHA-256 为
`faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9`，
版权信息为 Copyright 2014-2021 Adobe (http://www.adobe.com/)。
衍生字体附有 [OFL 许可证](OFL.txt)。没有使用私人待办文本或字符清单。
实际生成 21,393 个码位，源字体缺少请求范围中的 110 个码位，不覆盖全部 Unicode。
独立 RISC-V 对象测得只读区 1,656,220 字节，可写区为零。位图索引超过 1 MiB，必须
启用 `CONFIG_LV_FONT_FMT_TXT_LARGE=y`。保留缺字占位，设备渲染仍需单独验收。

## 文件

- `suite_charset.txt` —— 字集声明(数字、标点与套件 UI 用到的全部汉字)。
- `../main/fonts/suite_font_16.c` —— 16 px LVGL 字体,覆盖
  `suite_charset.txt` 全部字符(标签/日期/天气行/列表)。
- `../main/fonts/suite_font_clock.c` —— 32 px LVGL 字体,仅含
  `0123456789:- `(首页时钟数字)。

## 来源与许可

- 字形:**Noto Sans SC**(Regular)，Copyright 2014-2021 Adobe，采用
  [SIL Open Font License 1.1](https://openfontlicense.org/) 授权。OFL 允许
  子集化后随固件分发;再分发字体软件时须附带许可证文本。
- 生成所用下载地址:
  `https://cdn.jsdelivr.net/gh/notofonts/noto-cjk@main/Sans/SubsetOTF/SC/NotoSansSC-Regular.otf`

## 重新生成

修改 `suite_charset.txt` 后**必须**重新生成
`main/fonts/suite_font_16.c`(需 node ≥ 18,可用 `npx`):

```bash
# 在 assets/fonts/ 目录下运行;-o 需上跳两级才能落到 main/。
npx lv_font_conv@1.5.3 --size 16 --bpp 4 --format lvgl \
  --font NotoSansSC-Regular.otf \
  --symbols "$(sed 's|//.*||' suite_charset.txt | tr -d '[:space:]')" \
  --range 0x20-0x7e \
  --lv-font-name suite_font_16 --no-compress --force-fast-kern-format \
  -o ../../main/fonts/suite_font_16.c
```

`lv_font_conv` 通过 `--symbols` 接收字符;先剔除 `suite_charset.txt` 的注释行。
`--range 0x20` 必须保留:`--symbols` 把空白当分隔符,空格字形会被悄悄
丢弃(设备上渲染成占位方块)。当前 `0x20-0x7e` 还覆盖按键名及技术缩写所需的
全部可打印 ASCII。2026-10-20 待办控件新增 52 个声明字形，完成/保留请求控件再增加
50 个；固定标签使用本字体
（行高 20 像素），动态任务标题/备注使用公共待办字体（行高 31 像素），需分别
核验绑定及布局。`--no-compress` 表示字模不压缩存储,
渲染不依赖 LVGL 字体解压能力(`sdkconfig.defaults` 中仍保留
`CONFIG_LV_USE_FONT_COMPRESSED` 双保险)。32 px 时钟字体使用 `--range 0x20`、
`--symbols "0123456789:- "` 与 `--lv-font-name suite_font_clock` 生成。

## 缺字检查清单

2026-10-21 语音待办增量新增 16 个公共 UI 码位，使用同一固定版本 Noto 来源和转换器
重新生成 16 px 固定字体；动态识别标题使用已有公共待办字体。R1–R4 语音错误修复
新增“额”“度”用于额度文案，并重新生成同一字体。新增录音/确认/错误文案仍需实屏验收。

2026-10-23 OC-11 新增本地翻译页但未扩展固定字体：其标签只使用
`suite_font_16` 已声明并覆盖的字符。内置 mock 的动态英文/中文原文与译文在
`tests/test_suite_font_charset.py` 中按现有公共动态字体验证；未来供应商文字
超出该字体时仍显示 LVGL 显式占位符，不会静默丢弃。OC-11 无需重新生成字体。
新页面的设备渲染仍需实屏验收。

新增中文 UI 文案前(参见
`docs/development/engineering/coding-conventions.md#chinese-fonts-and-missing-glyphs`):

1. 把所有新字符加入 `suite_charset.txt`。
2. 重新生成字体文件。
3. 在设备上确认新文案正常显示(源码 UTF-8 与构建成功**不能**证明字形覆盖)。
