[English](README.md) · 简体中文

# Passport 配套服务器(D1a)

为行程 D1 数据契约 passport-travel-v1-draft(docs/assets/passport-travel-contract.zh_CN.md)提供的本地单用户测试服务器。除 Python 3.11+ 标准库外无运行时依赖;SQLite 是工作数据库,WebDAV 只保存不可变资源与完整快照。允许本地测试实例;生产部署需另行授权,且应由终止 TLS 的反向代理在前。

## 启动本地测试实例

```bash
cd companion/passport-server
python3 -m passport_server --data-dir ./passport-data --port 8642
# 首次运行打印一次管理员密码;请私下保存
```

网页:`http://127.0.0.1:8642/`(登录 → 资源包发布、设备绑定、记录/备份状态、恢复)。设备 API 按契约位于 `/api/v1/...`。测试中的 WebDAV 假服务器:`passport_server.devdav`;适配器 `passport_server.webdav` 同样可对接真实服务器。

## 测试

```bash
cd companion/passport-server && python3 -m unittest discover -s tests
```

同样作为配套门禁接入 `./tools/validate.sh --static`。

## 版本锁定

| 依赖 | 版本 | 说明 |
| --- | --- | --- |
| Python | ≥ 3.11(已验证 3.14) | 仅标准库:sqlite3、http.server、http.client、hashlib |
| SQLite | Python sqlite3 模块内置版本 | WAL + `synchronous=FULL` |
| lv_font_conv | 1.5.3 | 固件字体工具(Node ≥ 18),见 `assets/fonts/README.md` |
| Noto Sans CJK SC | OFL 1.1 | 字体源,已记录 SHA-256 |

## D1 安全边界

- 单用户;管理端在每个数据库中初始化一次,由运维控制(无公开未认证引导;恢复后引导会重置认证)。
- 网站会话:HttpOnly Cookie + 每个 POST 的 CSRF 令牌;密码 PBKDF2-SHA-256(10 万轮,每库独立盐)。
- 设备令牌只在确认时交付一次;服务器长期只保存摘要;撤销会删除恢复材料,重绑定后重新获取。
- 绑定码:31 字符表 6 位,5 分钟有效,错误 5 次作废;错误尝试按码限速。
- TLS:从不关闭证书/主机名/有效期校验。本地回环测试用 HTTP;生产设备流量走 HTTPS 且设备校验链与主机名。
- 密码/绑定码/令牌从不写入日志、快照、测试或仓库。

## 恢复行为

快照先在隔离暂存数据库中验证;空服务器确认后换入恢复数据库:新建 `server_epoch`、把已存储设备标记为待重绑、要求重新初始化管理员、并重新下载引用资源。`complete.json` 是最终标记;缺少它的快照永远不会被恢复看到。live SQLite 绝不放在远程挂载上。

## 已知 D1 边界

- 真实 WebDAV 验收(A06 实网部分)需要真实服务器;mock 与真实证据分开。
- OTA、多用户、配额看板与铁路/AI 适配器属于后续阶段。
- OTP 仅保留独立加密备份边界(无验证码、无真实种子)。
