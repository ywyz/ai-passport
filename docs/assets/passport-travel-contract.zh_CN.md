[English](passport-travel-contract.md) · **简体中文**

# AI Passport 首批旅行数据与同步约定

> **已被替代（2026-10-05）**：本文仅供历史追溯，不再是实施或验收依据。最新范围以[多合一固件规划](passport-suite-plan.zh_CN.md)为准：保留翻译、OTP、合并票夹、Nextcloud 待办及三体时钟天气；其余功能取消。旧阶段、手机/SoftAP 配网方案和实施提示词不得直接执行。

更新日期：2026-10-03。状态：`passport-travel-v1-draft`，D0 评审草案，未冻结、未实现。对应[首批 PRD](passport-travel-prd.zh_CN.md)和[总体规划](passport-suite-plan.zh_CN.md)。本文中的 API、字段、目录和上限都是应用设计提案，不是现有 BSP 或服务器能力。

## 1 通用数据规则

JSON 使用 UTF-8，网络格式版本为整数 `schema_version: 1`。ID 是大小写敏感 ASCII 字符串，最多 64 字节；设备、账号、地点、行程、版本和事件的命名空间分开。名称不是 ID。revision 为服务器生成的正整数；设备不自行递增服务器版本。时间字符串用带时区的 ISO 8601 表示；来源只有时间没有日期时先在服务器补齐日期或标记缺失，设备不猜跨午夜。

不支持的必需版本拒绝启用；可忽略文档明确允许的未知可选字段，未知事件操作或未知枚举必须报错。缺失值采用 `null` 及可选 `missing_reason`，不以零、空字符串或“未知”混充数值。数组、文本和响应都校验上限；HTML 及 Markdown 输入转换为受支持纯文本，不能把导入内容当代码或工具指令执行。

对象引用须在发布时验证，设备按流分页，不装入全部国家地点。用户内容包含隐私；测试资料明确 `fixture: true`。发布资料及事件只允许其所属账号读取。稳定 ID 变更必须带映射或保留历史；删除定义墓碑，避免恢复旧快照后悄悄复活已删除记录。

## 2 对象字典

| 对象 | 必需字段 | 可选字段与约束 |
| --- | --- | --- |
| itinerary | id、revision、title、timezone、start_date、end_date、entry_ids | `fixture`；日期有序 |
| rail_ticket | id、revision、itinerary_id、travel_date、boarding_station、arrival_station、train_number、review_state、provenance | carriage、seat、departure_at、stops、train_model 可缺失；上车站不是始发站 |
| rail_stop | station_id、name、sequence、arrival_at、departure_at、time_kind | 始发到达及终到发车可 `null`；日期完整，停站计算不能为负 |
| station_departures | station_id、service_date、data_kind、fetched_at、expires_at、source_id、items | `data_kind` 为 scheduled 或 live_board；platform、gate、delay、boarding_status 有实际来源才提供 |
| guide_route | id、revision、itinerary_id、review_state、ordered_stop_ids | 发布版本不可变，引用所需讲解和媒体 |
| guide_stop | id、place_id、title、narration_text_ref、practical_note_ref | audio_ref 可缺失但整阶段必须有离线播放验收样本；开放时间带来源和时间 |
| place | id、province_id、parent_id、kind、name、initials | kind 为 province、city、county、attraction；城市归属 ID 和全路径；首字母来自经审核数据 |
| province_pack | id、province_id、data_revision、coverage、place_index_ref、glyph_inventory_ref | stamp_refs、selected_refs；覆盖范围不等于完整全国目录 |
| record | id、kind、context_id、target_id、revision、state、updated_by | kind 为 visit、wish、guide_progress；去过和想去分别记录，不依赖资源包 |
| change_event | event_id、device_id、device_epoch、sequence、record_id、context_id、target_id、operation、value、base_revision | device_time、time_trust、predecessor_event_id；没有可信时间可 `null` |
| backup_snapshot | snapshot_id、schema_version、created_at、server_epoch、included_cursor、files | 文件大小及 SHA-256；只有 complete 才可恢复；未来可引用独立加密 OTP 文件 |

