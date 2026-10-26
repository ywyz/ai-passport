<p align="right">
  <a href="passport-translation-provider.md">English</a> · <strong>简体中文</strong>
</p>

# 翻译真实供应商与 HTTP 音频契约

更新：2026-10-25。本文描述已部署的 companion 侧真实 MiMo T1 适配器、有界音频上传、
单作业 worker、独立配额账本以及增量部署路径。它不是固件变更，也不执行刷机或进一步
付费流量授权。

已复核的供应商/保留决策仍见[翻译方案](passport-translation-plan.zh_CN.md)；
与供应商无关的记账和 mock 检查点仍见
[T1 核心检查点](passport-translation-t1-core.zh_CN.md)。

## 状态与安全边界

- 已实现、通过主机测试并部署：`companion/translation/` 的供应商/配置/服务器模块、
  带认证的规范 WAV `PUT`、单有界 worker、阶段间取消、重启 `UNKNOWN`、独立每日
  配额账本，以及本文档的中英配对。
- 部署证据：自 2026-10-23 起在 `/suite/v1/translations*` 之后运行。一次获准的合成
  EN->zh-CN 冒烟以 7.445 秒通过，产生一次 ASR 和一次 MT claim，从独立配额中消耗
  3 秒 ASR 和 1 个 MT 作业。
- 未运行：更多真实或付费调用、账单/发票核验，以及设备采集/上传。OC-11 设备侧尝试在
  准入之前就停于就绪检查，未创建作业或 claim；设备当前是只有占位页的旧构建。
- 用户已接受选定方案已披露的不确定性。实际账户的地域、数据驻留、音频训练处理和配额
  政策未经独立确认；除该已记录接受外，不声称地域或零保留保证。

## 供应商契约

端点固定为 `https://api.xiaomimimo.com/v1/chat/completions`。适配器使用 TLS
证书校验、服务端 `Authorization: Bearer` 密钥，并且不跟随重定向。密钥不会进入
设备配置、请求参数、日志或错误信息。

### 语音识别

模型 `mimo-v2.5-asr`，非流式。WAV 在发起任何调用前完成校验，并作为一个 data URL
发送。`zh-CN` 映射为 `zh`；`en` 映射为 `en`。

```json
{
  "model": "mimo-v2.5-asr",
  "stream": false,
  "messages": [{
    "role": "user",
    "content": [{
      "type": "input_audio",
      "input_audio": {
        "data": "data:audio/wav;base64,<canonical WAV>",
        "format": "wav"
      }
    }]
  }],
  "asr_options": {"language": "zh"}
}
```

### 文本翻译

模型 `mimo-v2.6-flash`，非流式，关闭 thinking，JSON 对象输出，并限制生成长度。
固定 system 指令声明用户文本是不可信数据而非指令。原文只出现在用户 JSON 对象中。

```json
{
  "model": "mimo-v2.6-flash",
  "stream": false,
  "thinking": {"type": "disabled"},
  "response_format": {"type": "json_object"},
  "max_completion_tokens": 1024,
  "messages": [
    {"role": "system", "content": "<fixed JSON-only translation instruction>"},
    {"role": "user", "content": "{\"text\":\"<source>\",\"source_lang\":\"en\",\"target_lang\":\"zh-CN\"}"}
  ]
}
```

返回内容必须恰好是一个 JSON 对象，且恰好包含以下键：

```json
{"target_text": "<complete translation>", "source_lang": "en", "target_lang": "zh-CN"}
```

`source_lang` 和 `target_lang` 必须等于请求语言对。`target_text` 必须是非空字符串，
且不超过 2,048 个 UTF-8 字节和 512 个 Unicode 标量值。重复键、非 UTF-8 字节、
未配对代理项、多余或缺失键、错误语言以及截断内容都会被拒绝，不会静默截断或用摘要
替代。

### 响应与错误处理

适配器要求恰好一个 choice、`finish_reason` 为 `stop`、`content` 为字符串。
`length`、`content_filter`、多 choice、外层 JSON 畸形、内层 JSON 畸形以及协议异常
都会被拒绝。读取使用固定 65,536 字节缓冲区。传输层使用非阻塞 socket 和单一绝对
单调截止时间，覆盖有界 DNS 解析、TCP 连接、TLS 握手、请求发送、响应头和响应体。
慢速逐段发送的响应头或响应体无法延长截止时间。重定向、非 identity 的
`Content-Encoding`、不支持的传输编码、超大或截断的响应，以及超过截止时间的响应
都会被拒绝。分块响应会在同一边界内解码。DNS 使用有界的同主机 singleflight 缓存
（正向 300 秒、负向 5 秒）；最多存在一个解析线程，它不捕获 API 密钥或请求体，
每个调用方仍使用自己的绝对截止时间。没有重试；失败的付费阶段成为终态失败。

只有净化后的枚举码会跨越边界：

