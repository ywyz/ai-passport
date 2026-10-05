[English](passport-suite-contract.md) · **简体中文**

# 五合一协议与数据约定

## 1 协议状态与公共边界

2026-10-05，Asia/Shanghai，核对基线 `0b9e4c81ee4421c0bac39ca3561d65a8285acd4a`。依据及 U/D/C/V 见 [PRD](passport-suite-prd.zh_CN.md)，映射及决策见 [prompts](passport-suite-prompts.zh_CN.md)。SC-01 至 SC-10 均是实现 U 要求的 D/V 建议，本协议、模型、应用缓存与安全机制均不是 C。实施前需评审和实测数值；修改已定上限需同步双语、测试与能力声明。

所有文字为合法 UTF-8；以下长度为编码字节，不含结尾 NUL（C 需另留一字节）。拒绝无效 UTF-8、重复 JSON key、无效枚举、整数溢出、超过 8 层嵌套及未知必需字段。不静默截断权威字段、ID 或票码。UUID 为规范小写字符串；JSON 版本号以十进制字符串表示 uint64，避免浏览器精度损失。哈希为小写十六进制 SHA-256。v1 不接收压缩网络负载；读入时即检查上限，不先分配；检查 Content-Type 和长度但不单独信任它们。

公共 HTTPS 封套：`{api:"suite/1", request_id, operation_id?, data}`；响应 `{request_id,status,revision?,error?,receipt?}`。请求 ID 每次尝试新建，操作 ID 在同一变更重试时保持不变。同操作 ID 携带不同内容返回 CONFLICT。服务器按所有者/设备/资源授权。列表使用有界不透明游标，每页 20 条，不将无限集合读入设备 RAM。票据/任务页 ≤32 KiB，设置/状态 ≤4 KiB。分页片段不能冒充完整快照。

错误为 `{code,retryable,retry_after_ms?,safe_detail}`，不含凭据/音频/种子。公共码：BAD_FORMAT、TOO_LARGE、UNSUPPORTED_VERSION、UNSUPPORTED_FIELD、UNAUTHENTICATED、FORBIDDEN、PHYSICAL_CONFIRM_REQUIRED、EXPIRED、CANCELLED、DISCONNECTED、TIME_UNTRUSTED、WIFI_AUTH、TLS_CERT、NETWORK_TIMEOUT、BUSY、NO_SPACE、CORRUPT、CONFLICT、NOT_FOUND、PROVIDER_ERROR、UNSUPPORTED_CODE。HTTP：400 格式、401 认证、403 权限、404 缺失、409 冲突、412 ETag 前提不满足、413 超大、429 限流/忙、503 服务不可用。可重试由应用决定，不能据此无操作 ID 重放破坏性动作。

版本：BLE 传输 major 1/minor 0，应用 `suite/1`，模型 `schema=1`。major 不兼容在变更前拒绝；协商 minor/能力，取双方上限较小值。仅验证后忽略未知可选新增字段，未知必需特性拒绝。固件遇到较新保存格式进入只读/恢复模式，不擦除或降级。迁移写入新的校验代次，提交前保留旧代次。

## SC-01 浏览器与 BLE 传输