`review_state` 为 draft、reviewed、published。字段 provenance 至少包含来源类型 user_ticket、user_confirmed、provider 或 fixture、source_id、获取时间及该字段审核状态；来源类型 fixture 不可作为真实接入证据。整个对象的审核状态不能掩盖未审核的重要字段。

`time_trust` 为 unknown、user_set 或 network_synced；时间信任不能替代 TLS 认证。省份命名空间例如 `CN-JS`；常驻 34 个标识及显示名称在编码时核对，不能只在测试计数写 34 就宣称行政区完整。县区与景点 ID 不使用名称哈希作为永久身份。

记录的逻辑唯一键为 `(account_id, kind, context_id, target_id)`。visit 和 wish 的 context_id 为 null，跨行程共享同一地点足迹；guide_progress 的 context_id 为稳定 route_id，同名或复用站点在不同路线中不共享进度。同一路线新版本保持该 ID 才继承进度。服务器为逻辑键预分配稳定 record_id，并通过本账号资源元数据提供离线映射；首次状态为不存在、base_revision 为 0。事件必须引用已分配映射，拒绝同一逻辑键另建任意 record_id 或将一个 ID 指向另一目标。资源索引按页携带所需映射，不把全省映射一次载入内存。

## 3 虚构车票样本

以下是逻辑样本，不对应任何实际车次。样本省包不依赖这个车次真实经过江苏。代码块采用 Unicode 转义，保持两种文档样本一致；显示前须解码为真实 UTF-8。

```json
{
  "schema_version": 1,
  "fixture": true,
  "id": "fixture-ticket-001",
  "revision": 1,
  "itinerary_id": "fixture-js-trip-001",
  "travel_date": "2026-10-03",
  "timezone": "Asia/Shanghai",
  "train_number": "TEST-001",
  "boarding_station": {"id": "fixture-station-b", "name": "\u6837\u672c\u7ad9 B"},
  "arrival_station": {"id": "fixture-station-d", "name": "\u6837\u672c\u7ad9 D"},
  "carriage": "02",
  "seat": "03A",
  "departure_at": "2026-10-03T23:58:00+08:00",
  "train_model": null,
  "missing_reason": {"train_model": "source_unavailable"},
  "review_state": "published",
  "provenance": {
    "source_type": "fixture",
    "source_id": "fixture-ticket-001",
    "fetched_at": null,
    "reviewed_fields": ["boarding_station", "arrival_station", "train_number", "travel_date", "carriage", "seat", "departure_at"]
  },
  "stops": [
    {"station_id": "fixture-station-a", "name": "\u6837\u672c\u7ad9 A", "sequence": 0, "arrival_at": null, "departure_at": "2026-10-03T23:20:00+08:00", "time_kind": "scheduled"},
    {"station_id": "fixture-station-b", "name": "\u6837\u672c\u7ad9 B", "sequence": 1, "arrival_at": "2026-10-03T23:55:00+08:00", "departure_at": "2026-10-03T23:58:00+08:00", "time_kind": "scheduled"},
    {"station_id": "fixture-station-c", "name": "\u6837\u672c\u7ad9 C", "sequence": 2, "arrival_at": "2026-10-04T00:20:00+08:00", "departure_at": "2026-10-04T00:25:00+08:00", "time_kind": "scheduled"},
    {"station_id": "fixture-station-d", "name": "\u6837\u672c\u7ad9 D", "sequence": 3, "arrival_at": "2026-10-04T01:00:00+08:00", "departure_at": null, "time_kind": "scheduled"}
  ]
}
```

验收断言：上车站 B，始发站 A；下车日期 10 月 4 日；C 停站 300 秒；未知车型显示不可用。补充案例应含无座、车厢缺失、同名站、日期不完整和实际时间与计划不同。

## 4 资源格式与启用

设备第一版不直接解压 ZIP。服务器发布资源清单，设备逐文件下载写入暂存区，校验后切换活动索引。允许纯文本、受支持的索引、已验证的字体或位图、受支持音频；不允许脚本、任意可执行内容和未登记编解码格式。

