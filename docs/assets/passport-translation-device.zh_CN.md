[English](passport-translation-device.md) · **简体中文**

# 翻译存储、配置与 USB 配置下发

本 T1 模块提供任务缓存非活动槽位的共享所有权、独立的加密翻译配置，以及仅用于翻译的
原生 USB Serial/JTAG 配置窗口。它已注册进构建并被真实 T1 worker 使用；麦克风采集和
供应商调用在 worker 内完成，TTS 仍未实现。现有任务缓存、日志、加密凭据、五分钟任务
刷新和分区表均不变。

当前状态，2026-10-25：

```text
Build: PASS（已注册；2026-10-24 原生完整门禁）
Host tests: PASS（下列聚焦用例）
Device tests: FAIL（OC-11 真机翻译在采集前就绪失败）。USB 配置帧已发送，固件随后加载到
有效配置；但原始电脑侧匹配 ACK 不确定，后续主机解析修复只有合成测试，因此不成立完整 USB
配置 ACK 验收。
Real provider calls: PASS（companion 侧一次获准的合成 EN->zh-CN 冒烟）；设备侧未发生供应商调用
Unverified: 硬件日志时序、擦除寿命/磨损、真实 NVS 掉电、内存/栈峰值、供应商实际保留策略
```

## 非活动槽位共享所有权

`main/suite_tasks.c` 仍是权威 `task_cache_t` 的所有者。新增的
`suite_audio_job` 模块在该对象及其活动 generation 之上增加外层锁 S，不会
打开第二个缓存，也不新增常驻工作线程。

| 阶段 | 所有权 |
| --- | --- |
| 任务刷新写入/提交 | S，然后在有界请求内取 N |
| 任务语音预检 | 仅 N |
| 任务语音采集 | S + A；上传前释放 A |
| 任务语音上传 | S + N |
| 翻译采集 | S + A |
| 翻译上传/下载 | S + N |
| 暂存清理 | 仅 S |
| 无暂存的天气/完成 | 仅 N；不得等待 S/A |

A 与 N 不会同时持有；持有 N 时不会再去获取 S/A。启动恢复与最终清理都先
擦除数据扇区，再擦除标记扇区。

租约会记录活动 generation 与选定的非活动槽位。完整任务刷新在写入到提交
期间一直持有 S，因此 generation 不会在采集或上传所有者脚下变化。槽位忙碌
或已隔离时按失败关闭，绝不复用状态不明的音频。清理失败只隔离带标记的
非活动槽位，本次启动内不再使用；启动恢复会先重试清理，之后语音或翻译才
能使用暂存。

现有暂存标记字逐字节保持不变，因此旧固件中断录制留下的标记仍会被识别并
清理。所有者、generation、请求跨度和隔离状态只存在于 RAM，不写入标记。
只要旧标记存在，清理就总是从非活动槽位基址擦除 `TASK_VOICE_ERASE`
（`0x5f000`），即使翻译阶段只请求了 `0x50000`；这是实际清理跨度，并不表示
整个生命周期只有 `0x50000`。数据扇区先于标记扇区擦除。缓存有效时恢复只
检查非活动槽位；没有有效 generation 时才检查两个槽位。恢复绝不会擦除活动
任务头、记录、日志或设置。标记读取失败不会被当作干净：清理失败、槽位保持
隔离，恢复保留该标记。

### 采集工作线程使用的公共接口