实现 SU-02。浏览器支持暂定：候选为 Windows/macOS/ChromeOS 的 Chrome；Linux 需单独验证配置，不直接标支持。记录 OS build、浏览器版本、适配器/驱动、HTTPS origin 和 flags。测试 `isSecureContext`、`navigator.bluetooth`、用户点击选择器、拒绝/取消、服务访问、写入前订阅通知、20 字节回退分片、刷新后权限、断连重连、实体确认，再将该组合标支持。[Chrome 官方说明](https://developer.chrome.com/docs/capabilities/bluetooth)解释安全上下文与用户手势，不代表设备认证。

不可用时 Q-01 评审电脑原生 BLE 桥/桌面 helper，采用 origin 白名单、本地明确同意并复用相同认证协议；USB 电脑配网 helper 为第二候选，需另设计设备传输。均未实现，不替换为手机或旧 SoftAP。远端网页服务器不能直接使用电脑 BLE 适配器。

建议私有 UUID（仅标识）：service `8c6f0001-6b54-4ec8-9fd1-318f72a90100`；control-write `8c6f0002-6b54-4ec8-9fd1-318f72a90100`（write with response）；event `8c6f0003-6b54-4ec8-9fd1-318f72a90100`（notify）；public capability `8c6f0004-6b54-4ec8-9fd1-318f72a90100`（read ≤128 字节，必要时也分片）。只接一个 central/session，无种子读取特征。先订阅再 HELLO。未认证仅允许 HELLO、AUTH_EXCHANGE、STATUS_PUBLIC、CANCEL；公开状态不含账户/SSID/种子。

帧值长度 `min(ATT_MTU-3, negotiated_max, 180)` 字节，回退 20。固定 12 字节小端头：u8 major、u8 kind（DATA=1、ACK=2、ABORT=3）、u16 会话 tag、u32 消息 ID、u16 分片序号、u16 分片总数。DATA 拼成逻辑对象 ≤4,096 字节，含加密开销；片数 ≤512。AUTH_EXCHANGE 可明文但不含设置/种子，此后应用消息均认证加密。非法片数/序号、会话 tag、超长均终止重组。每方向一个 ≤4 KiB 重组对象，只一个在途分片，不乱序组装。ACK 用相同标识/序号/总数，无负载；ABORT 含有界错误码。

ACK 时限 2 s，相同片最多重发两次（共三次），对象时限 120 s，未认证握手 60 s，闲置会话 120 s，实体配网窗口绝对 180 s。重复片应答但不重复追加。最后片 ACK 仅证明传输，不证明授权/保存，必须收到应用 RESULT。伪造/丢弃传输 ACK 可能造成拒绝服务，但不能伪造认证 RESULT。完成的重复消息 ID 经认证验证后回已有结果，会话保留最近 16 个结果；会话内消息 ID 不回绕。

加密 record 线格式建议：u8 encoding=1、u64 小端 counter、ciphertext、16 字节 GCM tag，25 字节开销计入 4,096 字节上限。明文 HELLO/AUTH 用 encoding=0 加有界 JSON，不能作为加密应用命令接纳。HELLO 声明传输/模型版本、权限、帧/对象上限、随机会话标识。AEAD 对象关联规范消息头（major/session tag/message ID/count）、方向和 transcript hash，不关联每片变化的 index。AUTH 结果及全部变更 RESULT/ERROR 必须密钥确认/认证。未认证 CANCEL 只能终止当前未认证握手，不能取消已提交/已认证操作。

加密逻辑消息：`{v:1,session_id,request_id,operation_id?,type,payload}`，按 type/result 关联。CANCEL 指向请求/操作，连接时是新的认证对象。断连丢弃部分重组/会话密钥、停止候选配网、保留已提交数据；重连重新认证/实体开窗，STATUS_QUERY 查询持久结果，不能用旧密钥/计数继续密文流。OTP 导入重连后仍需独立确认能力。

## SC-02 认证、配网与绑定

实现 SU-02/03/12。安全建议 Q-02 在使用凭据/种子前需密码学评审。威胁包括附近冒充/重放、恶意网页脚本、设备/存储被盗、备份服务器失陷。网页 origin 信任单独要求：HTTPS、第一方隔离导入页、严格 CSP、无第三方脚本/分析、清理 UI 文本。JavaScript 不能保证浏览器内存擦除；完成/取消清引用和输入，不将明文种子写 localStorage、IndexedDB、service-worker cache 或遥测。

候选会话：P-256 ECDH 临时密钥、32 字节随机 nonce、设备持久 P-256 身份签名密钥、签名 transcript（版本、角色、双方临时密钥/nonce、设备公钥、权限及 origin）、SHA-256/HKDF 方向密钥、AES-256-GCM 16 字节 tag。双方随机 nonce/临时公钥哈希先 commit/reveal，再导出短认证串，避免后发者搜索匹配 SAS；Q-02 必须明确规范 transcript 字节及公开负向向量。电脑和设备显示六位 SAS 与身份指纹；用户比较后两端明确确认。名称/UUID/浏览器授权都不是认证。首次是实体 SAS 的 TOFU，不是厂商证明；浏览器/所有者记录保留认可设备公钥指纹。后续身份变更阻断，需重新配对评审。设备通过同一确认 transcript 和所有者绑定 token 验证浏览器会话；所有权 bearer token 不能替代首次实体确认。每实体窗口最多三次确认失败，失败关闭。双方密钥确认前不传秘密。

AEAD nonce：每方向四字节会话随机前缀加 u64 单调计数，方向密钥不同；头/角色/版本/会话/transcript hash 为关联数据。重启不复用密钥/计数；重复消息仅重发完全相同密文。认证失败立即销毁会话。SAS 概率和实体密钥保护尚未验证，评审/实测前不能标生产安全。

状态：CLOSED → 菜单实体确认 → WINDOW_OPEN → AUTHENTICATING → AUTHENTICATED_SETTINGS → CANDIDATE_TEST → COMMITTED；或 CANCELLED/FAILED → 旧 COMMITTED/CLOSED。流量不延长窗口。WIFI_STAGE 含操作 ID、SSID ≤32 原始字节以 Base64 编码、密码 ≤64 字节（WPA2/WPA3/开放网络策略 Q-03）、服务器 origin ≤256 字节、绑定 token ≤512 字节。验证但不暴露密码。WIFI_TEST 时限 45 s，包含关联/DHCP/验证服务器健康；CANCEL 恢复旧网络。成功 TLS 测试和明确 WIFI_COMMIT 前仅内存保存候选；提交以校验事务持久保存。提交前掉电/取消/错密码/超时/断连均保留旧可用配置，不因认证失败清旧凭据。

命令：WIFI_STAGE/WIFI_TEST/WIFI_COMMIT/WIFI_CANCEL；BIND_PREPARE/BIND_COMMIT/BIND_STATUS/REVOKE；SETTINGS_PATCH/STATUS_QUERY。stage 回应不代表提交。BIND_PREPARE 浏览器获取所有者授权的单次 token（180 s），绑定设备身份指纹、服务器 origin、操作 ID。设备经 TLS 验服务器身份，对服务器 nonce 证明身份私钥持有，兑换 token。服务器先持久保存唯一 `(owner_id,device_id)` 绑定和操作结果，再签发受限设备凭据；相同 token/操作重试返回同一绑定，不同所有者返回 CONFLICT。过期返回 EXPIRED，不自动转移；丢响应由签名 BIND_STATUS 查询。普通 BLE 设置不能无新实体授权替换服务器身份或接管所有权。

撤销：所有者可经独立网页登录撤销，设备离线也可执行；服务器立即拒绝新请求，撤销刷新凭据并记录 epoch。设备收到经验证的拒绝/本地明确确认后标未绑定并删除服务器凭据；不静默擦除本地票据/待办/OTP。离线缓存查看与在线撤销分开。轮换凭据用版本化事务，不变更所有者，不记日志。Nextcloud 凭据只进服务器秘密存储，不进普通 BLE 设置。

## SC-03 时间、TLS 与 Wi-Fi 重连

实现 SU-03/09/12。不得关闭证书链、主机名或有效期检查；按批准服务器明确选择 CA bundle/私有 CA。[ESP-TLS 5.5.3](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-reference/protocols/esp_tls.html)支持配置可信锚。浏览器时钟和未认证 SNTP 仅为提示，不建立 OTP 信任。

Q-04 首次 TLS 初始化建议：固件携带独立分发的时间服务签名公钥；用全新 32 字节 boot nonce 请求初始化端点，不携凭据。仅接受签名的 `{schema,nonce,utc,uncertainty,key_id}`，核对 nonce、签名、合理固件最低日期、请求 RTT ≤2 s。公开签名对象可经明文传输，但不认证普通服务，也不放宽 TLS。签名验证后设时间，再完整验证 TLS/绑定。固定公钥轮换和初始化服务不可用的恢复需评审；普通服务器 URL 设置不能替换信任根。以后启动仍获取新时间，保存的 UTC 仅为防回退下界，不能计算断电经过时间。签名时间与单调计时建立运行期误差估计。

时间状态 `{UNKNOWN,HINT,TRUSTED,STALE}`，含来源、同步 UTC、单调参考、uncertainty_ms 和持久下界。回退或非预期 >2 s 跳变使 OTP 暂停，等待可信重同步。建议每 6 h 同步；按实测振荡器漂移和睡眠行为增加误差；仅 uncertainty ≤2 s 且同步年龄 ≤24 h 允许 OTP。未测漂移时，不能凭这些数值宣称离线可信。上电重置为 UNKNOWN，当前板级约定没有电池日历时钟保证；[IDF 时间文档](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-reference/system/system_time.html)说明重启/睡眠 timer 行为。首页可显式显示不可信时间；UNKNOWN/HINT/STALE 不显示验证码。

重连监督状态 DISABLED/CONNECTING/ONLINE/BACKOFF/AUTH_FAILED/REVOKED。关联 20 s、请求连接 10 s、读取闲置 10 s、普通请求总时限 30 s。退避 1/2/4/8/16/30 s 加 ±20% 抖动；五次失败后等待 5 min 或用户刷新。凭据/认证/证书失败停盲重试，显示明确原因。恢复可信连接/时间后才发送持久队列。BLE 配网与大票据下载、语音分开调度，遵循 [IDF 5.5.3 共存](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-guides/coexist.html)再测真实无线重叠；SDK 支持不代表性能通过。

## SC-04 页面与后台状态所有权

实现 SU-04。页面 reducer 接收 `{page_generation,gesture_id,request_id,event,data_handle}`，切换开始即递增代次。ENTERING → READY → LEAVING → STOPPED，前一停止确认前不重入。长按消耗先于导航；新 PRESS 建下一手势代次，忽略旧 CLICK/DOUBLE，包括换页后的事件。溢出时独立保留 LONG 消耗和取消，移动可合并。主机注入 PRESS/LONG/CLICK 顺序、重复 LONG、缺事件、快速双击、满队列。

worker 接收取消 token，不拿页面指针；完成模型引用稳定存储/模型 handle。UI 只接匹配代次/请求结果。后台同步用独立持久操作 ID，退页后继续；退出取消视图订阅，完成不能复活页面。音频/页面 timer 建议 2 s 内停止；若 BSP 阻塞 PCM 超时，保留所有者/对象，显示重试停止。停止超时不是释放在用资源的许可。BSP 若新增通用超时/取消 API，需单独公共约定、故障注入和实测停止验收。

仅单一音频仲裁者配置/读写/休眠 codec。后台经存储所有者写模型，不操作 LVGL；UI 所有者渲染，任何非 LVGL 任务使用 LVGL 持 `bsp_lvgl_lock()`，不得跨网络/Flash/PCM I/O 持 UI 锁。完成意图日志上限 64 条，输入深度 16，模型/事件队列深度 8，可合并最新状态，但不能静默丢持久操作回执。数量均为待测预算。

## SC-05 票据模型、发布与原始码

实现 SU-05/06。公共记录：`{schema:1,id,revision,type,title,source,review,updated_at,fields,missing,code,tombstone}`。ID 为服务器发的稳定 UUID，不从标题/日期/订单重建。类型 train/flight/performance/other。`source={kind:manual|paste|issuer,reference?,observed_at}`，人工修正保留来源。`review={state:draft|reviewed|published,reviewed_at?,reviewer_id?}`，设备仅接 published。字段可 null，同时 `missing[field]=not_provided|uncertain|not_applicable|redacted`，不填零或虚构座位。title ≤256 字节，单文本 ≤256，说明 ≤2,048，整条元数据 ≤8 KiB；本地最多 100 张，总元数据 ≤800 KiB。

时间：`{kind:instant|date|range,local_date,local_time?,tzid?,utc?,end?}`。instant 必须完整 ISO 8601 本地日期/时间、IANA 时区和明确 UTC offset/UTC 时刻；DST 歧义未解决则拒绝，不能根据出发时间猜到达日期。全天保持日期，附场所时区上下文，不转 UTC 午夜。出发/到达按各自当地时区显示；有效范围明确边界（`start_inclusive=true,end_inclusive=false`）。`updated_at` 为带 offset 的 UTC。每次发布修正版本递增，删除发布 tombstone。

| 类型 | fields（来源提供时显示） |
| --- | --- |
| train | service_number、travel_date、boarding_station、alighting_station、单独 origin_station、departure、arrival、carriage、seat、seat_kind（assigned/unreserved/standing）、class |
| flight | flight_number、departure_airport/terminal、arrival_airport/terminal、departure、arrival、boarding_at、gate、seat；gate/boarding 分别有来源和 observed_at |
| performance | event_name、venue、address、session_at、area、row、seat、standing |
| other | name、ticket_kind、validity、venue/address、entry_instructions、来源 seat |

网页接口：POST `/suite/v1/tickets/drafts` 手动/粘贴；PATCH 草稿用 If-Match 版本；POST `/{id}/review`；POST `/{id}/publish` 携操作 ID 与审核版本。审核后改动不能未经重审发布。GET `/suite/v1/tickets/snapshot?cursor=...` 返回 manifest 版本及分页 ID/hash；GET `/{id}/revisions/{revision}` 与 `/code` 返回有界不可变对象。设备所有分页固定同一 manifest；过期重启快照，不混代次。下载到临时存储，校验格式/长度/hash/来源及全部必需对象，再原子提交 active manifest。错误/掉电/空间满均保留旧有效版。本地清缓存与服务器删除分开，删除需审核 tombstone 回执。浏览/展示票码不产生核销或已使用状态。

票码对象：`{origin:issuer,kind:qr|barcode|image,mode:static|dynamic|identity_bound,payload_encoding:bytes|utf8,original_payload?,original_image?,symbology?,valid_from?,expires_at?,source_reference,sha256,display_status,reason?}`。只用发行方原始字节/验证后的发行方原图，不用订单/座位/车次生成通行数据。仅语义验证后可重编码完全相同的原始 QR 字节，否则原图。负载 ≤1,024 字节，原图编码 ≤64 KiB，解码尺寸 ≤512 × 512；流式解码/一位缓存，不分配完整 RGB 帧。缓存票码合计 ≤1 MiB。未知/动态/身份绑定离线有效性不承诺通行；动态支持需 Q-06 发行方刷新/认证规则与在线过期核查。详情仍显示票码原因 absent/expired/unsupported/too_dense/unverified。

渲染：停止无关动画；240 × 320 圆角屏内 216 × 216 方形。QR 模块数 N，四边白色静区至少四模块；整数缩放 `s=floor(216/(N+8))`，建议最小 s=3，即 N≤61。放不下拒绝，不插值/截静区/虚报扫码成功。[DENSO 指引](https://www.qrcode.com/en/howto/code.html)规定四模块边缘。条码最小模块宽度/静区依制式另定义与验证，不任意旋转缩放。几何/静区未知原图标未验证，不能通过票码验收。亮度/对比度和真实读码为独立 SA-09，不以元数据/编码/布局主机测试替代。扫码不代表发行方入场规则通过，也不替代官方身份证明。

## SC-06 Nextcloud CalDAV 与离线完成

实现 SU-07/08。由服务器适配器而非设备使用 CalDAV。发现认证 principal/current-user-principal、calendar-home-set，以 PROPFIND 读取集合及 supported-calendar-component-set 的 VTODO 支持，再 calendar-query REPORT，必要时 calendar-multiget。保留 collection href/凭据账户/ETag。选择保存明确 collection ID，不依赖显示名。[RFC 4791](https://www.rfc-editor.org/rfc/rfc4791)定义发现/report，[RFC 5545](https://www.rfc-editor.org/rfc/rfc5545)定义 VTODO/时间。[Nextcloud Tasks](https://github.com/nextcloud/tasks)是真实目标，Q-07 记录部署的 Nextcloud/Tasks 版本。WebDAV 文件备份不能代替 CalDAV。

模型：`{schema:1,account_id,collection_id,href,uid,recurrence_id?,etag,summary,status,percent_complete,completed_at?,due,note,modified_at?,snapshot_revision,downloaded_at}`。身份 `(account_id,collection_id,uid,recurrence_id)`；href 为定位，UID 单独不保证全局唯一。重复任务需真实版本验证，不通过简化条目完成整个重复系列。服务器保存原始 VCALENDAR/VTODO，保留未知属性、提醒、重复/子任务关系和折行。未验证实例/共享列表明确只读。

`due={kind:none|date|zoned|utc|floating,value,tzid?}`：date 为全天，不虚构时间；none 排在有日期之后；floating 保留无时区并明确标记，策略另评审。服务器解析 TZID/VTIMEZONE，返回原始及标准显示值，不丢来源时区。标题 ≤512 字节，备注预览 ≤4,096 字节按 UTF-8 边界截，超长设 `note_truncated=true,original_bytes`，服务器保留原文；设备详情分页，不用巨型 label。单任务 ≤8 KiB，页 ≤32 KiB，缓存 ≤200 条/1 MiB；集合 ≤20 个/8 KiB。超限明确提示选择/过滤/分页，不静默漏任务。24 h 后缓存过期，仍可读；离线完成携旧 ETag，可能冲突。

设备 API：GET `/suite/v1/task-collections`、PUT `/task-selection` 携版本、GET `/tasks/snapshot`、POST `/task-completions`、GET `/operations/{operation_id}`。刷新状态 FETCHING/READY/EMPTY/STALE/AUTH_FAILED/READ_ONLY；服务器连接与设备下载快照分别回执。Nextcloud 401/403 停重试，要求私下修复认证，不在设备错误显示密码。

完成意图 `{operation_id,identity,base_etag,desired:completed,created_at?,device_sequence,payload_hash}`。先设备日志持久接纳再显示待同步，徽标 pending 不代表远端成功。队列 ≤64，满/空间不足显示未保存并保持原状态。NEW → LOCAL_DURABLE → SERVER_DURABLE → REMOTE_APPLYING → SUCCEEDED 或 CONFLICT/FAILED；重启用同操作 ID。服务器事务保存操作和幂等结果后才 SERVER_DURABLE。完成时读取当前资源，精确比较 base ETag，只修改选中 VTODO 的 STATUS=COMPLETED/PERCENT-COMPLETE=100/可信 UTC 的 COMPLETED 及必要修改元数据，保留其余组件/属性；PUT 携 `If-Match`，禁止无条件 PUT。412/ETag 变更即 CONFLICT，其他字段变化也冲突；显示刷新远端内容，用户重确认后以当前 ETag/新操作 ID 发起。资源缺失返回 NOT_FOUND，不重建。

PUT 成功但响应丢失时重新读取：目标已完成且记录意图无相反较新状态，可报目标达成而不再写；变化/歧义进入冲突。无服务器证据不能宣称远端严格一次执行。相同操作/hash 返回已存结果，同 ID 不同 hash 冲突。临时重试 2/5/15/60/300 s 加抖动，每会话最多五次后显式暂停至重连/手动刷新；日志跨重启保留。认证/前提失败不盲重试，不覆盖远端恢复未完成任务。设备恢复未完成待 Q-07，不实现。远端执行前取消写持久取消意图；执行后取消太迟，不能暗示恢复未完成。快照更新独立保留未解决意图，不清队列。

SA-10 使用模拟发现/ETag/掉电；SA-11 单独要求真实 Nextcloud 发现/读取；SA-12 单独要求真实完成、重启/离线重放、网页并发修改。模拟成功不能满足真实验收。

## SC-07 首页场景与天气

实现 SU-09。ST-03 验收后首页为默认启动。程序三体风格场景是示意而非天文预测：三个归一化质量/位置，软化平方反比力，固定步积分，每模拟步最多 20 ms，每渲染帧最多四步，限制坐标/速度，发散时确定性种子重置。主机检查有界/确定性/无 NaN。渲染最多 10 FPS，隐藏暂停；时间文字 1 Hz 更新，不用整屏 RGB 帧。功耗/流畅度须实测，不从数值推定。

天气 `{schema,city_id,city_label,tzid,observed_at,fetched_at,expires_at,temperature_c,condition,provider,revision}`，label ≤128 字节，对象 ≤4 KiB。网页明确选择城市/时区，不隐含 GPS/手机定位。服务商/署名/许可见 Q-08。建议每 30 min 刷新，2 h 或服务商更早 expiry 后过期；失败保留旧记录并显示年龄，空为不可用。时钟可信和天气时效分开。天气错误不阻塞功能页、本地可信时钟、票夹/待办/OTP。HTTP 用 SC-03 时限和后台所有者，场景/时钟 timer 不做网络 I/O。

## SC-08 语音翻译与字体

实现 SU-10/11。格式建议：PCM 有符号 16-bit 小端、16,000 Hz、单声道，流式块无 WAV 头。最长 10 s =320,000 字节，最短 0.3 s。只有 ONLINE、可信 TLS、服务器会话接纳后 OK 开始，再次 OK 结束；UP/长 OK 取消。方向 zh-CN→en 或 en→zh-CN，不自动识别。每块 1,024 字节（512 samples/32 ms），八块 RAM ring =8 KiB，加录制/播放临时区 ≤4 KiB。不能用 demo 96 KB 整段分配，也不能 RAM 保留 320 KB。服务器暂存有界音频，设备不保存 Flash 音频历史。

POST `/suite/v1/translations` 建作业 `{operation_id,source_lang,target_lang,format,limit_bytes}`；PUT `/{job}/audio/{sequence}` body ≤8 KiB，带块 SHA-256，按序应答 offset；POST `/{job}/finish` 携总字节/hash。相同序号/hash 重复应答同 offset，不同内容冲突。ring 溢出/背压 >250 ms 则 BUSY 终止整作业，不丢样本后伪装完整录音。必须有预算地持续读 PCM；HTTP 批量跟不上则降低经批准录音上限或评审压缩格式，不扩大至无限缓冲。服务器作业绝对寿命 120 s，部分上传不自动触发付费识别。DELETE `/{job}` 幂等取消，但不承诺已发生服务商费用可撤销。

READY → RECORDING/UPLOADING → FINALIZING → RECOGNIZING → TRANSLATING → SYNTHESIZING → RESULT → PLAYING；或 CANCELLED/ERROR → READY。每忙状态 UP/长 OK 可取消，代次过滤迟到结果。上传连接 10 s、闲置 5 s、录音 10 s；识别 20 s、翻译 15 s、合成 20 s、录音后总流程 60 s。返回无秘密单调进度。仅同作业查询/幂等上传可重试；finish 结果不明不能自动新建付费作业。离线不新录音，中途断网明确取消，不能暗示离线翻译。

结果 `{job_id,source_text,target_text,source_lang,target_lang,provider_states,audio?,expires_at}`。每段文字 ≤2,048 字节且 ≤512 Unicode scalar，按字符边界验证。超限 TOO_LARGE，可保留明确标注的有效源文，不用摘要冒充翻译。源/译文独立标注滚动区。合成失败仍可展示文字并提示语音不可用。播放 PCM 16 kHz/16-bit/mono、≤15 s/480,000 字节，经同一有界 ring 流式播放；OK/UP/长 OK 经音频所有者停止，建议 ≤2 s，须实测 BSP 延迟。音频 URL 必须授权、5 min 过期，不是公开永久链接。

Q-09 适配边界：识别 有界音频→源文；翻译 明确语言对→译文；合成 文本→有界音频。真实适配前确定供应商/API 版本、区域、流式能力、上限、费用封顶、取消语义、数据保留和许可。模拟适配标 `provider_mode=mock`；SA-14 测模拟/边界，SA-15 要求真实双向语音/文字/播放，不能把 fixture 当真实服务。

保留建议：设备音频仅内存，结束/取消/错误后清理释放；文字仅当前页面会话，退出/重启清理，无历史。服务器原音/合成暂存取消或成功取回后删，绝对 TTL 5 min，重启清孤儿；源/译文负载不进请求日志/备份。不能假定服务商删除政策，Q-09 必须批准，否则阻断实际用户音频；匿名时长/错误指标也需另行批准。

中文字体：使用许可清楚、版本固定 CJK 字体，包含全部静态 UI 与声明动态字集（候选 ASCII/标点/CJK U+4E00–U+9FFF，容量待 Q-05），正文 16 px、标题 20 px。明确应用于 label/菜单/弹窗/状态。服务器报告不支持码点，设备明确提示不支持字符、网页保留原文，不静默删字符/关闭占位。验证逐码点覆盖、已知缺字负例、UTF-8 换行/截断、转换器/许可及[字体接入](../development/engineering/lvgl-chinese-fonts.zh_CN.md)。静态子集不能宣称任意姓名/译文覆盖。候选完整字集放不下则明确缩小或另评审有界字体交付设计，不能先宣称支持。SA-16 真机逐页观察长文本、中英文混排。

## SC-09 OTP 密库、权限与恢复

实现 SU-12/13。本轮已完整提出详细设计供评审，机制均 D/V。候选 TOTP：T0=0，counter=floor(UTC/period)，HMAC SHA-1/SHA-256/SHA-512、动态截取、十进制补零；6/8 位，周期 30/60 s，默认 SHA-1/6/30。v1 不含 HOTP，未知类型/算法/周期/位数明确拒绝，不静默替换。[RFC 6238](https://www.rfc-editor.org/rfc/rfc6238)提供算法/公开向量。URI 用 [Google Key URI 格式](https://github.com/google/google-authenticator/wiki/Key-Uri-Format)，不意味着所有验证器支持全部参数。虚构账户与独立向量交叉验证，含前导零/周期边界；实际账户兼容性见 Q-10。本轮不索取/保存真实种子。

网页本地解析 `otpauth://totp/...` 或本地 QR 图，解 Base32/percent，验证发行者 label/query 一致，拒绝重复/冲突参数，导入前显示发行者/账户/算法/位数/周期。URI ≤2,048 字节，种子解码 10–64 字节，issuer/account 各 ≤128 字节，最多 32 账户。URI 不进 URL/网络/日志。密库 `{schema,id,revision,issuer,account,algorithm,digits,period,encrypted_secret,created_at?,deleted}`；随机稳定 ID，按操作 ID 幂等导入，标签相同不证明种子相同。元数据本身也需解锁 OTP 权限。

普通 AUTHENTICATED_SETTINGS 无 OTP 权利。本地解锁密库加新实体菜单确认建立 OTP_IMPORT，绑定会话 transcript、60 s 过期、只允许一个审核账户。OTP_IMPORT_PREPARE（仅元数据）、OTP_IMPORT_COMMIT（加密种子）、OTP_IMPORT_STATUS、OTP_DELETE、OTP_BACKUP_OPEN/CHUNK/FINISH、OTP_RESTORE_PREPARE/COMMIT 只在独立受限单次权限内。空密库经实体确认进入 OTP 设置，只授元数据 PREPARE，无既有解锁要求；PIN 设置和单次权限前仍禁止种子 COMMIT。首个导入需先设本地 PIN/恢复策略，再持久提交秘密，用户核对虚构向量。提交前断连/取消清暂存种子；丢提交响应在新授权会话查操作状态，不盲重复插入。普通设置 RPC、票据导出/状态无种子字节，不提供普通明文读出命令。

本地解锁建议：8 位 PIN，上下逐位 0-9，OK 下一位，短暂显示后掩码，长 OK 取消。PBKDF2-HMAC-SHA256，每库 salt ≥16 字节，候选 600,000 次导出 wrap key，解开随机 256-bit data key；C3 实测后选工作因子/UI 延迟（Q-10）。AES-256-GCM 保存种子，每记录唯一 96-bit nonce，账户 ID/schema/revision 为关联数据。回复前持久保存失败次数；五次错误锁 5 min，指数延长至 1 h，重启不清计数/绕过等待（时间不可信需认证恢复/解锁策略）。闲置 60 s、退出 OTP、启动均锁定，清解密密钥/种子/验证码 label，暂停显示。SC-03 非 TRUSTED 范围不显示码；可信时间内离线不需服务器登录。

PIN 熵有限，软件限流不能防复制 Flash 离线穷举或恶意固件。真实种子使用由 Q-10 阻断，直至物理存储/密钥保护、secure boot/Flash/NVS 加密及锁定评审完成。[ESP-IDF 5.5.3 Flash 加密](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/security/flash-encryption.html)涉及设备安全配置，此约定不授权 eFuse/不可逆启用。设备身份私钥保护也纳入此评审。公开向量/虚构账户可测软件，但不能宣称抗物理攻击。

删除需本地解锁和实体确认，先提交 tombstone/manifest，再删加密记录、清工作缓冲、撤销 capability。Flash 磨损均衡可能留旧密文，不承诺取证擦除。旧备份可恢复已删账户，明确提示，可独立授权清理备份版本。升级迁移保留密库 UUID/data key wrap/验证后的密文，不以恢复出厂修复格式。布局变更刷写前导出加密恢复并核对兼容。0x0 合并镜像可能覆盖保存区；分段保留需布局/目标评审，不能默认保证。

独立恢复建议：电脑 Web Crypto 生成 32 随机字节恢复 key，用户在原设备和服务器之外保存（离线纸/文件/可无此 OTP 访问的密码管理器）。独立 OTP_BACKUP 权限生成 AES-256-GCM 恢复包，上传前已加密；恢复 key wrap 独立 archive data key，随机/salt/nonce/schema/KDF/AEAD 标识在认证头，账户记录在密文内。恢复 key 只经评审的受保护 BLE 传，不持久存设备/服务器。容器 ≤64 KiB、块 ≤1 KiB、流式加密，密文端点/存储 `/otp-backups` 独立权限，不混普通票据导出/WebDAV 树。浏览器可离线下载密文包，确认后清临时 key 引用。Q-10 实施前固定精确容器字节、nonce 分配、已知答案/错 key 测试。

无原设备恢复：独立网页登录恢复码/第二验证器或离线备份读取 → 获取离线 key → 浏览器本地认证解密 → 审核元数据 → 新设备验证会话/本地解锁/实体恢复权限 → 有界暂存 → checksum/schema 验证 → 原子密库提交。错 key/损坏/过新包失败不改旧库。备份访问不能只靠原设备 OTP。OTP 备份完成有独立回执和 key 持有/恢复测试，服务器收密文不证明可恢复。Web Crypto 仅提供原语，不证明产品协议正确，见 [W3C Web Crypto](https://www.w3.org/TR/webcrypto/)。

## SC-10 资源预算与可靠保存

实现 SU-14/15。ESP32-C3、8 MB Flash、无 PSRAM、IDF 5.5.3。上游[默认分区](../../partitions.csv)保留 NVS/PHY/factory，不继承 5 MiB app/双资源槽。应用分区 Q-05 按实测镜像和数据需求分配 cache/journal/vault/metadata，并在持久化实施前定义迁移/刷写后果；默认 24 KiB NVS 放不下拟定缓存，不把应用分区变成上游模板必需约束。

| 组件 | D/V 初始上限 / 测量门槛 |
| --- | --- |
| app 代码/只读数据（不含字体） | 目标 ≤3 MiB，IDF 实测，不是预留分区 |
| 字体 | 候选 Flash ≤2 MiB、动态 glyph/cache RAM ≤16 KiB；字集可能迫使重设计 |
| 票据元数据/票码 | 元数据 ≤800 KiB、票码 ≤1 MiB，解码临时区 ≤16 KiB，无整图 RAM 解码 |
| 待办/日志/设置/密库 | 待办 ≤1 MiB、日志 ≤64 KiB、设置/操作记录 ≤64 KiB、OTP ≤64 KiB；测加密/tag 开销 |
| 原子保存临时空间 | 至少最大可替换记录及 manifest/日志提交余量，不默认整快照双倍；测写放大后定 Flash |
| 音频 | ring 8 KiB +临时区 ≤4 KiB，DMA 另测，不整段录音入 RAM |
| LVGL/显示 | 候选 LVGL 32 KiB +DMA/draw ≤24 KiB；检查实际 BSP 分配/MAP，限对象数；无 153,600 字节 RGB565 全帧 |
| BLE/Wi-Fi/TLS/密码学 | 候选组合峰值 ≤140 KiB，真实认证/TLS 测最低 heap/最大内部/DMA 块，不是加法保证 |
| 任务栈 | UI/input 4 KiB、sync 6 KiB、storage 4 KiB、audio 4 KiB、app crypto 6 KiB；IDF/NimBLE/LVGL 另测，初始总栈目标 ≤48 KiB |
| 安全余量 | 峰值内部 free heap ≥40 KiB、最大块 ≥16 KiB；分配不能只看总量 |

此表是建议上限，不证明相加可放入 RAM/Flash。ST-01 测基线和并发最坏场景；用 `idf.py size`、size-components、size-files、MAP、`heap_caps_get_minimum_free_size`、最大块/DMA、栈 high-water mark。按调度组合测字体、QR 解码、加密 BLE、Wi-Fi TLS 握手、录制/播放、掉电存储。无余量不接纳作业。ST-06 检查 100 次导航/取消、24 h 混合运行、泄漏趋势、watchdog、heap/栈余量。缓存 quota 包括头/tag/日志/碎片/临时空间，NO_SPACE 拒绝更新，不删有效旧数据。

设备保存：storage-owner 日志含 schema/sequence/length/hash（CRC 检测撕裂，秘密由 AEAD 保护完整性），不可变记录 → 校验 manifest → 原子 active pointer。提交前掉电旧代次有效，提交后恢复新代次，损坏暂存忽略/隔离。待办意图独立持久日志，不随快照清理删除。检查余量后仅回收不可达已提交对象。NVS blob/文件系统及事务原语取决 Q-05；未经 flush/commit 掉电测试不能宣称 rename 即安全。

回执：LOCAL_SAVED =设备提交已验证；SERVER_RECEIVED =服务器数据库/文件及备份发送队列可靠提交；WEBDAV_BACKED_UP =对象/manifest/hash 上传并回读验证。每项含资源/版本/hash/时间/操作 ID，分别显示。服务器 202 可为可靠队列接纳，不等于 Nextcloud 完成或 WebDAV 完成。备份失败保留服务器可靠数据及重试队列；服务器磁盘满在成功前返回 NO_SPACE。普通 WebDAV 按批准内容存票据/设置/待办快照/日志，无明文凭据/OTP。OTP 密文独立权限/命名空间/恢复。

空白服务器恢复：不依赖原 OTP 的所有者认证，选择不可变已验证 WebDAV manifest，核对版本/hash，在暂存恢复记录与操作账本，再提交新服务器代次；不复活撤销凭据，绑定重确认。待办意图先与实时 CalDAV/ETag 对齐再重放，备份不能覆盖较新任务编辑。设备较新本地版本需审核导出/对账，不盲上传旧快照覆盖恢复状态。OTP 仅 SC-09 独立恢复。无备份明确记录不可恢复，可用已批准设备导出，不能保证仅凭元数据重建。SA-21 测磁盘满/撕裂写/损坏备份/无原设备恢复。
