[English](passport-translation-plan.md) · **简体中文**

# 翻译 T1 获准设计与当前证据

更新于 2026-10-25，Asia/Shanghai。T1 已实现并部署：固件采集/上传/轮询、MiMo 语音识别
与文本翻译、独立伴随服务、加密配置及仅翻译用 USB 配置下发。一次获准的合成真实供应商
EN->zh-CN 冒烟通过，历史上真机翻译页也可打开。OC-11 最后一次真机尝试在采集前停于服务
就绪检查；设备当前是只有占位页的旧构建，已无法走到该路径。T2/TTS 尚未实现；本文不等于
完整功能验收，SA-15 仍不完整。需求继续对应[五合一 PRD](passport-suite-prd.zh_CN.md)和
[契约](passport-suite-contract.zh_CN.md)中的 SU-10/11、SC-08、ST-04、SA-14/15/16。

## 已确认决策

| 决策 | 已记录结果 |
| --- | --- |
| 供应商 | MiMo `mimo-v2.5-asr` 显式识别 `zh`/`en`，`mimo-v2.6-flash` 负责文本，均调用 `POST https://api.xiaomimimo.com/v1/chat/completions`；识别非流式，文本输出 JSON-object，关闭 thinking 并限制补全长度。 |
| 已拒绝方案 | DeepSeek 不是生产翻译供应商；`deepseek-flash` 别名没有 ASR。OpenCode Go DeepSeek V4.1 Flash 仍是 OpenCode 开发工具。 |
| 采集例外 | 批准在自有非活动 Flash 槽临时保存明文 WAV（最长 10 秒），随后单次有界上传并清理，替代仅易失内存采集。 |
| 保留策略 | 用户接受已披露的无固定保留 TTL 及未确认的音频训练处理。不声称固定 TTL、零保留设置或账户区域保证。 |
| 真实调用 | 真实供应商、设备和付费测试在集成复核后获准，并仅在该授权范围内使用。 |
| T2 | 合成与播放尚未实现，本文不批准。 |

