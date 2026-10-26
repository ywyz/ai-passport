<p align="right">
  <a href="passport-translation-t1-core.md">English</a> · <strong>简体中文</strong>
</p>

# 翻译 T1 核心与合成伴随服务检查点

更新于 2026-10-26。OC-09（待办里程碑候选）与 OC-10（T1 核心及合成伴随服务）
已完成并独立复核。OC-11 增加重新设计的固件内翻译页、围绕实际
`suite_translation_core` 记账的有界 worker 及可替换的合成 provider。获准的真实 T1
集成增加默认 MiMo 的采集/上传/轮询 worker，并保留显式 MOCK 本地测试模式。

当前状态：伴随服务已部署，一次获准的合成真实供应商 EN->zh-CN 冒烟通过。原始 OC-11
应用已于 2026-10-25 仅应用恢复，用户已确认恢复后的翻译菜单和原有数据正常。当前已安装
应用为录音后响应缓存包 `d2975d36...`（应用 `79a0d3b5...`、ELF `9d71323f...`），于
2026-10-26 以一次受护栏的仅应用写入安装。一次 100.95 秒有界无复位启动窗口收到 0 个
串行字节、0 个 TRDIAG 事件、0 个故障标记，无 boot ELF 行或 trace；数据缺失不等于
干净、稳定或启动 PASS。用户随后宣布就绪；在该安装上完成两次受引导人工试验，一次
英译中、一次中译英：操作员报告两个方向识别、翻译与显示均正确，未见明显卡顿，并认为
响应速度不是很快但可以接受。设备测试仅对该“操作员报告的单方向单次”子集为 PASS；
延迟原因未确定，重复性、TTS 与 SA-15 仍未验收。先前 `902a0930...` 安装属已被取代的
按日期历史：其操作员报告的就绪/取消一轮正常，
其更晚真实 EN->zh-CN 在录音并按 OK 停止后以翻译缓存内存不足失败，无 A/F/L；失败 HTTP
调用与实机堆最大/空闲块未知。2026-10-26 完整门禁的 Build/Host PASS 沿用，未重跑固件。
先前 `cb8bd517...` 的 TLS 预检失败与天气 `task_wdt` 仍属已被取代的历史。精确计数、
产物与未决项见[当前检查点](#当前检查点2026-10-26)及[当前完整门禁](#当前完整门禁2026-10-26)。
T2/TTS 尚未实现，SA-15 仍不完整。用户决策记录如下。

## 当前检查点，2026-10-26

已安装身份。包 `d2975d36...`、应用 `79a0d3b5...`、ELF `9d71323f...`，3,697,184 字节，
于 2026-10-26 以一次仅应用写入落在 `[0x10000,0x397000)`。护栏 rc0 PASS
`APP_ONLY_WRITE_DIGEST_AND_CLOSE`，单次执行 78.57 秒：一次读加一次写桥接会话各自关闭，
COM3 已确认释放；写入应用前，全新实机 4,096 字节分区表扇区匹配 `ba728028...`，固定
esptool 摘要比对成功且标记为 1（原始设备摘要未记录）。完整包是应用源码包身份，不是整片
Flash 克隆：未写 bootloader、分区表和数据/密钥分区，设置、密钥、缓存、任务日志仅靠
区间隔离保留。

有界启动观测。一次全新无复位窗口 100.95 秒收到 0 个串行字节、0 个 TRDIAG 事件、
0 个故障标记，无 boot ELF 行或 trace。该缺失仅为按日期启动证据：它既不是当前总日志
状态，也不是干净、稳定或启动 PASS。其后即为下方人工验收。

双向人工验收。用户宣布就绪后，Codex 恰好引导一次英译中输入与一次中译英输入，其后
未重试、未再次触发。操作员报告英文原文被正确识别并显示，对应中文问候/早上好译文正确
显示；反向则识别出中文问候（例如“你好，早上好。”）并显示英文“Hello, good morning.”
译文。操作员明确报告两次均未见明显卡顿，认为速度不是很快但可以理解。该速度观察属性能
项目，原因未确定，不作服务等级、硬件上限或优化承诺。

英文试验窗口 100.96 秒、2,223 个串行字节，5 个数字 TRDIAG 事件（preflight、
capture_started、audio_released、capture、admit）结果均为 0，未捕获故障标记；
preflight、capture、admit 的累计本地耗时分别为 5.240764 秒、9.374270 秒、14.132428
秒，窗口内无 upload、status 或 final 事件。这些只是包含本地工作、网络与 TLS 的部分
本地累计时间，不是完整端到端或仅供应商延迟，也未证明客户端动作到窗口的时间。五个捕获
阶段最大块均为 9,216 字节，空闲字节依次为 35,524 / 23,560 / 31,524 / 31,524 /
30,876；不将更早的失败后 TLS 样本与之比较以声称因果字节收益。中文试验窗口 101.39 秒、
0 个串行字节、无 trace；这不否定操作员结果，也不证明无错误或完整运行。两个观测上下文
均以 rc0 关闭，COM3 已交还。其后未新增监控、录音、性能探测或供应商调用。

验收边界。这两次完成的试验未报告内存不足，但不证明永久修复或根因、稳定堆改善、重复
循环或无泄漏、长期稳定性、完整 SA-15 或 TTS 完成。当前设置、缓存与任务日志视觉检查
未单独报告，保留性仍只由刷写区间证据支撑。更早 100.95 秒零字节启动窗口仅为按日期启动
证据，不是当前总日志状态，也不是干净、稳定或启动 PASS。先前 `902a...` 安装仍属已被
取代的历史；其 admit、upload 或 status 是否实际发出仍未知。更早 `cb8bd517...` 的
TLS fatal 与天气 `task_wdt` 仍属关系未证明的未决历史，因此不把旧失败转移到
`d2975d36...`，也不称看门狗已修复。

先前安装，按日期历史。包 `902a0930...` / 应用 `a71b9c3e...` / ELF `0352858d...`，
3,696,416 字节，于 2026-10-26 仅应用安装，现已被当前刷写取代。其操作员报告的
“就绪 + 进入录音 + UP 取消”一轮正常；其 100.91 秒 / 112 字节启动窗口只属于它。该安装
上更晚一次真实 EN->zh-CN 在录音结束、按 OK 停止后以翻译缓存内存不足
（`无法准备翻译缓存，内存不足`）失败，未显示 A/F/L 数值。操作员确实执行了录音与停止；
是否实际发出 admit、upload 或 status 请求未验证，因此不声称上传或供应商计费完成。实机
堆最大/空闲块也未知。该失败与更早的 ready 前文本失败不同，也是本次响应缓存修复的原因；
不推断物理音频泄漏、稳定性 PASS 或根因。

响应缓存修复（已安装源码）。已安装源码在完成有界头部校验后，为全部五个 HTTP 调用分配
大小恰为 Content-Length + 1 的 NULL JSON 响应缓冲，最多 8,193 字节，8 KiB JSON 上限
不变。admit、upload 或 status 的迟到 E_BUFFER 视为结果不明，显示 UNKNOWN 及短状态
`内存不足，服务结果未知`。operation ID 仅在 session、取消与退出检查均有效时于 admit
前发布；已知 job 身份保留；不重放，也不发送未经请求的 DELETE。TLS 行为与 2,048 字节/
512 标量文本上限不变，没有新增全局 8 KB 分配，也没有配置、分区或 BSP 源码改动。源码
层面的收益未测量，不声称已证明预防。

内存生命周期改动。源码在全部五个 HTTP 操作中每次调用延后申请 8,193 字节 JSON 缓冲，
并在 ready 阶段再延后 4,098 字节；在 ready 之后、音频之前申请文本，同时保持 8 KiB JSON
上限、2,048 字节/512 标量文本上限、既有 OOM 文案、严格 TLS 行为与尽力取消。这些是
源码层面的减少量；实机堆收益未测量，也不声称内存修复或看门狗治愈。

已被取代的历史。先前 `cb8bd517...` / 应用 `d0396c22...` / ELF `0ac23853...` 安装记录了
一次 EN->zh-CN TLS 预检失败（`R7 S12 X11` / `E801A T3000 V0` / `N11 F12004 L4608`；
后来 `N11 F11800 L3840`）以及一次天气证书处理期间的运行时 `task_wdt`。两者属已被取代的
按日期历史，与当前安装的关系未证明，不是当前证据。此前仅确认一次 X.509 fatal；不声称
V0 代表证书有效。已被取代的 `43501...` 试验仍是历史人工 `R7 H0 B0` 的来源；原始
`f9a849be...` / `f5be0652...` 与 `8e86` 归档仍属历史/回滚。

未决。设备测试只接受上述“操作员报告的单方向单次”子集；它不建立重复性、无泄漏或
长期稳定性、永久内存修复、完整请求时序、供应商计费、TTS/播放或 SA-15。除五个部分采样
外，确切实机堆行为仍未测量；先前安装的失败 HTTP 调用仍未知；物理音频行为未验证。不把
任何旧失败转移到当前安装，也不称天气 `task_wdt` 已修复。当前剩余工作是延迟定位及其他
未验收场景，而不是再次刷写或基础翻译缺失。

## 已实现范围

- `main/suite_translation_core.*` 是无 ESP-IDF、LVGL、网络、存储或时钟依赖的
  纯 C11 状态/记账模块。它只接受明确的 `en` 到 `zh-CN` 与 `zh-CN` 到 `en`
  方向，分开保存原文和译文元数据，并校验每段文字不超过 2,048 个 UTF-8 字节
  和 512 个 Unicode 标量值。畸形 UTF-8、过长编码、代理码点和 NUL 字节会被
  拒绝，不做截断。
- 合成 WAV 信封上限为 320,044 字节，16 kHz、有符号 16 位小端单声道 PCM。
  模块校验头部、数据长度、至少 0.3 秒的载荷和 SHA-256 绑定；它不复制或保留
  音频。
- 操作携带 operation ID、独立的 job ID 和单调 generation。operation ID 只接受
  恰好 32 个小写十六进制字符，音频 SHA-256 只接受恰好 64 个；有效前缀后多出
  任何字符都会在查找、占槽或推进 generation 之前拒绝。相同请求重放返回原准入，
  不重复供应商 claim；相同操作的不同载荷是冲突。有界八槽表满时以 `BUSY` 拒绝
  新准入，绝不淘汰去重证据。
- 上传、运行、终态结果和取消是显式状态，并带有有界事件环。已取消或已终态的
  job 拒绝迟到结果。界面退出失效会取消活动 job 并推进本地 generation。
- `companion/translation/` 是独立 job 实现，拥有独立 SQLite 元数据账本、
  独立 Bearer capability、内存合成供应商和 WSGI 准入/状态/取消接口。它不导入
  或共享 Tasks 存储、journal、token、缓存或管理员凭据。
- 伴随账本只保存标识、载荷/音频/文字哈希、字节数、状态、generation 和时间。
  原文、译文和音频只留在有界内存或合成供应商中。供应商 claim 后重启变为
  `UNKNOWN`；持久 `DONE` 行对应的文字已不在内存时显示 `TEXT_LOST`。容量压力
  拒绝新准入，不删除既有证据。
- 本地 job 链路用合成 ASR、合成翻译和合成 WAV 走通端到端流程。音频上传仍是
  本地函数调用：单次 WAV HTTP PUT、Flash 暂存和生产适配器均未由本 mock 实现。
  用户后来批准了未来真实适配器的 Flash/WAV 方向；OC-11 仍未实现。
- 每个 `TranslationJobs` 实例的锁串行化 RAM 缓存发布与取消，包括短时间的
  SQLite 读取和取消事务；供应商调用始终在锁外执行。发布时重新检查持久状态
  和 generation，拒绝为已取消槽位回填文字。ASR
  与 MT 使用同一保护，迟到回调返回取消状态而不是未处理 generation 异常。
- 过期文字在每个 service 入口和显式 `cleanup()` 接口统一清扫。本地 mock 没有
  后台 scheduler 或全局 timer：过期文字可能在下一次 service 调用前仍物理占用
  内存；默认 TTL 下，120 秒后不再可见。元数据行保留，作为去重证据。

## OC-11 固件内页面与 worker mock

OC-11 增加启动器可见的翻译增量。它是重新设计的应用页面，不是只改标签的占位页，
但仍为合成、本地实现。

- `main/suite_translation_ui.c` 提供页面：诚实显示 `MOCK 本机模拟` 标记、两行
  明确方向（`英译中`、`中译英`）、状态行，以及包含分开的 `源文` 与 `译文` 标签
  窗格的可滚动正文。固定标签使用 `suite_font_16`；动态原文/译文使用现有公共
  动态字体 `suite_tasks_font_16`。
- 交互使用现有三按键。就绪态 UP/DOWN 切换方向，OK 开始；任务运行中 OK 取消；
  终态 UP/DOWN 滚动长文本，OK 返回方向选择以便再次开始；长按 OK 经共享外壳
  退出。提示行始终说明当前动作。
- `main/suite_translation.c` 每个派发任务拥有一个有界 worker。worker 使用实际
  核心的准入、上传、运行和终态结果路径，并在堆上临时构造 9,644 字节合法合成
  WAV。WAV 哈希在快照锁外计算；核心调用（含上传校验哈希）使用独立短核心锁。
  它不访问 LVGL、存储、凭据或网络。
  `main/suite_translation_provider.h` 隔离可替换的合成结果边界；
  `main/suite_translation_mock.c` 是固件内实现，后续获准的真实适配器可替换它而
  不改变界面、取消或 generation 逻辑。
- 同一时刻只有一个 worker 拥有清理权。在 worker 确认清除 `worker_busy` 前，
  `suite_translation_run` 拒绝新任务。页面退出不等待，直接取消并推进
  session/generation；旧 session 的迟到结果和清理被丢弃。任务创建失败进入明确
  `UNAVAILABLE` 状态，之后仍可重试。快照释放助手会排空持锁期间到达的取消/退出
  意图。所有锁所有者（包括 `suite_translation_view_release`）都参与；若另一
  所有者抢到锁，它会在释放时执行同样排空。因此即使页面定时器已删除、之后不再有
  任何视图读取，离屏清理也会完成。worker 在确认清理前还会再次检查并清空完整
  缓冲区。
- 页面呈现处理中、完成、部分失败（翻译失败时保留已识别源文）、取消、过期、
  元数据已满和不可用状态。所有保留的明文字段，包括部分失败时保留的源文，都只有
  RAM 内 120 秒 TTL，由页面定时器观察；到达精确截止时间即清空两个缓冲区。页面
  退出也会擦除完整文字缓冲区。核心元数据保留作去重证据。这不是持久存储，也不
  声称空闲时会物理删除。
- 供应商输出必须在 2,049 字节容量内包含 NUL 终止符。worker 不再自行写入终止符，
  因此未终止缓冲区会被拒绝而不是静默接受。恰好 2,048 字节 / 512 标量的文本
  原样接受；畸形或超限输出失败关闭。
- 核心八槽表不变。八次不同 mock 操作后新任务报告 `BUSY` 与 `8/8`，不淘汰证据；
  页面随后提供显式两按确认的本机 mock 恢复（提示为 `OK清除记录`），且仅在 worker
  清理完成后可用。该动作只重新初始化固件内易失 mock 表、推进 identity epoch，
  使第九个合成任务无需重启即可运行；不触碰伴随服务、生产去重或持久存储。重置前
  方向选择、滚动和取消仍可用。
- 本页固定中文标签复用 `assets/fonts/suite_charset.txt` 中已声明且由
  `main/fonts/suite_font_16.c` 覆盖的字符，无需重新生成字体。内置 mock 的
  动态文字按公共动态字体校验；未来供应商文字超出该字体时仍显示 LVGL 显式
  占位符，不静默丢弃或替换。
- OC-11 mock 增量本身不实现麦克风、真实录音/上传、scratch Flash、存储格式变更、
  Python 伴随/HTTP 路径、生产凭据、真实翻译调用、T2 合成/播放或 SA-15。下方
  获准的真实 T1 章节增加了固件采集/上传/轮询链路，但仍不实现 T2/TTS，也不声称
  SA-15。

主机测试在默认与 `-O2` 下用外部 FreeRTOS/LVGL/provider/时间桩执行实际改变的
worker 与页面路径：真实线程的派发前与工作中的取消、部分源文保留及精确 TTL
边界、恰好 2,048 字节 / 512 标量接受、未终止输出拒绝、退出时完整缓冲区擦除、
核心结果与最终界面发布边界的取消（含部分源文）、发布期间立即退出/重进、清理
释放边界取消/退出且之后无快照/轮询、完整缓冲区擦除、立即重进、方向与窗格分离、
互斥量与 worker 内存分配失败恢复、任务创建失败恢复、快速退出/重进、旧 session
拒绝、八次准入重置及页面拆除。

## 真实 T1 设备集成（代码检查点）

真实链路已实现，供 root 后续部署/复核。它不使用 mock 核心表、本地 mock 重置或
整段 RAM WAV。

- 页面默认显式 `MiMo 联网` 模式。`MOCK 本机合成` 是单独且明确标注的选择，真实
  失败绝不静默回退。行项为 `EN 译 中文`、`中文 译 EN`、`USB 配置` 与当前模式。
  `USB 配置` 打开现有最长 60 秒的 USB Serial/JTAG 窗口；离开页面会取消它。
  LVGL/输入锁内不执行 NVS、TLS、音频或 Flash 操作。
- 录音只由用户显式发起。OK 结束，UP 取消；忙时 UP/OK 取消。现有 worker 拥有
  流水线，无需额外 capture 文件。worker 加载私有翻译配置，检查在线与可信时间，
  通过 HTTP 传输调用需认证的 `/ready`，然后以 `TRANSLATION` 所有者获取共享
  `suite_audio_job` 租约。它固定共享缓存 generation，使用
  `SUITE_AUDIO_TRANSLATION_SPAN`（0x50000）、16 kHz/16 位/单声道，最多 320,000
  PCM 字节（10 秒），至少 9,600 PCM 字节（0.3 秒），每块不超过 512 字节。
  实际 BSP 读取可能因 codec/I2S 信号量每次阻塞约 1 秒，因此存储 PCM 上限精确，
  但墙钟采集与取消延迟受该读取限制；不将其伪装成 16 ms 保证。
- 任何 TLS 调用前先释放 BSP 音频。释放失败则拒绝网络并保留清理/隔离结果。
  规范化 WAV 由有界 Flash 读回完成并计算哈希；32 字节摘要直接十六进制编码，
  绝不二次哈希。录音后 60 秒截止时间在 PCM 循环后、BSP 释放前立即设置，覆盖
  释放/封装/哈希/准入/上传/轮询且不重置。POST 只尝试一次；PUT 通过租约读回调
  恰好流式上传一次。数字诊断增加时间戳/耗时后缀及独立的 `capture_started`、
  `audio_released` 检查点，用于区分 DMA 占用/释放测量。POST/PUT 结果不明进入 `UNKNOWN`；不重试、不重放、不
  新建付费任务。上传后、轮询前以仅 S 方式释放租约。清理失败会隔离槽位并显示
  存储失败。任何在尝试 init/format 后退出（包括采集循环前取消/退出）都会释放
  BSP 音频；释放失败阻止 TLS 并显示失败。
- 状态轮询使用服务端独立 `job_id`/generation 身份，不使用本地 UI session/epoch。
  在 DONE/FAILED/CANCELLED/UNKNOWN/TEXT_LOST 或录音后绝对 60 秒预算到达时停止。
  DONE 必须有原文和译文；FAILED 若有有效源文则保留，并使用同样的 120 秒明文
  TTL。本地取消/退出优先于迟到服务结果；已知 job 身份时最多发送一次有界
  best-effort DELETE。POST 身份丢失时显示 UNKNOWN，绝不盲目删除或重试。
- worker 诊断只含数字（阶段/状态/字节/耗时/堆/DMA/栈），不记录文字、音频、
  token、URL 或主机身份。五个调用各自在完成有界头部校验后按 Content-Length + 1
  分配 JSON 响应缓冲，文本缓冲区也在堆上分配；不保留整段 WAV。配置与临时文本在
  清理时擦除。已复核的释放排空清理交接继续用于离屏明文。

共享协议现已包含 `EXPIRED`，worker 将其作为终态过期结果；`FAILED` 且错误码为
`AUDIO_EXPIRED` 时也显示过期。其他协议解析失败仍为 `UNKNOWN` 且不重试。HTTP
传输源码已落地且当前树可链接，但 DNS/TLS/取消细节仍属传输所有者契约，本检查点
不重新验证。

## 主机测试覆盖的 job 行为

| 事件 | 已实现行为 |
| --- | --- |
| 相同准入重放 | 返回原 job 和 generation；不新增供应商 claim。 |
| 相同操作、不同载荷 | `OPERATION_CONFLICT`；不覆盖、不调用供应商。 |
| 相同上传重复 | 返回既有状态；不重复 ASR。 |
| 供应商回调期间取消 | 丢弃迟到结果；不进入翻译阶段、不保留文字。 |
| 取消后的迟到结果 | 由终态和已推进的 generation 拒绝。 |
| claim 后重启 | `UNKNOWN`；不自动重放付费阶段。 |
| `DONE` 后重启 | `TEXT_LOST`；元数据不是可恢复译文。 |
| 元数据容量满 | 新准入返回 `BUSY`；既有行保留。 |
| 出示 Tasks token | 拒绝；只接受独立翻译 capability。 |

## 验证

针对性主机检查：

```text
cc -std=c11 -Wall -Wextra -Werror -Imain \
  tests/test_suite_translation_core.c main/suite_translation_core.c
cc -std=c11 -Wall -Wextra -Werror -O2 -Imain \
  tests/test_suite_translation_core.c main/suite_translation_core.c
python3 tests/test_companion_translation.py
```

OC-11 聚焦检查（默认与 `-O2`）执行实际改变的 worker 与页面：

```text
cc -std=c11 -Wall -Wextra -Werror [-O2] -pthread -DSUITE_TRANSLATION_TEST \
  -Itests/translation_stubs -Imain \
  tests/test_suite_translation_worker.c main/suite_translation_core.c \
  main/suite_translation_mock.c
cc -std=c11 -Wall -Wextra -Werror [-O2] -Itests/tasks_ui_stubs \
  -Itests/translation_stubs -Imain tests/test_suite_translation_ui.c
PYTHONDONTWRITEBYTECODE=1 python3 tests/test_suite_font_charset.py
./tools/validate.sh --static
```

OC-11 结果：

```text
Build: PASS（OC-11 较早修订在隔离 ESP-IDF 5.5.3 下完整链接；应用 0x37b310
       字节，最小应用分区剩余 0x234cf0 字节 / 39%）。最新 OC-11 翻译对象可编译；
       当前共享树最终链接被下列并发 suite_audio_job 集成阻塞。
Host tests: PASS（默认与 -O2 的真实 worker/界面；字体字集与仓库检查）
Static gate: 共享树中被并发的非本任务 suite_audio_job 集成阻塞
             （suite_audio_job_* 未定义符号），OC-11 全部检查已先通过
Device tests: NOT RUN
Unverified: 并发集成稳定后的共享树最终链接/合并镜像归档/ELF、实屏渲染/输入、
            真实供应商、付费调用、T2 与 SA-15
```

确定性运行时复现还报告：部分源文在 TTL 后清空（`expires_at=0`）、退出后完成
文字清空（`0/0`）、未终止 provider 输出被拒绝（`accepted_DONE=0`）、最终界面
发布期间的取消不保留源文（`source_retained=0`），以及最终清理释放边界退出后
`retained_source=0 retained_target=0 exit_pending=0`（之后无快照或轮询）。测试
检查完整原文/译文缓冲区，而不只是首字节。

本次 OC-11 构建未运行 `merge-bin`、归档校验或刷机。随后共享树链接失败仅因
并发且非本任务的 `suite_tasks.c` 改动引用了尚未注册的 `suite_audio_job_*`
函数；OC-11 未修改这些路径。共享树稳定后由 Codex 运行完整门禁。此前复核的
OC-10 完整门禁产物是该 OC-10 源码的历史结果，不适用于本增量：

```text
Archive: build/firmware/c284de8dd95dd443b8154714a020f4ffbf4537c94ad7c6bca0acbc3b39d97b21
Full image SHA-256: c284de8dd95dd443b8154714a020f4ffbf4537c94ad7c6bca0acbc3b39d97b21
ELF SHA-256: 859d5cf3d582690a977dbc9a13c16cece1e3704d0aad2511e2640372fc69ecd6
Application image SHA-256: 6f045ff14d551f3f940479371f79ba0ed4829a55a3f20cc654ad61f4e681f40b
Merged image size: 3,696,656 bytes
Application size: 3,631,120 bytes at 0x10000
Version: 21c5110-dirty
```

`python3 tools/archive_firmware.py verify` 对上述归档通过。更早一次完整门禁在
初版静态检查后因 WSL 环境重启中断；它没有完成标记或新归档，记录为中断而不是
构建通过。链接构建和合成测试不能证明设备录音、供应商质量、账户条款、保留策略
或 SA-15 验收。

### 当前完整门禁，2026-10-26

已审查的录音后响应缓存修复通过一次完整门禁（`./tools/validate.sh`，不是分开的
静态与固件运行），仅以原生树为构建输入，并在已激活、固定的 ESP-IDF 5.5.3 下执行：

```text
Build: PASS
Host tests: PASS
Device tests: PASS（仅限操作员报告的单方向单次识别/翻译/显示子集且未见明显卡顿；
              见检查点）
Unverified: 重复性与长期稳定性、完整端到端及仅供应商延迟、部分采样之外的实机堆、
            物理音频行为、计费、TTS/播放、SA-15
```

```text
Archive: build/firmware/d2975d36eca1b89d98dd27f47711833f06afb4a8bb9a618eee11b0d357b94b0d
Full image SHA-256: d2975d36eca1b89d98dd27f47711833f06afb4a8bb9a618eee11b0d357b94b0d
Application image SHA-256: 79a0d3b5a59f5d1084267642837566b66054cb5d3ff3e06003457197371f59c1
ELF SHA-256: 9d71323ff8a1eb3231171dd2e6834f0f19d2ed07fb52c8fd413d4a97091ab66f
Merged image size: 3,762,720 bytes
Application size: 3,697,184 bytes at 0x10000
Partition table SHA-256: 34899c03...（3,072 字节；补齐 4,096 字节扇区 ba728028...）
Version: 21c5110-dirty
Status: 门禁阶段 VALIDATED_NOT_FLASHED；随后已安装
```

`python3 tools/archive_firmware.py verify` 通过，且应用镜像内嵌的 ELF 身份与归档
ELF 匹配。按扇区取整的应用区间 `[0x10000,0x397000)` 结束于 `0x5c0000` factory
边界之前，且不重叠受保护数据分区。门禁阶段的 `VALIDATED_NOT_FLASHED` 记录属历史；
`G` 未变。随后一次受护栏的仅应用写入安装了该确切包：护栏 rc0 PASS
`APP_ONLY_WRITE_DIGEST_AND_CLOSE`，写入前全新实机 4,096 字节分区表扇区匹配
`ba728028...`，固定 esptool 摘要比对成功且标记为 1，COM3 已释放。它现为当前已安装
应用，检查点记录的两次受引导人工试验即在其上完成。安装动作本身未发起供应商或付费调用，
也未重建固件。

### 真实 T1 聚焦验证，2026-10-23

```text
真实 worker 采集/配置/传输/身份/清理：PASS（默认、-O2），包括原始摘要十六进制
身份、init/format 失败后的 BSP 释放、ready/admit/upload/status 期间取消、已知
job 的单次 DELETE 及 EXPIRED 终态
真实翻译页模式/配置/录音/生命周期：PASS（默认、-O2）
翻译协议与 HTTP 传输聚焦测试：PASS（默认、-O2）
共享音频租约/配置/USB 测试：PASS
仓库检查与字体字集：PASS
隔离 ESP-IDF 5.5.3 构建/链接：PASS
  应用 0x385ce0 字节，0x22a320 字节 / 38% 空闲
Device tests: NOT RUN
Unverified: 真机 USB/配置、采集/上传时序与断电、真实 MiMo/付费调用、设备端
            DNS/TLS/取消、EXPIRED 枚举集成、T2/TTS 与 SA-15
```

构建包含已落地的 `suite_translation_http.c` 及共享 scratch/config/USB 源码。
这是一次定向集成构建，不是完整门禁；root 会在所有并发所有者完成后运行完整门禁。

## 2026-10-23 记录的用户决策

可信用户明确回复“都允许”和“请继续”：

- 选择候选 A：MiMo `mimo-v2.5-asr` 负责识别，`mimo-v2.6-flash` 负责文本翻译。
  接受已披露的无固定保留 TTL 及未确认的音频训练处理。
- 批准在自有非活动 Flash 槽临时保存明文 WAV，最长 10 秒，随后单次有界上传并
  清理。该决定批准 SC-08 采集例外方向，但本固件 mock 增量不实现真实适配器/服务。
- 真实供应商、设备和付费测试在集成复核后获准。实际账户区域未独立确认，因此不
  声称任何区域保留保证。
- 拒绝 DeepSeek 生产集成。MiMo ASR 加 MiMo MT 仍是选定方案；DeepSeek V4.1
  Flash 不在此作为生产翻译器。官方 `deepseek-flash` 别名不提供 ASR。
- T2/TTS 仍未实现，SA-15 未验收。

## 剩余依赖

- 真实 MiMo 适配器、伴随 HTTP 音频路径和部署已实现并运行；一次获准的合成供应商
  冒烟通过。更多真实或付费调用需单独授权。
- SC-08 Flash/WAV 路径及有界清理已在真实 worker 中实现；真机擦除行为、耗时和磨损
  仍未测量。
- 设备录音与停止已在先前 `902a...` 安装上执行：操作员确认就绪/取消子集，随后一次真实
  翻译在录音后以翻译缓存 OOM 失败。是否实际发出 admit、upload 或 status 请求未验证，
  因此不声称上传或供应商计费完成。原始 `FAIL_CACHE_ALLOCATION`（`failure_result=6`）与
  readiness8 属历史。当前 `d2975d36...` 应用已安装，并通过操作员报告的单方向单次
  双向子集；剩余工作是定位已报告的速度偏慢（原因未确定）及其他未验收场景：重复循环、
  长录音、本镜像上的取消/超时/离线恢复与断电、供应商计费及 SA-15。精确计数、产物与
  未决项见 [当前检查点](#当前检查点2026-10-26)。T2 合成/播放、OTP、票夹和任何进一步
  刷机仍不在本检查点范围内；该结果不再计划监控、录音或供应商调用。
- 设备测量和真机验收未完成。T1 core/mock 不是完整翻译功能，也不是 SA-15 验收。