清单每个文件还必须包含 file_id，与下载 API 的 file_id 对应；file_id 在包版本内唯一，不能把 path 当鉴权标识。清单包含 schema_version、pack_id、revision、kind、fixture、total_bytes、files、min_app_contract 和 glyph_inventory。每个文件包含相对 path、size_bytes、sha256、media_type、format。生成的散列必须是实际文件 SHA-256；这里不提供假的 64 位散列当有效样本。总大小等于文件大小总和，另计算文件系统及暂存余量。路径不得含绝对路径、`..`、空段、反斜杠或符号链接；明确规范化后再检查，禁止从清单跳转任意外部 URL。

参考逻辑目录：

```text
packs/<pack_id>/<revision>/manifest.json
packs/<pack_id>/<revision>/data/itinerary.json
packs/<pack_id>/<revision>/data/places-index.json
packs/<pack_id>/<revision>/text/stop-001.txt
packs/<pack_id>/<revision>/audio/stop-001.pcm
packs/<pack_id>/<revision>/fonts/glyphs.json
```

`glyphs.json` 只声明覆盖需求，不是字体。D1 固定小样本文字全部使用已生成的基础子集；D4 动态省份文本方案须原型证明，不得通过仅提供清单宣称字形可显示。缺字时阻止该版本启用并显示缺字原因；服务器可以制作新可显示包，不能静默丢字。删除省包仍可显示旧足迹摘要所需名称，记录保留可验证的显示名称快照或独立摘要资源，并保留摘要所需字形或预渲染显示材料；不能只存名称却同时删掉唯一可显示它的字体。摘要材料计入持久记录预算，与省包缓存分开清理。

状态为 absent → offered → preflight → downloading → verifying → activating → ready。失败为 failed 或 cancelled；ready 的旧版本一直保留到新版本完成切换。启动扫描暂存和活动索引，掉电不会把半包认作 ready。D1 选择中断后重传，不承诺断点续传；流式写入和分块处理仍必需。垃圾暂存清理不能删除活动包或个人记录。

下载前空间条件：可用空间至少是新文件总量、存储元数据和明确余量之和，旧包及记录不计入可清除空间，除非用户先明确删除。活动索引写入也需掉电安全。实际元数据和余量由选定存储实现测量，不硬编码经验百分比代替测量。

## 5 首版建议上限

这些是待评审的拒绝边界，不是实测容量。实际资源不满足时收紧并评审，不能扩大到耗尽内部 RAM。

| 项目 | 初始提案 |
| --- | --- |
| 单个 ID、名称、标题 | 64、128、128 UTF-8 字节；截断只能在字界 |
| 搜索初始串 | 8 ASCII 字母 |
| API JSON 响应、清单 | 各最多 32 KiB，流式解析优先，不同时保留多个响应副本 |
| 同步批次 | 最多 20 个事件，整个请求不超过 8 KiB |
| 对象列表 | 每页最多 20 项，游标读取；停站最多 128，路线站点最多 64，不能要求全部一次进 RAM |
| 单段讲解文本 | 8 KiB，上屏分页；站点目录引用文件，不把全讲解塞进清单 |
| 资源清单文件数 | 128；省包分片索引，地点规模另据数据集测定 |
| 网络与文件数据块 | 初始 4 KiB，TLS 缓冲另计且需实测 |
| D1 总样本包 | 768 KiB 上限，是否可下载取决于实际空间预检 |
| 本地待同步事件 | 256 项，接近上限提示先同步；满时停止新增而不静默丢弃 |
| 连接与请求 | 连接超时 10 秒，普通请求总时限 30 秒；资源进度 15 秒无数据判失败，整个下载最多五分钟 |
| 重试 | 设备每轮共三次尝试：首次及 2、4 秒后重试；失败暂停直到手动重试或连接恢复。服务器 WebDAV 临时失败任务 60 秒后重新排定，永久错误等待配置修正 |
| 录音 | 最多 10 秒，分块传输；无可靠时间不影响录音上限的单调计时 |