供应商接口已于 2026-10-22 复核：[ASR API](https://mimo.mi.com/static/docs/api/audio/Speech-Recognition.md)、
[文本 API](https://mimo.mi.com/static/docs/api/chat/openai-api.md) 及
[TTS API](https://mimo.mi.com/static/docs/api/audio/tts.md)。
[MiMo 平台隐私政策](https://privacy.mi.com/XiaomiMiMoPlatform/zh_CN/)未给出固定 API TTL。
用户已接受选定方案已披露的不确定性；实际账户地区、设置及音频训练处理仍未核实。除该已记录
接受外，不声称固定 TTL、零保留设置或隐私保证。支持 SSE 不代表供应商保证取消或退款；关闭
连接并丢弃迟到结果仍可能计费。此边界同时适用于 T1 ASR 与 T2 TTS。

## 复用与隔离

复用 BSP PCM 访问、可信时间/TLS 门禁、`suite_net`、严格解析、页面生命周期及现有
公开动态字体。任务创建/完成令牌、`taskops`/`taskvoice`、CalDAV 写入、回执和活动
缓存代均不进入翻译。现有两类任务操作共用受限写令牌；翻译使用独立能力。

独立翻译能力使用加密配置中的独立键，安全提交并回读，保留所有既有命名空间；仅新增
键不等于格式迁移。供应商密钥留在服务端；缺少翻译配置不阻断待办、天气或配网。

共用非活动槽音频以 `suite_audio_job` 作为明确 scratch 所有者；不复制待办 worker，
也不调整分区。待办语音、任务下载和翻译共同遵循下述获取顺序。

## 有界 T1 协议（已批准）

两项 SC-08 采集例外已批准：用设备临时 Flash 音频替代仅易失存储，以及用单次 WAV PUT
替代分块 PUT/finish。TTS 成功取回后的额外保留仍属 T2 问题，尚未批准。

1. 预检 ONLINE、可信时间和服务就绪。采集最多 10 秒、最少 0.3 秒的 16 kHz、
   有符号 16 位小端单声道 PCM。
2. 在自有非活动槽完成 canonical WAV 并计算完整文件 SHA-256，再 POST。不把整段
   音频放入 RAM。
3. POST `/suite/v1/translations`，携带 `schema`、`operation_id`、显式语言对、
   `format`、`audio_sha256`、`audio_bytes`；持久准入成功后才返回 202。
4. PUT `/{job}/audio`，最多 320,044 字节。完整长度、WAV 和哈希校验后才允许
   provider claim。重复上传同一内容不得重复 ASR。
5. GET `/{job}` 轮询状态/结果，DELETE 幂等取消。回包不明时不自动创建新的付费操作。
   T2 增加 `/{job}/speech` 和规范化播放音频的受认证 GET。

全部路由要求独立 Bearer 翻译令牌。任务 reader/writer/admin 令牌不能访问翻译，
翻译令牌不能访问任务路由。令牌不进 URL 或日志。响应身份必须匹配作业、操作和设备
generation。Content-Length 和文字计数均从实际字节计算。

```json
{"schema":1,"operation_id":"0123456789abcdef0123456789abcdef","source_lang":"en","target_lang":"zh-CN","format":"wav","audio_sha256":"<computed SHA-256>","audio_bytes":320044}
```

```json
{"schema":1,"job_id":"0123456789abcdef0123456789abcdef","state":"DONE","source_lang":"en","target_lang":"zh-CN","source_text":"<recognized source>","target_text":"<translated text>"}
```

示例只说明字段，哈希和音频长度必须对应实际录音。每段文字限制为 2,048 UTF-8 字节
且最多 512 个 Unicode scalar，分别校验，不截断或用摘要代替翻译。原文与译文分开
标注并滚动。翻译或合成失败时保留有效原文；缺字必须明确显示。

## 重启、付费和保留

使用独立有界元数据账本，不进入任务日志。POST 时持久化操作 ID 和载荷指纹；每次
ASR、MT、TTS 外部调用前原子 claim 并提交对应阶段。每阶段只能 claim 一次。
TTS 指纹还绑定文字哈希、模型、音色及输出格式。

| 事件 | 必须行为 |
| --- | --- |
| POST 丢回包，存在匹配持久行 | 返回该准入/状态，不创建第二个操作。 |
| POST 丢回包，不存在行 | 上次事务从未准许 provider；可以安全完成真正首次准入。 |
| 相同 ID，不同载荷 | 冲突；不覆盖，不调用 provider。 |
| provider claim 后重启 | UNKNOWN；不自动重放付费阶段。调用前崩溃也可能是 UNKNOWN。 |
| DONE 后重启，文字丢失 | TEXT_LOST/结果过期；元数据不是可恢复译文。 |
| speech 丢回包 | 复用同一 speech ID/状态，不自动用新 ID 合成。 |
| 本地仍有 PCM 的重播 | 读取本地音频，不新增 provider 调用。 |
| 元数据容量满 | BUSY 拒绝新准入，不静默淘汰去重证据。 |

终态行可以压缩为 tombstone，但不能在 24 小时后自动删除唯一去重证据。保证只在行
或 tombstone 存续期间有效；显式管理清理结束该保证。用户明确发起新操作可使用新 ID，
并可能产生新的费用。

已实现生命周期：RAM 作业/文字 120 秒，服务端排队音频最多 300 秒，过期后为
`EXPIRED`/`AUDIO_EXPIRED` 并在重启时清理。ASR 后删除上传音频，取消后丢弃 provider
结果。文字/音频不进入 SQLite、请求日志或备份；元数据只含标识、哈希、状态和时间。
Python 文字可释放引用，但不能承诺不可变字符串被安全清零；供应商保留仍是另外的
未决边界。

## 所有权、资源上限与交互

S 是 scratch 外层互斥锁。audio A 与 network N 是 S 内互斥的两个阶段，不同时
持有 A 和 N，也不在持有 N 时获取 S/A。

| 阶段 | 所有权 |
| --- | --- |
| 采集 | S + A；TLS 前释放 A。 |
| 上传/下载 | S + N；请求有界；播放前释放 N。 |
| 播放（T2） | S + A。 |
| 清理 | 仅 S；先擦数据，再移除持久标记。 |
| 不需 scratch 的天气/完成 | 仅 N；不等待 S/A。 |

既有待办语音/下载也必须遵守相同顺序。页面退出停 timer、失效 generation 并发出
取消信号，不等网络。worker 拥有清理责任，不持 LVGL 指针。收到 worker 确认后
才能复用 scratch。清理失败时在 S 内标记槽 quarantined，释放 S 并拒绝后续使用，
不得让退出 worker 永久持锁。启动清理只处理有标记的非活动槽。任务缓存提交也需 S，
保证 scratch 所有者运行期间活动代不变。

以下为设计上限，不一定经过实测。播放与 TTS 行属于 T2 建议：

| 资源 | 建议上限 |
| --- | ---: |
| 采集 WAV | 320,044 字节；含 4 KiB 标记的擦除范围 `0x50000`。 |
| 播放 WAV（T2） | 480,044 字节 / 15 秒；擦除范围 `0x77000`。 |
| PCM I/O scratch | 512 字节；不分配整段 RAM。 |
| worker 栈 / HTTP JSON 缓冲 / 视图 | 8 KiB / 8,193 字节 / 约 5 KiB。 |
| TLS 清理后的解析余量 | 初步预算 12–16 KiB，实际峰值需测。 |
| ASR / MT / 录音后管线 | 20 s / 15 s / 60 s 绝对预算；TTS 20 s 仍为 T2 建议。 |

仅 T2：MiMo 流式 TTS 示例为 24 kHz PCM16LE 单声道；非流式 WAV 必须解析真实头部，不能
假设采样率。服务端用受维护、固定版本的低通抗混叠重采样器，将支持的输入规范化为
16 kHz 单声道 PCM16。不使用已移除的 `audioop` 或裸线性插值。用 1 kHz 和高于
8 kHz 的合成音验证时长、字节、幅值及混叠抑制。超长/不支持音频只留下可用文字和
语音不可用状态，不播放半段音频。

| 状态/事件 | 行为 |
| --- | --- |
| READY：UP / OK | 切换方向 / 预检后录音。离线或时间不可信时明确报错。 |
| RECORDING：OK / 10 s / UP | 结束 / 达上限结束 / 取消。 |
| 上传、ASR、MT、T2 合成：UP | 取消、失效 generation、丢弃迟到结果。长 OK 取消并退出。 |
| RESULT：UP/DOWN | 滚动有标注的原文/译文。T2 OK 请求或复用合成。 |
| PLAYING（T2）：OK 或 UP | 停止回 RESULT。长 OK 停止并退出。停止目标为两秒内，需实测。 |
| 失败 | 区分认证、额度、临时故障、结果过期/未知和存储清理；不静默重试付费。 |

## 实施与验收交接

T1 已实现为纯逻辑 `suite_translation_core.*`、`suite_translation.*` worker、
重新设计的 `suite_translation_ui.c`、`suite_audio_job` scratch 所有权、独立
companion 翻译适配器/作业/认证，以及启动器和构建注册。LVGL 访问持锁，慢工作不在
按钮回调。静态中文标签复用已生成字体；动态文本复用公开字体并明确处理覆盖缺口。
参见 [T1 核心](passport-translation-t1-core.zh_CN.md)、
[存储与配置](passport-translation-device.zh_CN.md)及
[供应商/HTTP 契约](passport-translation-provider.zh_CN.md)记录。

T2 增加合成、规范化、有界下载/播放和取消。T1 单独不满足 SA-15，T2 尚未实现。

主机验收通过合成 I/O 执行真实 worker/传输路径：上限边界、畸形/重复 JSON、身份
不符、上传/哈希冲突、每个崩溃/claim 边界、取消/迟到结果、令牌权限矩阵、元数据
容量、清理隔离、任务数据保留、UTF-8/字体覆盖、重采样。聚焦测试已通过，2026-10-24
原生完整门禁通过并验证归档 `119f73c2...`；该构建仅用于验证，从未刷写。

真机验收仍需获准刷写及真实音频：双向翻译、原文/译文分区、长文与字形、逐阶段取消、
反复进退页面、TTS 音调/速率/停止时延、离线错误，以及设置/任务缓存/日志保留。
测量内部最低空闲堆、最大内部/DMA 连续块、栈水位、Flash 擦写耗时和 TLS/音频峰值。
链接尺寸与模拟测试不能代替这些测量。2026-10-24 OC-11 真机尝试在采集前即停于就绪
检查；设备当前是只有占位页的旧构建。

供应商/API 条款及 SC-08 Flash/协议例外已记录于上文“已确认决策”。独立翻译认证与
数据保留仍是工程要求；后续真实音频或付费调用须单独授权。

当前状态：

```text
Build: PASS（2026-10-24 原生完整门禁；归档 119f73c2...，仅验证未刷写）
Host tests: PASS（聚焦翻译、共享与 companion 测试）
Device tests: FAIL（真机翻译就绪失败；T2 未实现）
Unverified: 供应商质量/账单、就绪根因、真实资源峰值、T2/TTS、账户区域与保留条款
```


## OC-06 决策包，2026-10-22

历史决策包。其中的供应商、采集和保留选择已于 2026-10-23 由用户确定（见上文“已确认
决策”）；下表保留为按日期记录，不是当前待办。

决策版本：`T1-DP-2026-10-22-A`，已由 Codex 复核。OpenCode Go 会话
`ses_e9c64ac4fffey5pxSI3FepdjI7` 使用 `deepseek-v4.1-flash` 默认 variant；
本轮 WSL 原生 CLI 实际为 2.0.22。本决策包不代替用户选择，不实施 T1，也不授权
真实翻译调用；当前固件的待办验收独立推进。

### 推荐 T1 与待选事项

保留方案 A：MiMo `mimo-v2.5-asr` 显式指定 `zh`/`en` 识别，
`mimo-v2.6-flash` 负责文本，均调用
`POST https://api.xiaomimimo.com/v1/chat/completions`。识别使用非流式，文本使用
JSON-object，显式指定 `thinking: {type: disabled}`，限制
`max_completion_tokens`，严格校验字节数、标量数和请求身份。
当前[文本 API](https://mimo.mi.com/static/docs/api/chat/openai-api.md)的 v2.6 默认
启用思考；[ASR API](https://mimo.mi.com/static/docs/api/audio/Speech-Recognition.md)
支持显式语言。2026-10-22 已重新核对官方接口；没有调用供应商或测试翻译质量。

2026-10-10 生效的[MiMo 平台隐私政策](https://privacy.mi.com/XiaomiMiMoPlatform/zh_CN/)
适用于其 API 平台，表明未经事先同意不将文本用于训练，并说明按目的/法律需要保留、
删除申请、备份延迟删除及中国境内服务使用时的境内存储。政策没有给出固定 API TTL；
音频训练待遇以及实际账号地区/设置仍未确认。不能据此推断零保留合约、供应商取消
或退款保证。

| 待选事项 | 建议 | 未确定时阻塞的工作 |
| --- | --- | --- |
| 供应商/账号条款 | 方案 A；确认实际账号地区，并选择是否接受已说明的保留方式及未知音频训练待遇。 | 真实适配器和真实翻译音频阻塞；独立模拟工作仍须先获实施请求。 |
| SC-08 采集例外 | 明确批准在有所有权的非活动 Flash 槽暂存明文 WAV，并用一次有界 WAV PUT 代替仅易失内存/分块/finish。 | 设备采集/上传和对应合约修改阻塞。 |
| 真实调用 | 未来真实翻译调用单独授权；既有待办 ASR 授权不等于选择翻译。 | 真实调用验收阻塞。 |

独立翻译 capability/token、隔离预算、保留设置/缓存/日志/timer 及不调整分区仍是
要求。单独的供应商 API key 是可选运维选择，不新增为强制条件。TTS 音色与保留仍属
T2；T1 不能独自满足 SA-15。

### 数据与所有权复核

保留既有的 10 秒/320,044 字节 WAV 上限、文本仅 RAM 生命周期和服务端音频清理。
结果/音频过期后仍须保留持久去重行或 tombstone，不能因此自动重放已 claim 的付费
阶段。不把 Flash 磨损称为低：擦除频率、清理擦除和寿命均未测量。在 scratch 元数据
增加 owner/version 会改变该元数据，须明确设计旧 marker 兼容和重启清理，同时保留
待办日志、缓存与配置格式。

先取最外层 scratch 锁 S，再取音频 A 或网络 N；不同时持有 A+N，不在持 N 时取
S/A。待办语音、下载及缓存激活遵守同一所有权；worker 清理并释放所有权后确认退出，
才允许复用槽。UI 退出只取消并失效 generation，不等待网络；丢弃迟到结果。文本/音频
不进入待办 token/日志、请求日志或持久翻译元数据。静态标签与动态缺字提示遵循既有
中文字体检查表。

### 用户选择后的有界实施交接

```text
仅实施明确批准的 T1-DP-2026-10-22-A 子集。D1/D2 未确定时，只阻塞依赖它们的真实
适配器或采集/上传工作，不推定批准；实施本身仍须有请求。
真实翻译调用单独授权前使用模拟供应商。
负责 suite_translation_core/worker/UI、suite_audio_job scratch 所有权、伴随服务
翻译作业/认证/模拟适配器、最小待办所有权接线、字体/配置注册和相关主机测试。
保留既有设置、待办格式/日志/缓存/token、timer 和分区。
覆盖实际 worker/传输取消、generation/迟到结果、S->A/N 锁序、worker 退出确认、
清理隔离、重启/付费 claim 歧义、同载荷重放/异载荷冲突、token 矩阵、文本/音频上限
和字形。执行针对测试及完整门禁，复验精确镜像/ELF。
不含 T2、部署、刷写、真实调用、提交和发布。
```

详细文件职责仍见上文实施交接。
本决策包：Build: NOT RUN；Host tests: NOT RUN；Device tests: NOT RUN。
Unverified：用户选择、账号条款以及全部翻译实现、供应商质量、运行资源和真机行为。

## OC-10 T1 核心/mock 检查点，2026-10-23

OC-10 只实现上述与供应商无关的 T1 子集；见
[T1 核心检查点](passport-translation-t1-core.zh_CN.md)。生产供应商选择、账户条款、
录音/Flash 例外、真实翻译调用、T2、设备验收和 SA-15 仍未确定。本检查点不授权
其中任何一项。复核后修正了描述符精确长度、取消/发布竞态下的 RAM 缓存保护，
并在 service 入口清扫所有过期 job。

## OC-11 固件内界面/worker mock 检查点，2026-10-23

OC-11 实现了建议的本地界面增量：从启动器可进入的翻译页，具有明确的
`zh-CN`/`en` 方向控件、分开的原文/译文窗格、可见 MOCK 标识、处理/失败/取消/
过期状态、三按键导航及长文本滚动。有界 worker 使用现有
`suite_translation_core` 的准入/结果记账，输入和供应商均为合成，并通过可替换的
合成 provider 隔离外部边界。所有保留的明文字段，包括部分失败时保留的源文，
都只在 RAM 中并由页面观察 120 秒 TTL，退出时擦除。核心八槽表仍绝不淘汰去重
证据；八次不同 mock 操作后页面显示 `BUSY`，并在 worker 清理完成后提供显式两按
确认的本机 mock 重置。该重置推进新的 identity epoch，使第九个合成任务无需重启
即可运行，且不触碰生产/伴随存储。供应商输出必须在容量内 NUL 终止；恰好
2,048 字节 / 512 标量原样接受，未终止输出被拒绝。本页和 worker 不实现麦克风
采集、上传、Flash 暂存、Python 伴随/HTTP 路径、生产供应商、真实调用、T2 或
TTS。详见 [T1 核心检查点](passport-translation-t1-core.zh_CN.md)。

可信用户随后批准全部请求选项并回复“请继续”：MiMo 候选 A
（`mimo-v2.5-asr` 加 `mimo-v2.6-flash`）；接受已披露的无固定保留 TTL 和未确认
的音频训练处理；批准在自有非活动 Flash 槽临时保存明文 WAV 最长 10 秒，随后
单次有界上传并清理；以及在集成复核后推进真实供应商、设备和付费测试。实际账户
区域未独立确认，因此不声称区域保证。后续真实 T1 固件章节实现了采集/上传/
轮询适配器，但部署和实际供应商调用仍由 root 负责。T2/TTS 与 SA-15 仍未完成。拒绝 DeepSeek 生产集成；`deepseek-flash` 别名没有 ASR，
选定 MiMo ASR+MT 计划不变。OC-09
（待办候选）和 OC-10（T1 核心/mock）已在本增量前完成并独立复核。

## 真实 T1 设备集成代码检查点，2026-10-23

固件现已包含获准的真实 T1 链路：UI 默认 MiMo，MOCK 仍是显式本地测试模式，真实
失败绝不回退 mock。worker 加载私有翻译配置，检查在线/可信时间与 `/ready`，以
`TRANSLATION` 所有者使用共享 `suite_audio_job` 租约，按 16 kHz/16 位/单声道录制
0.3–10 秒、每块不超过 512 字节；在 TLS 前释放 BSP 音频；通过有界 Flash 读回
完成 WAV 封装/哈希；只执行一次 POST 和一次 PUT；轮询前释放 scratch，并执行
录音后 60 秒绝对预算。本地取消/退出优先于迟到服务结果。POST/PUT 结果不明为
UNKNOWN，不重试；MT 部分失败保留有效源文并使用同样 120 秒明文 TTL。服务端
job/generation 身份与本地 UI session/epoch 分离。RAM 中不保留整段 WAV。

共享 HTTP 传输源码已落地，当前树可构建/链接。共享协议现已包含 EXPIRED；worker
将其作为过期终态，并将 FAILED/AUDIO_EXPIRED 也映射为过期。其他协议解析失败为
UNKNOWN 且不重试。最终传输验证及设备/付费测试由 root 负责。聚焦主机测试、共享
测试和定向 ESP-IDF 构建通过；设备、真实供应商、付费与完整门禁结果仍未声称。
