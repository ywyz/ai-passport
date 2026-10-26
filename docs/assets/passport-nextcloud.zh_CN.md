[English](passport-nextcloud.md) · **简体中文**

# Nextcloud 待办伴随服务

这是 [suite-plan](passport-suite-plan.zh_CN.md#0-当前实现与验收) 中已获准待办增量的服务端实现。
实现位于 `companion/tasks/`，使用 Python 3.12 或更新版本，提供 CalDAV 发现、显式列表选择、
读取任务和设备缓存 API。后续[设备端增量](passport-device-tasks.zh_CN.md)实现了列表/详情、
快照下载、设备持久缓存和私密 USB 令牌配置，物理设备验收仍待完成。
服务已部署到现有配网页的 HTTPS 代理之后。
当前源码增加可选条件完成和冲突重放；用户已于 2026-10-20 确认并完成启用和确切
设备镜像刷写，随后完成/离线缓存/正常重启重放已通过用户验收。五分钟服务端刷新 timer
已部署。当前整体状态见 [suite-plan 第 0 节](passport-suite-plan.zh_CN.md#0-当前实现与验收)。

## 完成操作增量（历史），2026-10-20

用户已删除 Deck。来源检查确认剩余一个所有者可写 Tasks 列表，150 条来源记录、
两条未完成任务，没有活动空标题。现网只读选择/快照已原子更新，移除不存在的 Deck
选择，未改动 Nextcloud 记录或凭据。下文两个列表/154 条的观察属于历史记录。

可选私密字段 `completion_token` 启用写接口和各 WSGI 进程的后台任务。它必须是
独立随机的 32–128 字符 URL 安全令牌，区别于读取及管理令牌。不含此字段的旧配置
仍为只读。在本机受限配置中生成/保存，不放入命令参数或聊天；首次升级仅增加此字段。

POST 接纳先将规范意图和回执持久提交到 SQLite，再返回 `SERVER_DURABLE`；相同
ID/hash 返回已存回执。64 位十六进制任务 ID 绑定账户/列表/UID/重复实例身份。仅
允许已选、重新验证所有者可写的列表，以及具有强 ETag 的普通非循环 VTODO。共享
列表、子任务、重复任务维持只读。后台重新检查发现/选择，通过 CalDAV REPORT 读当前资源，再携精确
`If-Match` PUT；只更新 STATUS、PERCENT-COMPLETE、COMPLETED 和必要时间戳，
保留其他行、折行及提醒。最终 REPORT 必须确认完成、100% 和 UTC 完成时间，不创建、
删除或恢复未完成任务。

ETag 变化/412 进入冲突，不覆盖较新编辑。响应丢失恢复要求与持久记录的提交资源
哈希精确一致，并具有有效完成字段，存在歧义则冲突。持久租约恢复中断任务。临时
故障按 2/5/15/60 秒加抖动重试，五次后 `RETRY_PAUSED`；显式重试仅重置暂停预算。
认证失败停止执行，需私下修复并 `resume-auth`。台账最多保留 1,024 份回执，不自动
淘汰，满时明确拒绝。

成功时事务更新活动视图，保留完成历史及全来源抓取时间，不覆盖并发较新来源快照。
普通刷新仍下载已提交缓存。新增十二项模拟完成测试，连同 30 项读取回归覆盖持久性/
权限、属性保留、条件竞争、响应丢失、重启和有界重试。真实 SA-12 仍需设备完成、
离线重启重放和网页并发修改验收。

已批准完成包 `cfb330b4` 现已启用，`d5779c7e` 固件在 COM3 的五个镜像均校验通过。
回环/公网 HTTPS 权限检查未接纳完成意图，保留已有凭据值、列表选择、代理和配网页。
基线台账为空、活动任务两条；启动恢复 Wi-Fi/时间/天气。真实联网完成、离线重放及
冲突仍待操作者反馈。[设备记录](passport-device-tasks.zh_CN.md#完成操作候选版验证2026-10-20)
保留完整镜像/ELF 身份及有限观察，下文为历史只读部署事实。

## 部署事实与边界

2026-10-20 的 GET 缓存修复替代了最初完成读取路径。用户反馈其余设备功能正常，但
完成后提示冲突。两个操作回执指向同一任务；新鲜 REPORT 显示已完成、100%、UTC
完成时间和新 ETag，GET 仍返回旧的未完成资源。GET 忽略请求中的禁用缓存头，显示
Age 546–547 秒、边缘缓存期 3,600 秒。一份提交资源哈希与权威完成资源完全匹配，
证据说明完成后的旧读取导致误报，而非首次完成写入失败。

资源读取现使用标准
[calendar-multiget REPORT](https://www.rfc-editor.org/rfc/rfc4791#section-7.9)，
要求单个精确 href、ETag 和完整 calendar-data；缺失、多条或替换资源明确失败。
XML 归一化行尾在编辑/哈希前恢复为规范 CRLF，响应丢失恢复仍要求精确提交资源
匹配。强 ETag 和条件 PUT 保护不变，完成校验不再使用 GET。15 项完成测试及 30 项
读取回归覆盖旧 GET 缓存、CRLF 响应丢失恢复和 multiget 身份/资源缺失拒绝。真实
multiget 只读验证通过，未修改 Nextcloud。设备固件、配置及未解决本地意图不变；
修复后的真实完成/离线验收与上述检查分别记录。

2026-10-19 的只读检查确认 Nextcloud **35.0.0**（内部版本 `35.0.0.10`）、Tasks **0.18.1**、
AIO Docker 部署，以及主机 Python **3.12.3**。公开状态接口与 `occ` 一致；未处于维护模式，也不需要数据库升级。
用户先核对 SSH 主机指纹，再建立连接。服务器根文件系统当时剩余约 6.9 GiB。
用户说明服务前置阿里云 ESA；下述本机读取已验证通过该边缘层的认证 PROPFIND/REPORT 行为。
私有域名、账户名、凭据和原始结果不写入受版本管理的文档。

本机私密配置完成、用户明确选择全部两个列表后，认证发现及 REPORT 读取已通过公开 HTTPS 地址成功完成。
所有返回任务均有 ETag；缓存 API 分页和数据库重新打开检查通过。这仅覆盖 SA-11 的服务端读取部分，
不代表实体设备下载验收。本次数据只有 UTC/无截止时间，没有重复任务或备注，其他日期、重复及备注行为仍仅有模拟测试。
脱敏的数量、耗时和资源测量保留在本地 `build/acceptance/20261019-nextcloud-reader/`。

先对现有配网页主机进行了只读检查：具备 Caddy 2.11.7、Python 3.12.3，检查时约有 722 MiB 可用 RAM、
6.1 GiB 剩余磁盘。2026-10-19，用户明确批准在该主机部署服务、传输私密应用密码/令牌配置、
创建专用账户，并在现有 HTTPS 来源下新增 `/suite/v1/` 路由。已部署的源码/依赖包 SHA-256 为
`0c8bdec141bf1d0ab65bee0e636baba7b5c4d76b5760cc506615101d48f73bf5`。
安装过程核验全部打包文件及未变化的 Caddyfile 基线，检查回环 API，校验建议 Caddy 配置后重载。
Nextcloud 继续作为远程 CalDAV 数据源，其 AIO 容器没有改动。

服务器在 3.628 秒内成功读取两个已选列表，共 154 条任务，全部具备 ETag。
公网 HTTPS 检查通过：认证读取、五页快照下载（最大页 32,252 字节）、`no-store` 响应、
未认证读取被拒绝（401）、设备令牌修改列表选择被拒绝（403）。手动刷新服务以状态 0 完成。
API 正在运行且已启用开机启动；刷新后服务内存实测 36,937,728 字节，低于 192 MiB 上限。
这只是一次观测，不是负载测试峰值。现有三个配网页文件字节保持一致，其公网 HTTPS 响应与当前源码相符。
没有刷写固件。

可审阅的部署文件位于 [`companion/deploy/`](../../companion/deploy/)：

- `gunicorn.conf.py` 使用 Gunicorn 26.2.0、一个 worker/两个线程、回环端口 8787、请求上限，并关闭访问日志。
  Linux host test 实际验证 HTTP 启动、认证响应和未认证拒绝。
- `passport-tasks.service` 使用专用 `passport-tasks` 账户，代码和哈希核验过的纯 Python 依赖位于
  `/opt/passport-tasks/`，私密配置位于 `/etc/passport-tasks/config.json`，SQLite 位于
  `/var/lib/passport-tasks/`，内存上限 192 MiB。依赖随包放在 `vendor/` 中，避免修改系统 Python，也不要求主机提供 ensurepip。
- `passport-tasks-refresh.timer` 在启动后五分钟、以及每次任务完成五分钟后调度同一个
  `passport-tasks-refresh.service` oneshot 任务；该服务仍可由 `systemctl start` 显式触发。
  它共用私密配置/缓存，任务时限 30 分钟。timer 已于 2026-10-22 部署并启用，实测记录见下文。
- `caddy-route.example` 仅新增 API 路由，并限制请求体为 4 KiB；现有静态配网页文件保持原样。
  应用本地建议配置前，实际 Caddyfile 哈希必须与审阅基线一致。

私密配置已经单独传输，源码/依赖包不含凭据或任务数据。配置和 SQLite 文件仅供服务账户及管理员访问。
部署保留了私密 Caddy 备份；回滚仅在实际配置仍匹配已部署方案时恢复已审阅基线，然后停止新服务，
不能覆盖之后无关的代理修改。后续部署仍需具备相应授权范围。
[Gunicorn 部署指南](https://gunicorn.org/deploy/) 说明了这里采用的代理边界。

获准的操作者可用 `sudo systemctl start passport-tasks-refresh.service` 按需刷新服务端快照，
并用 `systemctl show passport-tasks-refresh.service -p Result -p ExecMainStatus` 检查结果。
该操作只读取 Nextcloud，不修改待办。周期 timer 在启动后五分钟、以及每次任务完成五分钟后
调度同一个 oneshot 服务，因此慢任务只会推迟下一次运行，不会并行。读取失败或认证停止时保留
上一份已提交缓存，不保证五分钟新鲜度。API 持续提供已提交快照，24 小时后标记过期；设备仍只在
操作者按“刷新待办”时下载该快照。认证失败时先私密修正凭据，再以服务账户运行 CLI 的
`resume-auth`，然后重试。

timer 单元安装后，可用以下命令启用和检查：

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now passport-tasks-refresh.timer
systemctl list-timers passport-tasks-refresh.timer
systemctl show passport-tasks-refresh.timer -p NextElapseUSecMonotonic
systemctl show passport-tasks-refresh.service -p Result -p ExecMainStatus
```

生产环境的手动读取应通过服务单元启动，不要并行直接运行 CLI `refresh`，以便 systemd 与 timer
串行化。最小回滚只需停用 timer，配置、数据库、API 和数据均不变：

```bash
sudo systemctl disable --now passport-tasks-refresh.timer
sudo systemctl daemon-reload
```

删除单元文件不是必需的；停用即可恢复手动操作。timer 已于 2026-10-22 部署并启用；
以上运行手册保留启用、检查和回滚步骤，实际验证范围见下文。

## 读取与缓存行为

2026-10-20，操作者反馈设备显示 154 条，包含已完成历史。来源快照中有 148 条
`COMPLETED` 和 6 条 `NEEDS-ACTION`。活动任务视图修复通过完整仓库门禁
（Linux 下 30 项伴随服务测试），随后仅更新现有服务的 `store.py`，执行前置
哈希检查并保留回退副本。部署文件 SHA-256 为
`742719788b71a78a0f8e19b56c0b2837cd91ce3fdf7a6ee8608665f7251401bd`，
这是对上文原始源码包的覆盖更新。回环及严格校验证书的公网 HTTPS 均返回 6 条
活动任务，单页 5,241 字节、状态 `READY`。旧全量游标被拒绝，401/403 权限检查
通过。数据库、私密配置、Caddyfile 和配网页资源哈希保持一致。筛选后的真实
载荷也通过设备解析器/缓存的主机重放。本次未写设备固件或令牌配置；修正视图的
实屏 Refresh、详情及离线验收仍待完成。

- 设备快照只包含 `NEEDS-ACTION` 和 `IN-PROCESS` 任务。`COMPLETED`、`CANCELLED`
  历史记录仍保留在 SQLite 和日历原文中，筛选不会执行 CalDAV 写操作。
  先筛选再计算总数和分页；全部结束的快照返回 `EMPTY`。活动视图版本与存储的
  来源版本不同，防止旧的全量游标混入新分页。现有服务端缓存立即应用筛选，
  无需重新读取 CalDAV。
- 发现流程依次读取 `current-user-principal`、`calendar-home-set` 和支持 VTODO 的集合。
  列表 ID 绑定账户及集合 URL，显示名称不是 ID。
- `calendar-query REPORT` 获取完整 VCALENDAR 和 ETag。保留返回的日历文本，包括未知属性、
  提醒和重复关系。XML 传输会规范化换行；不能声称与另行下载的 `.ics` 文件字节完全相同。
- 任务标识包括账户、集合、UID 和 recurrence ID。显式重复实例及子任务标记为只读；
  不展开重复规则，也不声称完整支持重复任务互操作。
- 纯日期、UTC、TZID/VTIMEZONE、浮动时间和无截止时间分别保留。有时区的时间同时提供 UTC 和配置后的显示值。
  浮动时间保持无时区属性。无法解析的时区或错误日历会使刷新失败，不捏造时间或静默丢弃任务。
- 拒绝跨来源、DAV 路径之外的 href 以及所有重定向，保持 TLS 证书校验。
  连接/读取超时为 15 秒，响应体读取预算为 60 秒；DNS 和正在等待的 socket 调用可能增加耗时。
  禁止 XML DTD/实体展开；错误响应只暴露固定错误码。
- 上限为 20 个集合/8 KiB、200 条任务/1 MiB、每条记录 8 KiB、每页 API 32 KiB、
  标题 512 个 UTF-8 字节、备注预览 4,096 字节。备注说明截断状态和原始字节数。
  单次 CalDAV 响应最大 4 MiB；一个日历资源最大 256 KiB，每份快照保留的原文合计最大 8 MiB。
  超限明确失败，不将不完整列表作为成功结果。
- SQLite 将新快照和原始资源一起提交。部分读取失败、重复标识、所选集合消失、超限或存储失败时，
  保留上一份已提交快照。列表选择采用版本检查，正在运行的刷新不能覆盖较新选择。
  旧选择的数据不会作为新选择的快照返回。
- 缓存超过 24 小时或刷新失败后标记 STALE。认证失败保留缓存并停止远程重试，直到操作者显式恢复。
  分页携带快照版本；并发刷新后返回 `REVISION_CONFLICT`，不混用两份快照的页面。

服务端的 `downloaded_at` 表示完成 CalDAV 读取的时间，不代表实体设备已经下载或启用该快照。

[SC-06 contract](passport-suite-contract.zh_CN.md#sc-06-nextcloud-caldav-与离线完成) 仍是产品目标。
周期服务端读取 timer 已于 2026-10-22 部署并启用。仍无浏览器管理界面、设备下载回执、
所有者登录/恢复、多账户托管、备份或加密存储。私密本地文件及独立只读/管理 bearer token 是
开发阶段的服务边界，不能作为 SC-02/SC-03 绑定方案的验收。

## 本地配置与使用

在 Nextcloud 个人安全设置中新建专用应用密码，然后在本机输入；不要放入聊天、命令参数、受版本管理的文件或日志。
Nextcloud 密码只留在伴随服务端。不要通过当前普通 BLE 设置协议发送它或服务 bearer token。
专用应用密码在 Nextcloud 中仍可能访问待办之外的数据；适配器方法限制不等于服务端限制了凭据权限。

使用受版本管理源码之外的私密目录，例如 `build/nextcloud/private/`。
POSIX 下目录使用 700、配置/数据库文件使用 600 权限；Windows 下将 ACL 限制为所有者与系统管理员。
配置操作不覆盖现有文件。`configure` 隐藏登录名和密码输入，并生成不同的随机设备/管理令牌，不将其打印出来。

```bash
python3 -m venv build/nextcloud/venv
source build/nextcloud/venv/bin/activate
python -m pip install --require-hashes --only-binary=:all: -r companion/requirements.lock
python -m companion.tasks --config build/nextcloud/private/config.json configure
```

下例用 `$task_args` 简写公共参数，采用数组保证路径仍为独立参数：

```bash
task_args=(--config build/nextcloud/private/config.json --database build/nextcloud/private/tasks.sqlite3)
python -m companion.tasks "${task_args[@]}" discover
python -m companion.tasks "${task_args[@]}" collections
python -m companion.tasks "${task_args[@]}" select --revision 0 <collection-id>
python -m companion.tasks "${task_args[@]}" refresh
python -m companion.tasks "${task_args[@]}" status
python -m companion.tasks "${task_args[@]}" preview --port 8787
```

`collections` 是显式在本机查看私密列表名的命令，不要分享输出。
`discover`、`refresh` 和 `status` 仅打印数量、版本或状态。
`select` 可用零个 ID 表示主动选择空列表，并且必须提供当前选择版本。
`refresh` 是由 timer 或手动触发的有界读取任务；预览服务器只提供已经提交的缓存。
私密修正应用密码后，先运行 `resume-auth` 再重试。在 Nextcloud 撤销不再使用的应用密码；
删除本地配置不等于撤销凭据。

`preview` 仅监听 `127.0.0.1`，使用开发 WSGI 服务器，不能直接作为正式公网服务。
WSGI `API` 提供：

| 方法和路径 | 权限 | 结果 |
| --- | --- | --- |
| GET `/suite/v1/task-collections` | 设备或管理令牌 | 集合、选择和版本 |
| PUT `/suite/v1/task-selection` | 管理令牌 | `{collection_ids, revision}` 版本比较并更新 |
| GET `/suite/v1/tasks/snapshot` | 设备或管理令牌 | 有界缓存页；后续页需携带 `revision` 和 `offset` |
| POST `/suite/v1/task-completions` | 仅完成令牌 | 持久意图/回执；可选显式 `retry` |
| GET `/suite/v1/operations/{operation_id}` | 仅完成令牌 | 已保存操作回执 |

统一使用 `Authorization: Bearer ...`，不把令牌放入查询串、Cookie 或访问日志。
响应使用 `Cache-Control: no-store`。读取/管理令牌不能完成任务；完成令牌不能读取快照或修改选择。
快照 `read_only` 表示读取 API，逐条 `completion_allowed` 表示独立写权限的适用资格。

## 历史读取验证与交接

下文服务端检查点及设备建议描述上一增量；当前设备实现及验证边界见
[设备阅读器记录](passport-device-tasks.zh_CN.md)。

`bash tools/test-companion.sh` 使用锁定哈希的隔离 Python 依赖运行模拟 host tests，
依赖位于被忽略的 `build/tooling/companion/`。仓库全门禁也会调用该脚本。
依赖缓存按 Python 版本和 lockfile 哈希区分，不替换 ESP-IDF/系统包。
可以通过 `PIP_NO_INDEX=1` 和 `PIP_FIND_LINKS` 指定离线 wheel 目录，仍必须校验依赖哈希。

日历解析使用 [icalendar 7.3.0](https://icalendar.readthedocs.io/en/stable/)，XML 解析使用
[defusedxml 0.7.1](https://github.com/tiran/defusedxml)。传递依赖和 wheel SHA-256 固定在
[`companion/requirements.lock`](../../companion/requirements.lock)。

测试覆盖发现、VTODO 解析、时区/日期形式、备注截断、不安全链接/重定向/实体、响应和数量上限、
缺失属性、重复标识、离线/过期/认证状态、重启、事务回滚、选择竞态、绑定版本的分页及设备/管理 API 权限。
这些模拟数据不满足真实 Nextcloud SA-11 或设备验收。SQLite 回滚/重新打开测试不是实体断电测试。

2026-10-19 验证检查点：

- **Build: PASS** — 在 ESP-IDF 5.5.3 下完成 `./tools/validate.sh`，随后独立核验归档。
  固件版本 `21c5110-dirty`，完整镜像 SHA-256
  `8285859fa1c230379312b685a542f47ebc323b09d988f008538cf5ce82a84097`，
  对应 ELF SHA-256 `a311c62c08cea2ac72f472439d76ea8fa776f46683a0b7b9f19b7d6e8ef16354`。
  归档位于 `build/firmware/<完整镜像-sha256>/`，未刷写该镜像。
- **Host tests: PASS** — 仓库/静态回归门禁及最新 27 项伴随服务测试通过，包含 Linux Gunicorn HTTP 启动。
  封存服务包与当前第一方源码逐文件一致。最终文档检查也通过；真实服务器/HTTPS 证据在上文单独记录。
- **Device tests: NOT RUN**（Nextcloud）— 设备仍运行此前验收的星轨版本，其验收结果不转移为本功能通过。
- **Unverified：** 设备界面、受保护令牌配置、持久缓存/掉电、动态字体显示和集成 RAM/Flash 余量；
  真实重复、共享、子任务、浮动日期兼容性；标记完成/冲突重放；服务器重启/负载/回滚及备份恢复。
  自动刷新及设备刷新操作尚未实现。

此前星轨/验收提交已推送。本次 Nextcloud 增量尚未提交；私密配置、日志、任务数据及生成产物均留在受版本管理源码之外。

私密账户配置、显式列表选择、通过 ESA 的真实读取及获准的服务端部署均已完成。
证据分别记录模拟测试、本机真实读取和部署后 HTTPS 检查，均不代表实体设备已下载。
接入设备前明确 Flash 缓存布局和掉电安全更新策略：当前 24 KiB NVS 不能用来存放 1 MiB 任务缓存。
根据实测内部 RAM 为 TLS、任务页解析和动态中文字形安排预算。真实快照已超过 NVS 容量，
标题中有当前 UI 字符集未声明的字符。没有将私密任务文本或派生字符清单导出到源码/字体资产。
展示真实私密待办前，先实现受保护的设备令牌配置路径。
任何新固件都需要单独完整门禁、核验归档及准确镜像刷写许可。

下一设备增量按以下顺序实施：

| 工作 | 当前证据与所需结果 |
| --- | --- |
| 存储与字体（Q-05） | 实测 JSON 快照为 148,940 字节；当前生成字体缺少本次数据中的 275 个不同字符，与随仓库提供的 CJK 字体合用仍缺 138 个。先选定有许可的公开字符集，测量生成字体/镜像，再明确应用专用缓存分区与掉电安全提交流程，然后修改分区。保留 Wi-Fi/城市 NVS 的偏移。这些数量不能证明支持任意标题。 |
| 设备授权（Q-02/03/04/11） | 已部署的设备 bearer token 仅有读取权限，但仍是秘密。传输前实现并审阅受保护的配置路径及 TLS/时间检查；不得写进固件、二维码、普通 BLE 设置或源码。所有者登录/恢复不属于本次部署。 |
| 下载与持久化 | 增加后台同步任务，保证有界分页解析、版本检查、取消和迟到结果抑制。只有完整核验并提交的缓存才能生效；设备下载时间与服务端读取时间分别显示。先用模拟数据验证写入中断，再检查真实数据/设备。 |
| 界面与验收 | 仅替换 `main/suite_launcher.c` 中待办入口的占位页，提供列表/详情/刷新页面、明确只读与离线/过期/认证状态，并限制滚动资源。在准确获准固件上验证动态中文、堆/栈余量、Wi-Fi/城市配置保留及重启恢复缓存。标记完成仍是后续增量。 |

当前 `main/suite_storage.c` 仅承载小型 NVS 设置；现有初始化逻辑可能在报告布局/版本故障时擦除 NVS。
新待办缓存后端不能因任务缓存故障而走这条恢复路径擦除设置。本次尚未实施这些固件或存储修改。

本地字体可行性检查使用 Noto Sans SC Regular 2.004（源文件 SHA-256
`faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9`）、
[lv_font_conv 1.5.3](https://github.com/lvgl/lv_font_conv)、16 px、不压缩位图和快速分类字距。
公开请求范围为 U+0020–007E、U+2000–206F、U+3000–303F、U+4E00–9FFF 和 U+FF00–FFEF。
两个候选均生成 21,393 个码点；源字体缺少请求范围中的 110 个码点，因此不声称完整覆盖这些范围。
两个候选均覆盖本次私密数据用字，但没有导出私密文本或由其派生的字符清单。

| 候选 | RISC-V 对象只读字节 | 位图字节 | 可写静态字节 |
| --- | --- | --- | --- |
| 4 位灰度 | 2,920,353 | 2,531,072 | 0 |
| 2 位灰度 | 1,656,220 | 1,266,939 | 0 |

两个独立对象均使用门禁的 IDF 5.5.3/LVGL 头文件及 `CONFIG_LV_FONT_FMT_TXT_LARGE=1` 编译通过；
其位图均超过默认 20 位索引范围。这些测量不包含最终镜像对齐、其他字体描述符的变化和运行时绘制/缓存分配。
2 位候选符合拟定的 2 MiB 字体上限，但仍需完成集成资源及显示效果验收。候选源码/对象留在忽略的构建输出中，未接入固件。
分发派生字体前，需要随附版权声明及[上游 OFL 1.1 许可](https://github.com/notofonts/noto-cjk/blob/main/Sans/LICENSE)。

## MiMo 语音待办候选，2026-10-21

用户选择 MiMo 按量 API 用于 SU-17。代码遵循[SC-06.1](passport-suite-contract.zh_CN.md#sc-061-语音新建待办)，
尚未部署；工具没有调用真实供应商或新建真实任务。已有完成令牌仅扩展到明确的
语音/创建接口，设备读取和管理选择权限继续分开。即使活动任务为零，也要求唯一
已选、属于当前账户且具有 DAV bind/write/all 权限的列表。未来部署后应运行来源
刷新任务以发现新权限标志；仅能完成的 write-content 权限不代表可创建。

在私密终端运行 `python3 tools/configure-task-speech.py --output <private-path>`，以无
回显输入普通按量 API Key。不要将密钥放入聊天、命令行参数、已跟踪文件、镜像或
设备配置。默认 `daily_seconds` 为 300，可设 1..3600。服务端将生成文件安装为
`/etc/passport-tasks/speech.json`，权限 0600，服务账户可读。确认确切候选后，再通过
systemd override 添加 `SUITE_TASKS_SPEECH_CONFIG=/etc/passport-tasks/speech.json`。
不新增 Python 依赖。可选语音配置缺失/无效时，已有待办仍可用，语音显示未配置。
沿用当前单 Gunicorn 进程、双 HTTP 线程；识别锁仅在单进程内生效。

`companion/deploy/caddy-route.example` 中精确的转写路径允许 384,044 字节；合并时
保留原配网页路由，不放宽其他请求上限。识别不保存音频文件。SQLite 用量日志在
外部调用前扣减，结果不明时不自动重试或退还预算。新建持久回执最多 1,024 条，
空间满时拒绝接收。生产部署、真实识别、麦克风/堆栈及设备新建/重放仍需验收。

## 创建前空查询修复，2026-10-21

用户确认识别标题和待同步新建请求经过正常重启仍保留；恢复网络后却出现暂停/
重试/待同步。服务端已有唯一一份持久新建回执，尝试五次后为 RETRY_PAUSED，
错误 INVALID_DAV；目标列表仍被选中、归属当前账户且可写，认证没有停止。只读
诊断确认新资源 URI 返回空 DAV multistatus。传输层要求恰有一份响应，因而在条件
创建之前拒绝。此前模拟资源直接返回 NOT_FOUND，没有覆盖真实传输返回。

候选仅允许创建前探测将合法的空 multistatus 当作不存在的提示；If-None-Match:*
仍是最终保护，写后和完成读取继续严格要求唯一精确资源。错误格式、其他资源和
多资源返回仍拒绝。新增回归覆盖真实传输、创建后读取、丢响应和已有资源竞争；
真实只读端点探测已确认行为差异。不需新意图或重新识别。原选中列表、凭据、语音
Key/预算、缓存及设备固件不变。完整门禁已通过（61 条服务端用例，零跳过），两文件服务端修复已部署。HTTPS
健康及令牌权限通过；原暂停请求、三份完成回执、设置、密钥和五秒语音预算记录
不变。设备仍为 10f8cb21/d7654fa9（完整镜像/ELF 前缀）；本次额外门禁产物
4e5beb17/73a04243 已归档但未刷写。随后设备重试及独立单资源核验已通过，见下方验收记录。

## 语音新建验收，2026-10-21

服务端修复后，用户仅重试原已确认请求一次，设备显示“任务已新建，OK继续”，
网页版出现新任务。独立只读 PROPFIND/REPORT 核验确认恰有一条匹配资源及一份
SUCCEEDED 新建回执；UID、目标列表和标题均与持久确认意图一致，状态为
NEEDS-ACTION。验证证书的 HTTPS 快照现有一条活动任务。原三份完成回执保留；
服务端接收预算仍为五秒音频，此次重放没有重新识别。核验未修改 Nextcloud。

Build: PASS。Host tests: PASS（完整门禁，含 61 条服务端用例，零跳过）。
Device tests: PASS，范围为真实麦克风/MiMo 识别、中文预览/默认取消、识别后离线
保存、正常重启保留，以及原请求联网重试最终仅创建一条真实任务。Wi-Fi/城市设置、
凭据、密钥、缓存及完成日志保留。设备仍为 10f8cb21/d7654fa9，无需再次刷写。

Unverified: 实测录音停止延迟及运行时堆栈峰值、语音/BLE/天气混合使用、长期运行、
物理断电和真实网页并发编辑。语音识别需要联网；本次离线验收仅覆盖识别后提交。
成功页面可按 OK 继续正常使用。

## 源数据新鲜度排查，2026-10-22

可靠性固件刷入后，用户确认原任务仍可见，但最初一次刷新提示失败并保留缓存。
该次失败没有对应串口日志，原因仍未确认。随后捕获的重试为
`Refresh result=2, cached=1`，没有错误日志：设备下载和缓存激活成功，但这不证明
源数据已更新。

服务器已提交快照距源读取 80,792 秒，含 151 条源记录、一条活动任务；仅选中一个
列表，选择版本为 2，没有认证暂停或已记录的源错误。随后执行既有的一次性
`passport-tasks-refresh.service` 成功，发布 160 条源记录、六条活动任务；选择
未变，没有写入 Nextcloud 任务。独立的读取令牌 API 检查返回 HTTP 200、`READY`，
一个 6,200 字节页面包含六条任务，无后续页，响应为 `Cache-Control: no-store`。
用户随后确认六条最新任务在设备上显示正确；后续被动观察窗口未捕获新的刷新动作。

OpenCode Go 的 `deepseek-v4.1-flash` 默认 variant 在会话
`ses_e9cb86cbaffeukdNjpG6M7gxNb` 中只读评估了源同步方式，Codex 核验模型元数据、
本地实现和仅含计数的在线证据。用户选择五分钟服务端 timer，调用既有 oneshot 任务；
设备触发源读取需要另行设计 API/权限。timer 已部署并启用，详见下方实测记录。
按上次任务结束计时的调度，不保证源失败时仍保持五分钟新鲜度。生产环境的手动读取
应通过同一个服务单元启动，避免直接 CLI 并行读取。

设备仍为 `7c292cfc/e8b6ae41`，没有再次刷写、重写配置/令牌、替换应用源码或写入
Nextcloud 任务。本地脱敏摘要及私密有界串口日志位于
`build/acceptance/20261022-task-refresh/`。

Build: PASS，适用于不变的已核验固件。
Host tests: PASS，来自其完整门禁；本次排查没有修改源码。
Device tests: PASS，仅限原缓存可见、捕获到的一条任务下载，以及用户确认的六条任务
显示；此前刷写/启动子集仍单独记录。
Unverified: 最初刷新失败原因、录音/预览/取消、错误文案渲染、实测时延/内存峰值、
掉电和混合运行。

### 五分钟 timer 部署，2026-10-22

用户选定的 timer 由 OpenCode Go 的 `deepseek-v4.1-flash` 默认 variant 在会话
`ses_e9cb86cbaffeukdNjpG6M7gxNb` 中实现，随后由 Codex 独立复核并部署。service
仅修改 Description；oneshot 命令、账户、路径、加固和 30 分钟时限均不变。已部署
的 timer SHA-256 为
`30895929af695494450e066bacd31bf9f6d2f9d6efc3e8fadbb5a5e254a0569d`；
service SHA-256 为
`3dd65f5ea8862e2a1a133b1f5700f21202b1db1d1d770ba37241870994066f24`。
替换前已保留原 service 副本，仅安装这两个 systemd 单元文件；没有替换 API、
Caddy、应用代码或数据库结构。

实际生效配置和开机启用链接已核验：`OnBootSec=5min`、`OnUnitInactiveSec=5min`、
`AccuracySec=1s`，目标为既有 oneshot 服务。连续两轮自动读取均成功退出，两次
快照发布时间相隔 304 秒；timer 触发及服务启动时间均已推进，下一轮仍处于等待调度。
最新核验的 API 返回 HTTP 200、`READY`，含七条活动任务。此前设备显示六条已由
用户确认；服务端计数不代表新增了一份设备显示回执。

首次在线读取期间的 17 次 API 请求全部为 200，观测到的最大时延为 220.44 ms，
API 进程没有重启。两轮观察前后，配置/令牌字节、权限、所选列表/版本、认证状态、
完成和新建回执以及语音用量均与部署前哈希一致。读取没有修改 Nextcloud 任务。

同一服务器的隔离临时单元测试先故意失败一次，随后自动重试；后续四秒任务使用
两秒的结束后间隔，未发生重叠，同一活动单元的重复手动启动被合并。临时测试已停止，
没有调用生产读取器。既有主机回归覆盖源失败/回滚、认证暂停和缓存保留；没有向生产
注入错误凭据或源故障。

完整 `./tools/validate.sh` 已通过，包含 64 条 companion 用例且零跳过、运行时测试、
固件布局及归档验证。本地和服务端的独立单元验证均通过。额外门禁构建仅用于验证：
完整镜像 SHA-256 为 `21c12bfb12befb43f52274617ed5c60feae573f5c237f2d711217daef45a9a90`，匹配 ELF SHA-256 为
`f90c023d8da41412f9c1049fb9e4b4832b8dd4fcd427fd9a7e83a9eb9aa50228`，归档于
`build/firmware/21c12bfb12befb43f52274617ed5c60feae573f5c237f2d711217daef45a9a90/`，没有刷写。设备仍为
`7c292cfc/e8b6ae41`。本地证据和已审阅回滚步骤保存在
`build/acceptance/20261022-refresh-timer/`。

Build: PASS。Host tests: PASS。
Server synchronization: PASS，范围为两次自动读取、生效配置、API 可用性、状态保留
及隔离调度测试。
Device tests: PASS，仅限用户已确认的六条任务下载/显示，设备固件不变，无需再次刷写。
Unverified: 最初偶发设备刷新失败原因、录音/预览/取消、错误文案渲染、实测设备时延/
内存峰值、掉电和混合运行；没有执行真实服务器重启或强制 30 分钟生产超时。