上传资料由服务器设置单文件 10 MiB 的初始上限；不把这个值用于设备响应。浏览器和服务器也验证格式、数量、解码后的大小，OCR 等后台工作有自己的队列上限。设备只缓存用户选择的完整版本；具体保留数量由实际字节决定，不承诺固定几个省或几个行程。

## 6 设备 API 草案

以下不是服务器已经实现的接口。网页使用独立登录会话及防跨站请求机制，设备使用可撤销凭据；WebDAV 凭据不出服务器。API 在同一受控服务器 origin 下，重定向不能把 Authorization 转发到未受信任主机。

| 方法与路径 | 请求或响应含义 |
| --- | --- |
| POST `/api/v1/web/device-bindings` | 网页登录后创建一次性绑定码及 expires_at；仅创建请求，不直接激活设备 |
| POST `/api/v1/device-bindings/exchange` | 绑定码和设备显示标识；返回待确认 binding_id 和账号显示标签，受限公共兑换入口 |
| POST `/api/v1/device-bindings/{id}/confirm` | 仅原兑换会话可提交实体确认结果，成功返回设备专用凭据；交换会话也需短期认证 |
| GET `/api/v1/device/catalog?cursor=...&limit=20` | 可用版本、覆盖、大小、已审核状态及下一游标，设备仅访问本账号 |
| GET `/api/v1/device/packs/{id}/{revision}/manifest` | 不可变清单，兼容性与摘要 |
| GET `/api/v1/device/packs/{id}/{revision}/files/{file_id}` | 由服务端清单内 ID 定位文件，不直接采用任意请求路径 |
| POST `/api/v1/device/sync` | push 事件及 pull 游标；逐事件结果和有上限的记录变更 |
| GET `/api/v1/device/status` | server_epoch、server_time、可见同步游标、备份快照及状态，不暴露其他账号资料 |
| POST `/api/v1/web/backups` | 网页发起完整快照任务，异步返回 job_id，不把排队当完成 |
| GET `/api/v1/web/backups/{job_id}` | 任务状态、错误及已完成 snapshot_id |
| POST `/api/v1/web/restore-previews` | 独立恢复环境中验证指定快照并提供预览；正式恢复需明确确认 |

响应错误包含稳定 code、用户可读 message、可选 retry_after_seconds 和 request_id。初始错误包含 `AUTH_REQUIRED`、`BINDING_EXPIRED`、`REVISION_CONFLICT`、`SCHEMA_UNSUPPORTED`、`SIZE_LIMIT`、`NO_SPACE`、`HASH_MISMATCH`、`GLYPH_MISSING`、`PROVIDER_UNAVAILABLE`、`DAV_PENDING`、`DAV_QUOTA`。密码、种子、完整车票和绑定码不写入错误日志。

设备身份显示标识不是认证秘密。绑定码输入错误限速；服务器兑换成功后保留 pending 状态直至实体确认，不让已泄露短码直接获得长期令牌。设备凭据交换的随机性、TLS 引导及身份证明须在 D1 编码前写设计核查记录，不能只靠上述接口名当安全完成。

绑定请求须在设备发出前持久保存其随机请求 ID 和短期重试证明；它们是敏感会话材料，不写日志。兑换与确认都支持相同请求的幂等重试：短码同一请求的丢包重试返回同一个 pending 会话，其他请求不能再次兑换；确认成功但响应丢失时，原会话在五分钟有效窗口内可取回同一结果。服务器短期保护保存结果，过期后删除会话恢复材料并要求重新绑定；未兑换码按原过期规则失效。不要每次确认重试新增设备或改变已返回的有效凭据。长令牌只在受保护客户端保存，服务器长期存摘要；短期可恢复结果的保护与删除需实现并测试。撤销后原会话不可复活设备。

## 7 事件去重与冲突

device_epoch 是持久事件实例标识，普通重启不改变；事件携带它以核查前序依赖。sequence 为 1 至 2^53−1 的整数，达到上限前创建新 epoch，不溢出或重用旧 ID。