```c
#include "suite_audio_job.h"

suite_audio_lease_t *lease = NULL;
esp_err_t err = suite_audio_job_acquire(SUITE_AUDIO_OWNER_TRANSLATION, 1000, &lease);
if (err != ESP_OK) { /* 有界：报告忙碌/不可用 */ }

/* 先配置 BSP 为 16 kHz、16 位、单声道。强制至少 9,600 PCM 字节（0.3 秒），
   至多 320,000 PCM 字节（10 秒）。 */
if (suite_audio_job_stage_begin(lease, SUITE_AUDIO_TRANSLATION_SPAN) != ESP_OK) {
    /* 存储失败 */
}
while (capturing) {
    /* PCM16LE 单声道 16 kHz，每次最多 SUITE_AUDIO_CHUNK_MAX（512）字节 */
    if (suite_audio_job_append(lease, pcm, bytes) != ESP_OK) { /* 存储失败 */ }
}
if (suite_audio_job_finalize(lease) != ESP_OK) { /* 存储失败 */ }

uint8_t digest[32];
if (suite_audio_job_sha256(lease, digest) != ESP_OK) { /* 存储失败 */ }

/* S 已持有：在 S 内获取 N，分块上传，再释放 N 后回到采集/播放。
   持有 N 时绝不获取 S。 */
if (suite_net_take(1000)) {
    uint32_t offset = 0;
    while (offset < suite_audio_job_wav_bytes(lease)) {
        uint8_t chunk[SUITE_AUDIO_CHUNK_MAX];
        size_t n = suite_audio_job_wav_bytes(lease) - offset;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        suite_audio_job_read(lease, offset, chunk, n); /* 有界读取 */
        /* 将 chunk 写入请求体 */
        offset += (uint32_t)n;
    }
    suite_net_give();
}

/* 一定释放 S。返回值非 OK 表示清理失败且该槽位已隔离。 */
esp_err_t cleanup = suite_audio_job_release(lease);
```

最终规范化 WAV 最大为 `SUITE_AUDIO_WAV_MAX`（320,044）字节（含 44 字节头）。
音频不会整段驻留 RAM。`stage_begin` 接受按扇区对齐、最大
`SUITE_AUDIO_SPAN_MAX`（`TASK_VOICE_ERASE`）的跨度；`0x50000` 足以覆盖翻译
WAV 及其 4 KiB 标记。取消是协作式的：工作线程调用
`suite_audio_job_cancel()` 后仍必须调用 `suite_audio_job_release()`（或
`suite_audio_job_abort()`），由所有者释放 S 并完成暂存清理。

## 加密翻译配置

`suite_translation_config` 在现有加密 `taskcfg` 分区、命名空间
`translation`、键 `config` 中保存单个带版本 blob：

| 字段 | 含义 |
| --- | --- |
| `magic`、`version` | blob 布局标识 |
| `schema` | 翻译协议 schema；当前为 `1` |
| `origin` | 仅 HTTPS 源，不含路径、查询、片段、用户信息或端口 |
| `token` | 独立能力令牌，32-128 个 URL 安全字符 |
| `crc` | 覆盖前面所有字段的 CRC |

读取时先找到 `taskkeys`，调用 `nvs_flash_read_security_cfg()`，再调用
`nvs_flash_secure_init_partition("taskcfg", &key)`。写入前也会先初始化该加密
分区，再检查任务令牌命名空间，因此 Tasks 尚未运行的首次启动也能完成第一次
写入。它绝不擦除分区、反初始化 NVS 或生成密钥。命名空间不存在时为
`UNCONFIGURED`；无效或不可读的 blob 按失败关闭。

写入配置时先校验 origin 与令牌，证明令牌不同于现有任务 `token` 与
`write_token`，写入 blob 并提交，然后重新打开命名空间比对回读。结果明确：

| 结果 | 含义 |
| --- | --- |
| `APPLY_OK` | 新 blob 已提交并校验 |
| `APPLY_UNCHANGED` | 相同配置已存在；未写 NVS |
| `APPLY_INVALID` | 校验或令牌独立性失败；未写 |
| `APPLY_STORAGE_NO_WRITE` | 存储失败，原 blob 仍为权威值 |
| `APPLY_VERIFY_UNKNOWN` | 提交/回读结果不明；可安全幂等重试 |

容量或提交失败绝不会替换其他命名空间：任务 origin、读/写令牌、全部八条
完成记录、语音意图及其他所有键保持逐字节不变。重复写入相同配置不会再次
写 NVS。日志中不出现令牌或 origin。

工作线程应把 `suite_translation_config_get()` 的结果放入栈结构，使用后用
`mbedtls_platform_zeroize()` 清零。

## 仅翻译用的 USB 配置下发

设备没有其他运行时串口命令处理器。配置下发使用 GPIO18/19 的原生 USB
Serial/JTAG，绝不使用 UART0（其默认 TX 与 GPIO21 背光冲突）。