| 代码 | 含义 |
| --- | --- |
| `PROVIDER_AUTH` | HTTP 401 或 403 |
| `PROVIDER_QUOTA` | HTTP 429 |
| `PROVIDER_TIMEOUT` | 超过墙钟截止时间 |
| `PROVIDER_UNAVAILABLE` | DNS/TLS/socket 失败或 HTTP 5xx |
| `PROVIDER_INVALID_RESPONSE` | 重定向、压缩、HTTP 4xx 或非法响应体 |
| `QUOTA` | 本地每日翻译预算耗尽 |

ASR 失败不保留原文。MT 失败会在剩余文本生命周期内保留 RAM 中有效的 ASR 原文。

## HTTP 契约

所有路由都要求独立的翻译 bearer。任务 reader、writer 和 admin token 会被拒绝，
翻译 token 也不会被任务路由接受。Token 不会出现在 URL 或日志中。

### 本地就绪

`GET /suite/v1/translations/ready` 是精确路由，在 job-ID 匹配之前检查。它使用相同的
token 矩阵并返回有界 JSON：

```json
{"schema":1,"ready":true,"provider":"mimo"}
```

仅当本地翻译服务已配置且 worker 正在运行时为 true，关闭期间为 false。未就绪时返回
`503 Service Unavailable` 和 `{"error":"NOT_READY"}`。这只表示本地就绪/认证，
不会调用 MiMo、消耗配额、修改元数据，也不保证供应商可用性。

### 接纳

`POST /suite/v1/translations`，`Content-Type: application/json`，且恰好包含：

```json
{"schema":1,"operation_id":"<32 lowercase hex>","source_lang":"en","target_lang":"zh-CN","format":"wav","audio_sha256":"<64 lowercase hex>","audio_bytes":9644}
```

只接受 `en` 到 `zh-CN` 和 `zh-CN` 到 `en`。匹配重放返回原接纳结果，不产生新的
供应商声明；不同载荷返回 `OPERATION_CONFLICT`。响应包括 202 接纳/重放、
409 `OPERATION_CONFLICT`、400、401、429 `BUSY`，以及关闭期间的 503
`UNAVAILABLE`。

### 单次有界音频上传

`PUT /suite/v1/translations/{job_id}/audio`

- `job_id` 恰好 32 个小写十六进制字符。
- 必须提供 `Authorization: Bearer <translation token>`。
- `Content-Type` 必须恰好是 `audio/wav`；带参数会被拒绝。
- 必须提供 `Content-Length`。缺失长度、`Transfer-Encoding`、非 identity 的
  `Content-Encoding`、短请求体或超限长度都会在任何供应商声明之前被拒绝。
- 请求体最多 320,044 字节：44 字节规范 WAV 头，16 kHz、有符号 16 位小端单声道
  PCM，至少 0.3 秒且最多 10 秒。整文件 SHA-256 和长度必须与接纳记录一致。
- 请求体按有界流式块读取；服务不会缓冲无界请求。

响应：

| 状态 | 含义 |
| --- | --- |
| `202 Accepted` | 已校验并进入有界 worker 队列 |
| `200 OK` | 匹配重放或终态；不产生新的供应商声明 |
| `400 Bad Request` | 内容类型、长度、编码或 WAV 信封非法 |
| `401 Unauthorized` | 缺失或错误 bearer |
| `404 Not Found` | 未知 job ID |
| `409 Conflict` | 音频冲突、已取消作业或过期 generation |
| `413 Payload Too Large` | `audio_bytes` 或请求体超过 320,044 |
| `429 Too Many Requests` | 有界队列或元数据容量已满 |
| `503 Service Unavailable` | 本地服务正在关闭（`UNAVAILABLE`） |

非法 WAV 在 ASR 声明之前被拒绝，且不消耗配额。匹配重放不会重复 ASR。`202` 只表示
队列已接受，不表示识别或翻译已完成。若排队的 WAV 在 ASR 声明前达到 300 秒生命周期，
持久行会变为 `EXPIRED` 并带 `AUDIO_EXPIRED`；该行保留用于去重，且不会自动重放。

### 状态与取消

`GET /suite/v1/translations/{job_id}` 返回作业视图。`DELETE` 幂等取消：

```json
{"schema":1,"operation_id":"<hex>","job_id":"<hex>","state":"DONE","source_lang":"en","target_lang":"zh-CN","generation":1,"audio_bytes":9644,"source_bytes":24,"target_bytes":27,"updated_at":1000,"error":null,"source_text":"<recognized source>","target_text":"<translated text>","text_lost":false}
```

状态包括 `ADMITTED`、`RUNNING`、`DONE`、`FAILED`、`CANCELLED`、`EXPIRED`、
`UNKNOWN`，以及仅存在于视图的 `TEXT_LOST`。`TEXT_LOST` 表示持久行是 `DONE`，但
RAM 文本已过期或进程已重启；元数据不是可恢复的翻译。`UNKNOWN` 表示重启前某个阶段
已被声明，因此不会自动重放付费阶段。`EXPIRED` 表示排队的 WAV 在 ASR 声明前超过
生命周期，并带 `AUDIO_EXPIRED`。受控关闭会以 `SHUTDOWN` 取消待处理和活动作业，
清除 RAM 文本/音频，且不允许后续付费阶段；worker 排空是有界的。