事件 ID 建议为持久 device_id、持久 boot-independent epoch 和单调 sequence 的组合。sequence 计数与事件保存必须一致；刷写或重建标识需新 epoch。服务器去重键是账号、device_id、event_id；重复相同内容返回原结果，重复 ID 不同内容拒绝。

支持 `visit.set`、`wish.set`、`guide_progress.set`，值为明确布尔或定义的站点状态，不能用 toggle 以免重试翻转。网页修改也进入相同版本机制。新记录 `base_revision: 0`；已存在记录修改携带所知版本。服务器在一个本地数据库事务内保存接受的事件、更新记录和待上传任务，然后才返回 acknowledged。

版本相符则更新；不同目标可独立合并；相同目标旧版本但值与当前相同可作为 no-op 接受；值不同则冲突，返回当前 revision 和状态。设备保留冲突事件并显示待处理，不自动按未知时间覆盖。用户网页选择保留当前值或明确覆盖后，产生带当前 base_revision 的新事件；原冲突标记 resolved，不重传旧事件覆盖。

device_id、设备事件 epoch、sequence 和事件内容作为同一可恢复日志设计，事件 ID 编码不得超过 64 字节；可以采用固定长度不透明 ID，不能直接拼接三个最长 64 字节字段。设备本地状态是已确认快照加待处理事件。未确认事件只在收到 ack 后可清理；网络断开后重传不重复。冲突时仍保留操作说明，明确同步未完成。队列满或保存失败不能显示完成印章。个人足迹记录和资源包独立持久保存。

同一记录连续离线修改必须有顺序关系，不能将第二次操作当成外部冲突。建议后续事件带 predecessor_event_id，服务器用已接受前序事件的结果版本作为有效基础；如果网页在前序之后修改了该记录，仍产生真正冲突。前序未接收则返回依赖待处理，不 ack；前序冲突时后续操作进入同组待解决状态。只对同一记录建立依赖，不阻塞无关记录；不同 epoch 的事件不得伪造依赖。A05 增加连续离线去过、撤销、再去过及交叉网页修正案例。

同步请求与响应轮廓示例，仅使用测试 ID：

```json
{
  "schema_version": 1,
  "server_epoch": "fixture-server-epoch-1",
  "pull_cursor": null,
  "events": [{
    "event_id": "fixture-event-1",
    "device_id": "fixture-device-1",
    "device_epoch": "fixture-device-epoch-1",
    "sequence": 1,
    "record_id": "fixture-progress-stop-001",
    "context_id": "fixture-route-001",
    "target_id": "stop-001",
    "operation": "guide_progress.set",
    "value": true,
    "base_revision": 0,
    "predecessor_event_id": null,
    "device_time": null,
    "time_trust": "unknown"
  }]
}
```

```json
{
  "schema_version": 1,
  "server_epoch": "fixture-server-epoch-1",
  "results": [{"event_id": "fixture-event-1", "status": "acknowledged", "record_revision": 1}],
  "changes": [{"id": "fixture-progress-stop-001", "kind": "guide_progress", "context_id": "fixture-route-001", "target_id": "stop-001", "revision": 1, "state": true, "updated_by": "fixture-device-1"}],
  "next_cursor": "fixture-cursor-1",
  "has_more": false,
  "backup": {"state": "pending", "included_cursor": null, "snapshot_id": null}
}
```

结果状态为 acknowledged、conflict、rejected 或 dependency_pending；响应中未列的事件仍待确认。目标 ID 须存在于本账号已发布资料或有效历史记录中，不能通过事件创建任意私有目标。遇到 server_epoch 不同返回需核对错误，不先接受推送。备份 included_cursor 表示已覆盖的变更水位，不是客户端可以按字符串大小比较的时间；是否覆盖当前操作由服务器返回判定。

服务器变更日志按提交顺序记录不可变记录版本及删除墓碑。分页游标保存读取水位及本轮上界，不以随时变化的当前记录列表作为变更日志；先完成本轮上界，再读取新提交，避免分页遗漏。去重结果与前序事件所需版本保持到设备明确推进同步水位后才能安全压缩；D1 不自动清理仍有依赖的去重数据。全部 conflict/rejected 事件保留可见错误且可人工放弃或修正，永久错误不无限重试。

