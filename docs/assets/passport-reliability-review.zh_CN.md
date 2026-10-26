[English](passport-reliability-review.md) · **简体中文**

# OC-01/02/03 复核与验证检查点

**按日期保留的证据记录。** 本文保留 OC-01 至 OC-11 的复核与测量历史，不代表当前
项目状态。当前权威见 [suite-plan 第 0 节](passport-suite-plan.zh_CN.md#0-当前实现与验收)。
此前的 WSL `p9_client_rpc` 现象属于共享文件系统访问问题，已于 2026-10-22 通过迁移到
原生 ext4 解决；它不是未决阻塞，也不意味着继续前必须先恢复 WSL。

日期：2026-10-21，Asia/Shanghai。基线 HEAD 为
`21c5110548e329019b2418ded506db68fe991418`，分支
`feature/feature10-ble-provisioning`，保留原有脏工作区。本次 OpenCode 执行和 Codex
复核在 2026-10-21 推进到验证阻塞。下列 WSL 原生运行已完成自动验证；真机验收仍待执行。

## WSL 原生验证，2026-10-22

用户已授权在 Linux 文件系统中建立独立验证副本。目录为 WSL Ubuntu ext4 上的
`/home/ywyz/code/ai-passport-oc-20261022/repo`，使用相同基线 HEAD 并覆盖源工作区
修改。487 个源条目均已复制并核对哈希；测试前，上一轮最终候选清单中的 18 个文件
全部匹配。两边原有工作副本均保留。既有 `/home/ywyz/code/ai-passport` 的 HEAD
不同且有自己的未提交修改，没有将它用作本候选，也没有覆盖它。

最终 28 组运行时和纯语音测试在默认与 `-O2` 构建下均 PASS。完整
`./tools/validate.sh` 使用 Linux 原生 Node.js、Python、GCC 和 ESP-IDF v5.5.3
通过。Companion 执行 Tasks 30 条、completion 15 条和 voice 19 条，零跳过；字体
覆盖和其余静态门禁均通过。本轮没有出现需要再次交给 OpenCode 回修的新失败。
验证未改变候选的生产代码、测试、字体、配置和锁文件字节；之后仅更新这些双语检查点。

固件门禁使用已维护的默认配置和 `partitions.suite.csv`，未使用忽略的本地
sdkconfig。应用为 3,624,704 字节，factory 分区为 5,963,776 字节；已核验合并
镜像为 3,690,240 字节，目标为 8 MB ESP32-C3。另行执行
`tools/archive_firmware.py verify` 也通过。

- 完整镜像 SHA-256：`7c292cfc48ed58de50ffa71d3fb5dafb0c88cddb21f5d2289588780f51819d51`。
- 匹配 ELF SHA-256：`e8b6ae41f0bd32690107aafb73bb3990836d688a0349700432f5ddc8fdb3521a`。
- 已核验固件包：`build/firmware/7c292cfc48ed58de50ffa71d3fb5dafb0c88cddb21f5d2289588780f51819d51/`。
- 合并镜像：`build/FoloToy-AI-Passport-full.bin`。
- 本地清单、专项日志、完整门禁日志及固件包复验记录：
  `/home/ywyz/code/ai-passport-oc-20261022/evidence/`。

Linux 原生访问和 IDF 激活正常。此前的 `p9_client_rpc` 观察涉及共享文件系统
访问，不足以说明 WSL 原生存储阻塞。本次无需重启 WSL。只读枚举时，WSL 未暴露
任何 ttyACM/ttyUSB 串口；这不能说明 Windows 宿主机未连接设备。

## 执行与复核

三个会话的元数据均确认为 `providerID=opencode-go`、
`model.id=deepseek-v4.1-flash`、`variant=default`；本机 CLI 为 2.0.32。

| 任务 | 会话 | 结果 |
| --- | --- | --- |
| OC-01 | `ses_ea13a76aaffezLo8zwAgLGhM31` | 待办/语音只读可靠性审查；Codex 收窄发现并选择回归。 |
| OC-02 及回修 | `ses_ea131593bffezbd6sQeTLUxe9V` | 先补测试，再分别下发生产修复和复核回修。 |
| OC-03 | `ses_ea134d1f4ffedPDm40hs0S2dCY` | 翻译提案经两轮复核修订，保存为[复核后的方案](passport-translation-plan.zh_CN.md)。 |

OpenCode 的图索引可用，但覆盖时效为 `metadata_changed`，因此仍需直接核对源码。
Codex 当前会话无图工具，使用 codebase-memory 技能的源码回退。双方均未把“覆盖无
记录缺口”当作完整性证明。

复核保留了稳定操作 ID、持久回执、CalDAV 条件写入、严格回读和缓存保护，并修正
两项过度断言：创建/完成共用受限写令牌；QUEUE_FULL/未知失败不能安全地视为可放弃。
周期性轮询仍是未测量的设计取舍，本轮没有新增退避策略。

## 修复候选

| 范围 | 修复后行为 |
| --- | --- |
| 慢转写 | 仅转写响应等待可使用请求剩余 45 秒预算。握手/上传及普通预检/创建仍有 10 秒阶段上限。每阶段按请求绝对期限夹取，拒绝迟到响应；没有新增 ASR 自动重发。 |
| 明确准入拒绝 | 仅精确 HTTP 400 `CREATION_TARGET_REQUIRED` 可进入既有 READONLY 状态，经明确确认放弃。未知错误、队列满、畸形体和传输结果不明均保留意图。 |
| 错误提示 | 区分认证/配置、额度及临时语音故障；不显示未知或私密上游载荷。 |
| 日志未就绪 | 开始、清预览、worker 启动前取消均在存储/配置不可用时保持可见失败；已接受的开始操作不再静默停在 CHECKING。 |
| 解析/传输边界 | 拒绝重复/缺失/非字符串错误字段、尾随垃圾、键/值转义 NUL、实际 HTTP 体中的原始 NUL；诊断前清空未完整响应。 |
| 字体 | 固定字集增加 U+989D/U+5EA6，用固定公开 Noto 源和 lv_font_conv 1.5.3 重生成 16 px 字体；控件字体绑定不变。 |

生产修改仅在 `main/suite_tasks_voice_impl.h`、`main/suite_task_voice.c/.h` 和固定
字体/字集。另加一个等价的 unsigned 显式转换，解决独立 RISC-V/桩语法检查暴露的
既有日志格式类型警告。BSP、已部署 companion 代码、分区、凭据、完成协议及日志
布局均未修改。

测试通过外部 I/O 桩执行真实语音 worker/HTTP 路径，覆盖超时顺序、完整响应后的取消、
明确与含糊拒绝、NUL/重复字段错误、未就绪的开始/取消交错、本地确认保存失败后同 ID
重放、服务端 lease 恢复、不重复 ASR 的显式 resume，以及持久准入前后的拒绝边界。
最终运行时源码为 28 组，另含纯解析断言和三项服务端新回归。

## 历史验证记录，2026-10-21

| 检查 | 实际结果 |
| --- | --- |
| 初始完整 `./tools/validate.sh --static` | 本轮编辑前 PASS；WSL 下 61 条 companion 用例零跳过，不能据此验证当前候选。 |
| 选定的红回归 | 生产修复前，Codex 在默认与 `-O2` 下独立复现 7/18 组失败。 |
| 最后传输/取消补充之前的运行时 | Codex 独立运行默认与 `-O2`，26/26 组 PASS。 |
| 最终原生 Windows Python 回归 | Tasks 30 条含 1 条 POSIX 权限跳过；completion 15/15、voice 19/19、字体覆盖 4/4；已执行部分 PASS。 |
| 最终 RISC-V GCC 14.2 配桩语法检查 | `-std=c11 -Wall -Wextra -Werror -O2 -fsyntax-only` PASS；不是固件构建或运行时测试。 |
| 最终 28 组执行 | 最后补充后 NOT RUN。 |
| 最终完整静态门禁 | BLOCKED；此前一次在英文字体文档问题处失败，已修正；重跑卡在 WSL 共享文件 I/O，随后中断。 |
| 固件门禁/合并镜像 | NOT RUN；本轮没有新的已验证固件包。 |
| 设备测试 | 本候选 NOT RUN。 |

观察到 WSL Python 处于 `D` 状态，等待点为 `p9_client_rpc`；独立编译和有界本地副本
探测也超时。原生 Windows 检查仍可用。本轮没有重启 WSL 或终止他人的工作。
OpenCode 报告的排除红例诊断运行，没有用于门禁放行。

后续 OpenCode 编译器探测在不适用的 shell 中使用 `2>nul`，生成了一个 34 字节的
保留名文件。初始检查点没有该文件，Codex 已把本轮自有产物移至本地证据目录保留。
最终 45 秒有界运行时重试仍无输出并超时；没有证据认定该文件是更早 WSL 阻塞的根因。

本地证据位于 `build/acceptance/20261021-oc-cycle/`：模型元数据、红/绿专项日志、
原生 Python 结果、语法检查、最终回修记录和源码清单。这些本地记录未提交或发布。

## 真机刷写与后续验收

用户已批准精确的 `7c292cfc/e8b6ae41` 固件分段刷写。COM3 的 ESP32-C3/8 MB
身份及当前分区扇区已核实；引导程序、表和应用三段写入及独立校验通过。实际擦除
范围未覆盖任何数据分区。90.52 秒正常复位窗口匹配 ELF，应用就绪、保留的 Wi-Fi
联网及 HTTPS 天气更新通过；没有观察到错误日志或崩溃，串口已释放。完整范围及
采样见[设备记录](passport-device-tasks.zh_CN.md#可靠性候选刷写与启动2026-10-22)。

用户已确认原任务缓存保留、六条最新任务下载/显示正确；录音/中文预览/取消仍待确认。
源同步排查及本次下载证据见[设备记录](passport-device-tasks.zh_CN.md#源数据新鲜度排查2026-10-22)。
不得把启动日志通过扩展为这些交互已验收。截至该日期翻译 T1/T2 仍是提案，SC-08 例外尚未确定；后续获准决策和证据见[翻译方案](passport-translation-plan.zh_CN.md)。本轮没有
替换应用源码、新任务确认、提交或推送；后续获准启用的服务端读取 timer 已完成两轮自动同步，
见[部署记录](passport-nextcloud.zh_CN.md#五分钟-timer-部署2026-10-22)。

Build：PASS。
Host tests：PASS。
Device tests：PASS，仅限分段刷写/校验及启动/联网/天气子集。
Unverified：最初刷新失败原因、录音/中文预览/取消、新错误文案渲染、真实语音/
取消时延、堆/DMA/栈峰值、物理掉电和混合运行。


## OC-04/05/06 后续执行，2026-10-22

用户已授权在既有 WSL 原生目录执行下一组提示词。原生 OpenCode CLI 2.0.22 完成
三项任务；导出的 assistant 消息元数据均经独立核验，确认为
`opencode-go/deepseek-v4.1-flash` 默认 variant。

| 任务 | 会话 | 复核结果 |
| --- | --- | --- |
| OC-04 | `ses_e9c64ac39ffeRWkenRepa5uFUL` | 当前固件验收矩阵；源码复核后修正：离线/准入提前返回不输出两条下载日志，缺日志不能证明失败；标准时间戳存在，但缺少语音开始/阶段时间戳。 |
| OC-05 | `ses_e9c64ac5fffe4KxP9lFU5hEe6C` | 只读受理后补一项已复核的测试覆盖，不臆造生产缺陷。 |
| OC-06 | `ses_e9c64ac4fffey5pxSI3FepdjI7` | 翻译决策包，见已复核的 [T1 决策包](passport-translation-plan.zh_CN.md#oc-06-决策包2026-10-22)；未实施或代选供应商。 |

除成对检查点文档外，仅修改 `tests/test_suite_tasks_runtime.c`。新增真实 worker
预检用例断言目标不可写的固定文案、终态失败、不启麦克风/ASR/写意图、保留缓存及
释放 client/网络所有权。只在复制的头文件中移除该文案映射：旧 28 组漏检，新 29 组
恰好检出此例，默认和 `-O2` 均如此。Codex 独立复现四组变异运行及两组未变异的
29 组通过。生产源码、BSP、字体、配置、分区、锁文件和已部署服务代码字节未变。

ESP-IDF 5.5.3 完整门禁通过，含 64 条 companion 用例、零跳过。新包另行复验通过：

- 仅验证镜像 SHA-256：`8e5bfcdf0d7d8139d12fc02e6d4271d8d5998a91ecc3b3b22e3b3cc33d2fd41f`。
- 匹配 ELF SHA-256：`221c99980e6b93d16c6b2c487b82c8f8cd7f01f2dd0143b9aab636648745a436`。
- 固件包：`build/firmware/8e5bfcdf0d7d8139d12fc02e6d4271d8d5998a91ecc3b3b22e3b3cc33d2fd41f/`。

该构建未刷入。真机验收仍绑定既有 `7c292cfc/e8b6ae41`，已核验原包并匹配观察到的
启动 ELF。用户确认录音取消/重进、中文预览/默认取消、退出清除预览、约十次导航、
七条最新待办、离线缓存和联网后刷新。服务端只读回执哈希未变，取消未新建任务。
临时端口消失与用户拔插 USB 一致；重连窗口含一次 USB 复位/启动，不计作无复位
测试。精确采样与限制见[设备验收](passport-device-tasks.zh_CN.md#oc-04-当前固件验收2026-10-22)。

私有证据：`/home/ywyz/code/ai-passport-oc-20261022/oc-040506-20261022/`。
没有同步副本、reset/clean、提交、推送、部署、固件刷写、整片擦除或 eFuse 操作。
保留既有原工作副本、设备/服务端配置、待办缓存和日志。

Build: PASS；Host tests: PASS。
Device tests: PASS，仅限记录的用户/日志观察子集。
Unverified：首次刷新失败根因、识别等待中取消、真机错误文案、语音时延/最低堆/DMA
峰值、写入中物理掉电及长期/混合运行。现有遥测不能关闭资源/时延缺口；翻译选择独立
保留待定。


## OC-07/05/08 测量增量复核，2026-10-22

源码、构建、测试与证据操作全部留在上述独立 WSL 原生工作副本。原生
OpenCode 2.0.22 依次执行 OC-07、两轮经复核的 OC-05 修复，再执行只读
OC-08。三个会话的导出消息元数据均确认 `opencode-go/deepseek-v4.1-flash`、
default 变体：OC-07 `ses_e9c3b2ea5ffeiS85vMLLo06ide`，OC-05
`ses_e9c64ac5fffe4KxP9lFU5hEe6C`，OC-08 `ses_e9bef4c10ffeBwlQeSneEowWJ4`。
可用图谱属于 Windows 工作副本，路径元数据过期，因此采用原生源码定向
核查，不宣称图谱已覆盖本副本。

本增量添加有界的数字化语音/下载诊断。Codex 复核复现了终结记录丢失/
序号、请求标志发布、清理边界及打开连接成功后立即取消等证据缺陷。OC-05
修复这些问题，并强化真实 worker/传输路径回归、独立假时钟/资源掩码断言
及源码副本变异检查。生产改动限于 `main/suite_tasks.c` 与
`main/suite_tasks_voice_impl.h`；测试改动限于 `tests/test_suite_tasks_runtime.c`
及其外部 I/O 桩头文件。既有请求预算、付费阶段策略、存储格式和设备配置
保持不变，未添加 ASR 自动重发。

最终 44 组 runtime 测试在默认与 `-O2` 下通过。静态与 ESP-IDF 5.5.3
完整门禁通过，包括 64 项 companion 测试且无跳过；Codex 独立验证了
不可变归档及已复核源码哈希。

- 完整镜像 SHA-256：`a353524e7c8782a75d068aef81007a21c247ff4b0daff12eefc1c00c9077d679`。
- 匹配 ELF SHA-256：`dc655e73a1242d9959f386440b00f1d320872ca7b21b7b17604046ae5745253c`。
- 应用 SHA-256：`997690fb289aa3dc5f7a1106d3fd87e7ab1f87e8239bc5a014c947dcaa471da4`。
- 应用/合并镜像大小：3,631,120 / 3,696,656 字节；归档位于 `build/firmware/` 下，以完整镜像哈希命名。
- 四文件诊断源码清单 SHA-256：`61aa7941841815fd9c5b299af38f7357df39418d28735732c6440ea984a66341`；它不是全源码树的溯源哈希。

诊断固定 RAM 为 376 字节。最终镜像比仅 OC-07 的中间版本大 736 字节。
未增加诊断动态分配、常驻任务、周期 heap 遍历、逐块日志、IDF 补丁或
局部最低值监视器。heap/DMA 是能力集合重叠的边界采样，不能相加，也
不是真实操作局部峰值；栈值是以字节计的 worker 水位。HTTP 等待返回/
状态字段不证明接纳、计费或退款。SC-10 资源余量仍只是提案。

OC-08 已准备有界测量用例及其剩余缺口。硬件桥通过内存串口清理/帧传输
检查；被动采集器通过合成解析及真实 C 格式化器兼容性检查。这些检查
没有打开 COM3，不是真机证据。新候选的精确仅应用刷写与测量交接见
[设备检查点](passport-device-tasks.zh_CN.md#oc-0708-测量候选交接2026-10-22)。
私有本地证据、字段契约、差异、红绿/门禁日志及导出元数据位于
`/home/ywyz/code/ai-passport-oc-20261022/oc-0708-20261022/`。

Build: PASS（上述精确复核归档）。
Host tests: PASS（44 组默认/优化测试、静态及完整门禁）。
Device tests: NOT RUN（本候选）；当前安装仍是 `7c292cfc/e8b6ae41`。
Unverified: 真机事件/串口行为、停止/取消/释放/清理测量、真实瞬态
heap/DMA/最大块峰值、界面耗时/渲染、写入时物理断电、长时间/混合操作
及尚未复现的原始 Refresh 原因。翻译决策独立保留。本增量未同步回原副本、
提交、推送、部署、擦除数据或写入固件。


### 已批准的测量应用写入，2026-10-23

用户已批准，Codex 已完成 COM3 上精确 `a353524e/dc655e73` 的仅应用写入。
当前分区表匹配、写入范围、设备摘要及连接关闭均通过，测量用户交互仍
待执行。详见[刷写记录](passport-device-tasks.zh_CN.md#测量应用刷写2026-10-23)。
本记录更新此前未安装状态，不改记旧镜像验收。


### 已测子集收口，2026-10-23

用户确认待办保留、录音可用及取消/重进正常。同窗口两条完整序列各终结
一次：录音取消到采集结束 117.741 ms，短按停止到采集结束 19.503 ms。
第二条随后的取消与用户澄清的操作一致。清理记录完整，未发现新的修复
问题。资源值仍为边界采样，内部空闲/最大块最小值为 42,544/23,552 字节，
worker 水位为 2,308 字节。用户要求无需过多防御性测试，因此 Codex
正常停止采集并释放 COM3。预览/远端等待耗时与真实峰值保持未验证，
不因此增加轮次。详见[实测记录](passport-device-tasks.zh_CN.md#语音实测子集与收口2026-10-23)。
Build/Host tests 仍沿用精确复核构建的 PASS；Device tests PASS 仅覆盖
记录中的刷写、用户操作与数字子集。