一个 worker 按接纳顺序处理作业。`PUT` 在队列接受后即返回；SQLite 读写都很短，
ASR 或 MT 期间不持有请求级全局锁。因此，即使供应商调用仍在运行，状态和取消仍可
服务，包括通过线程化 WSGI 服务器和 `gthread` 部署配置。

## 配置与部署

服务读取一个私有 JSON 文件，例如 `/etc/passport-translation/config.json`：

```json
{
  "schema": 1,
  "translation_token": "<independent 32-128 character token>",
  "database": "/var/lib/passport-translation/translation.sqlite3",
  "provider_key_file": "/etc/passport-translation/provider.json",
  "daily_asr_seconds": 60,
  "daily_mt_jobs": 6
}
```

可选键：`forbidden_tokens`、`text_ttl`、`audio_ttl`、`queue_size`、
`cleanup_interval`、`asr_timeout`、`mt_timeout`、`capacity`。

`provider_key_file` 是包含 `api_key` 的私有 JSON 文件。当翻译服务账户可读时，可以
引用现有的 `/etc/passport-tasks/speech.json`，也可以使用独立密钥文件。Tasks 文件
永远不会被修改；其中的 `daily_seconds` 不会被复用。翻译账本拥有自己的每日预算：
默认 60 秒 ASR 和 6 次 MT 作业。

增量创建配置；生成器拒绝已存在的输出、拒绝写入 `/etc/passport-tasks`、不接受
密钥参数，也不会打印密钥：

```bash
python3 tools/configure-translation.py \
  --output /etc/passport-translation/config.json \
  --provider-key-file /etc/passport-translation/provider.json
```

配置和密钥文件必须是 `0600`；父目录应为 `0700`。数据库以 `0600` 创建。
`read_config` 会拒绝符号链接、组/其他可读文件、未知键和相对路径。

部署工件是增量的：

- `companion/deploy/passport-translation.service` 以 `passport-translation` 用户
  运行单进程，使用 `StateDirectory=passport-translation` 和
  `ReadWritePaths=/var/lib/passport-translation`。
- `companion/deploy/gunicorn-translation.conf.py` 绑定回环
  `127.0.0.1:8788`，使用一个 worker 和四个请求线程。
- `companion/deploy/caddy-translation-route.example` 在现有 Tasks 兜底路由之前
  将 `/suite/v1/translations*` 路由到 `8788`。

本地检查使用线程化标准库服务器：

```bash
python3 -m companion.translation --config /etc/passport-translation/config.json --port 8788
```

配置生成器不会写入 Tasks 配置、NVS、密钥分区或现有语音文件。增加翻译不会禁用
Tasks、天气或配网。

## 边界与保留

| 资源 | 边界 |
| --- | --- |
| 规范 WAV | 320,044 字节；16 kHz PCM16 单声道；0.3-10 秒 |
| 认证请求体读取 | 8,192 字节流式块 |
| Worker 队列 | 默认 8 个作业 |
| 元数据行 | 默认 64；新接纳返回 `BUSY`，不驱逐 |
| RAM 原文/译文 | 120 秒；调度器独立于请求清扫 |
| RAM 排队音频 | 300 秒；未声明时 `EXPIRED`/`AUDIO_EXPIRED`，阶段完成或取消后移除 |
| 每日 ASR 预算 | 默认 60 秒 |
| 每日 MT 预算 | 默认 6 次作业 |
| 供应商响应缓冲 | 65,536 字节 |

原文、译文和音频不会进入 SQLite、任务日志、请求日志或备份。元数据账本只保存标识、
哈希、字节数、状态、generation 和时间。过期会移除 RAM 文本/音频，但保留持久化
去重行；它绝不会触发自动付费重放。配额每个 UTC 日一行，较旧行会被修剪。

## 验证

```bash
PYTHONDONTWRITEBYTECODE=1 python3 tests/test_companion_translation.py
PYTHONDONTWRITEBYTECODE=1 python3 tests/test_companion_translation_real.py
./tools/test-companion.sh
```

真实适配器测试套件只注入外部 HTTP 传输和时钟，并执行真实的服务、worker、供应商、
WSGI 路由、线程化服务器和配置生成器。它覆盖两种请求形状、非法音频在声明前拒绝、
401/403/429/5xx/重定向、超时/慢读/超大/压缩响应、外层和内层 JSON 畸形、重复键、
未配对 Unicode、`length`/`content_filter`、MT 失败保留原文、声明前及 ASR/MT 之间
取消、重启 `UNKNOWN`、零调用匹配重放、独立认证与配额、无需 API 请求的调度器过期，
以及错误中不出现密钥或原文哨兵。

```text
Build: NOT RUN（仅服务端服务；无固件变更）
Host tests: PASS（保留 18 个 + 43 个供应商/服务/配置测试）
Device tests: FAIL（真实翻译就绪失败；设备未创建作业或 claim）
Deployment: PASS（自 2026-10-23 起运行）
Real provider: PASS（一次获准的合成 EN->zh-CN 冒烟）；付费账单与发票核验 NOT RUN
Unverified: 账户条款/地域/保留、更多付费调用、设备采集/上传与固件集成、串口与刷机
```