## 8 游标与服务器恢复

pull 游标是服务器不透明字符串，设备保存它和 server_epoch。响应有 next_cursor 和 has_more，设备先应用本页变更并持久保存，再推进游标。服务器本地记录去重及事务定义不能仅依靠 HTTP 成功状态。

服务器从快照恢复时生成新 server_epoch，旧设备令牌吊销。设备重新绑定后发现 epoch 不同，进入 reconciliation，不能把旧序号直接当新服务器进度。先分页获取快照，保留本地未确认事件；已确认但不在恢复快照中的本地记录也要比较并提示恢复差异，不能因原 ack 已清队列就静默丢失它们。重提变更使用新事件和明确的当前版本，由用户处理冲突；资源版本重新校验。

删除墓碑必须包含在备份内并保持既定保留范围。不能用一次全量列表缺席推断记录被删除。D1 先单设备，若以后多设备，仍使用相同服务器版本冲突规则，不为多设备假定无需冲突。

## 9 WebDAV 保存与恢复

服务器工作数据库保存在本地卷，WebDAV 保存不可变资源和完整记录快照。建议目录以配置的私有根下账号 ID 区分：

```text
passport/v1/accounts/<account_id>/resources/<pack_id>/<revision>/...
passport/v1/accounts/<account_id>/snapshots/<snapshot_id>/records.json
passport/v1/accounts/<account_id>/snapshots/<snapshot_id>/manifest.json
passport/v1/accounts/<account_id>/snapshots/<snapshot_id>/complete.json
passport/v1/accounts/<account_id>/otp-backups/<backup_id>/ciphertext.bin
passport/v1/accounts/<account_id>/otp-backups/<backup_id>/manifest.json
```

OTP 目录仅是后续隔离边界，D1 不创建含真实种子的文件或冻结其密码学格式。账号标签、账号参数等敏感内容也应在后期加密文件内，不通过公开清单泄露。

建议快照流程：取得一致性数据库导出及 included_cursor → 上传缺失不可变资源 → 上传 records 和清单 → 核验所有必需引用 → 最后发布 complete。中断快照没有 complete，不可恢复。latest 索引仅作提示；索引损坏时枚举和验证完整快照，不依赖一个可覆盖文件决定有效性。摘要验证恢复完整性，但不替代认证和授权。