只有用户显式选择才打开最长 60 秒的窗口。存在共享暂存租约（采集/上传）或
可选的翻译忙碌判断报告活动工作时，窗口拒绝打开。窗口安装 ESP-IDF
`esp_driver_usb_serial_jtag` 驱动，并把现有控制台 VFS 切到该驱动以保持正常
日志，运行唯一的读取者，关闭时卸载驱动。窗口之外没有读取者，也没有已安装
的驱动。任务退出是唯一清理路径，不会遗留过期写入者。

每个窗口只接受一个有界请求 ID。帧为严格 UTF-8 JSON 行，最长 1,024 字节：

```json
{"schema":1,"operation":"set_translation_config","request_id":"<32 位小写十六进制>","origin":"https://...","token":"<能力令牌>"}
```

解析器拒绝重复键、多余或缺失字段、类型错误、非 `1` 的 schema、非法请求 ID、
非法 origin/token、原始或转义 NUL、控制字符、尾随字节和非法 UTF-8。超长行
直接丢弃且不写入。普通串口噪声无法满足帧结构。输入绝不回显。配置最多写入
一次。解析失败但唯一紧凑请求 ID 仍可安全确定时，返回 `INVALID`，而不是空 ID
或丢失错误。带已知 ID 的被拒帧会消耗窗口的一次尝试；不含请求 ID 的普通
控制台日志不消耗。相同的重复帧返回同一应答，冲突或第二个请求得到有界失败
且不写入。

应答只包含匹配的请求 ID、状态和校验标志：

```json
{"request_id":"<同一 ID>","status":"OK","verified":true}
```

状态为 `OK`、`INVALID`、`BUSY`、`STORAGE`、`UNVERIFIED`。`STORAGE` 表示原
配置仍为权威值；`UNVERIFIED` 表示提交/回读结果不明，应幂等重试。应答不会
重复 origin 或令牌。

### 主机工具

`tools/configure-translation-device.py` 从独立的私有 JSON 文件或隐藏输入读取
origin 与令牌，绝不使用命令行值。在 POSIX 上文件权限为 `0600`，内容仅：

```json
{"origin": "https://translate.example.test", "token": "<能力令牌>"}
```

```bash
python3 tools/configure-translation-device.py \
  --config /private/translation.json --port COM7 --timeout 15
python3 tools/configure-translation-device.py \
  --config /private/translation.json --port rfc2217://host:4000
```

工具以 DTR/RTS 非活动状态打开端口，在绝对有界时间内等待一个严格匹配的
应答。它会过滤启动、任务和天气日志且不打印，忽略过期请求 ID，拒绝携带自身
请求 ID 但格式错误的应答，并且不会因无关日志延长截止时间。它不打印主机名、
令牌或原始串口数据。它不烧录、不擦除、不运行 esptool，也不生成任务配置
镜像/密钥。

## 已测试范围

聚焦主机测试编译实际所有权、配置与协议源码：

- `tests/test_suite_audio_job.c`：真实缓存绑定、有界获取、规范化 WAV、
  有界追加/读取/哈希、先数据后标记的擦除、标记读取失败、擦除失败/隔离与
  旧标记兼容。
- `tests/test_suite_tasks_runtime.c`：现有任务刷新/语音回归，外加翻译租约在
  N/擦除之前阻塞刷新、取消释放、活动 generation 不变。
- `tests/test_suite_translation_config.c`：精确 blob 回读、首次使用在任务查询
  前初始化加密分区、幂等、令牌独立性、注入的密钥/写入/提交/回读失败，以及
  任务记录、语音意图和其他键的逐字节保留。
- `tests/test_suite_translation_usb_proto.c`：分片/超长/非法/重复帧、转义 NUL/
  控制字符拒绝、已知 ID 错误应答、严格应答结构、读取期间取消/超时、窗口
  边界、输入不回显。
- `tests/test_translation_device_config.py`：私有文件处理、RFC2217 打开、
  DTR/RTS 非活动、控制台日志过滤且不延长截止时间、过期 ID 与严格格式错误
  应答拒绝。

这些测试使用内存 NVS 模型与脚本化传输。它们不证明真实 NVS 掉电行为、Flash
擦除寿命、USB 时序、控制台/日志交互、内存峰值、硬件采集、供应商保留策略或
一次成功的设备烧录。