具体 WebDAV 能力需要实际服务验证；[RFC 4918](https://www.rfc-editor.org/rfc/rfc4918.html)定义 WebDAV 方法和条件处理，不能据此假定目标服务支持整个快照的原子事务。D1 至少测目录创建、上传、下载、枚举、覆盖冲突、权限、配额和上传中断；若依赖 ETag 或条件更新，先核查实际行为。不依赖 LOCK 或跨文件事务完成 v1 快照发布，采用唯一版本路径和最后完成标记。

服务器本地存储持久成功、WebDAV 上传成功、快照完整可恢复为不同状态。上传成功但清单未完成仍是 partial。网页显示 pending、uploading、complete、failed 和最后完整版本；设备收到当前版本及备份状态，不把最后旧快照显示成当前新修改已备份。

恢复应在空白本地数据库或单独环境进行：配置 WebDAV 凭据 → 列出完整快照 → 校验版本、大小、摘要、引用 → 预览账号记录数及缺失 → 用户确认导入 → 建立新 server_epoch → 重新绑定设备并核对差异。仅从 WebDAV 恢复时不需要原服务器数据库；尚未上传及原本已被删除且不在保留版本的内容不能保证恢复。备份保留天数和删除策略在使用真实资料前确定，首版不自动清理最后一份完整快照。

完整快照还须包含稳定账号和地点/路线身份、已发布资料及版本、资源映射、记录和墓碑、与该快照一致的变更水位及去重材料。恢复不能只导入 records.json 而丢失记录引用的路线或地点。密码摘要、设备长期令牌、绑定会话及 WebDAV 密码不作为自动复活的认证材料；空白实例在受控本地流程重新设管理员和 WebDAV 凭据，恢复后保持原数据账号 ID，生成新的 server_epoch。恢复资源清单时只接受快照目录及允许的资源根内相对引用，验证大小和路径，防止导入任意本地文件或外部地址。最后完整快照和它引用的资源须受保留策略共同保护。

## 10 OTP 备份必须满足的边界

OTP 的密钥及生成参数必须有独立加密备份，经服务器保存到 WebDAV。解密能力由用户独立保管，不只绑定原设备，不与密文放在同一普通备份中。备份服务的恢复登录也不能只依赖原设备 OTP。具体算法、导入格式、计数器处理和密钥管理在最后阶段决定，本约定不提前冻结。

备份状态必须对应当前账号版本，导入或更改后尚未上传时显示未完成。恢复缺少凭据、密文损坏或格式不兼容必须明确失败，不能生成貌似有效但实际错误的验证码。最后阶段在替代设备用测试账号验证恢复；恢复不使遗失设备上的种子自动失效，遗失流程另含服务器令牌撤销和各服务账号的种子更换。

### D1 实验存储布局

以下为评审后的实验建议，尚未写入 partitions.csv，也不证明最终多合一容量。仅在获准的功能分支实施，真实数据前重新评估迁移。无 OTA 双槽。

| 区域 | 类型 | 起始地址 | 大小 |
| --- | --- | --- | --- |
| nvs | data/nvs | 0x9000 | 0x6000 |
| phy_init | data/phy | 0xF000 | 0x1000 |
| factory | app/factory | 0x10000 | 0x500000，5 MiB，含链接字体 |
| records | data/nvs | 0x510000 | 0x20000，128 KiB |
| cache_a | data/fat | 0x530000 | 0x168000，1.40625 MiB |
| cache_b | data/fat | 0x698000 | 0x168000，1.40625 MiB |

布局在 0x800000 结束，应用偏移保持基线一致；引导及分区元数据占前部地址空间。D1 建议大资源用 IDF FAT＋磨损均衡、个人事件和摘要用独立 NVS。依据 [NVS](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-reference/storage/nvs_flash.html) 与 [FAT](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-reference/storage/fatfs.html) 文档选择，仍须真机掉电验证，不把存储 API 本身当作多字段事务保证。

cache_a 与 cache_b 共 2.8125 MiB；每槽保留一个完整资源集，活动槽只读，更新只写非活动槽，完成后切换 NVS 代际指针。D1 资源集仅含一个测试包；后续多包应组成完整代际或另行评审更新方案，不能在活动 FAT 卷里修改其他文件后声称旧包有隔离保护。两个 768 KiB 版本占 1.5 MiB，各槽剩余 0.65625 MiB 原始空间，合计 1.3125 MiB；FAT、磨损均衡和保留余量尚需实测。启动验证活动指针和资源摘要，新槽未完整则继续旧槽；挂载失败不自动格式化，仍需掉电故障验证。128 KiB records 不能保证可容纳 256 个最坏事件及全部摘要：同时检查事件数量和实际剩余条目/字节，达到任一限制就拒绝新增并要求同步或导出，不丢弃历史。记录日志先可靠保存，再由它重建本地状态；ack 收据及变更后的基础状态可靠保存后才清队列。网络游标与资源活动指针也用可恢复版本，不能假定多个 NVS 键同时写就是原子事务。缓存损坏时禁止自动格式化 records；显示可恢复错误，只有用户明确选择才清资源缓存。

## 11 第一版冻结条件

冻结前须评审对象字段、资源编码、事件版本机制、全部拒绝上限、配网和 HTTPS 引导、分区与迁移实验、WebDAV 发布策略及测试入口。本文样本 JSON可在设计核查时解析，但不等同于实现已经支持；清单、音频和字体真实产物在 D1/D3 生成。

D1 如仅通过 mocked WebDAV、模拟设备或固定成功响应，A06 和设备验收保持未验证。真实数据源与 AI 适配留到对应阶段；每次扩展提升契约版本并记录兼容路径，不能随意改 v1 而让旧设备静默误读。
