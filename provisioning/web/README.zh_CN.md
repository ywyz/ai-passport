<p align="right"><strong>简体中文</strong> · <a href="README.md">English</a></p>

# 手机网页 BLE 配网页

FoloToy AI Passport 的纯静态、无构建配网网页。页面运行在手机浏览器中,通过
Web Bluetooth 连接设备,完成 Wi-Fi 扫描与凭据保存、省市选择。

## 文件说明

- `index.html` —— 手机优先单屏页面(响应头要求 no-cache)。
- `app.js` —— Web Bluetooth 连接、分包协议与仪表盘,无任何第三方依赖。
- `style.css` —— 手机端深色样式。
- `deploy.sh` —— 上传三个文件到服务器并 reload Caddy,可重复执行。

## 浏览器要求

- 必须通过 **HTTPS** 打开(除 `localhost` 外,HTTP 下 Web Bluetooth 被禁用)。
- 需要 `navigator.bluetooth` 能力:**Android 版 Chrome / Edge 或其他
  Chromium 内核浏览器**。iOS/iPadOS 的 Safari 不支持 Web Bluetooth,页面会
  明确提示用户换用安卓 Chromium 浏览器。
- 系统可能要求授予蓝牙与定位权限,用于弹出设备选择列表。

## 用户流程

1. 设备:功能列表 → 设置 → **手机配网**。屏幕显示二维码(内含网页 URL 与
   设备 ID)、本次随机生成的 6 位配对码,并开始 BLE 广播。
2. 扫码打开 `https://passport.ywyz.tech/?d=<设备ID>`。
3. 点“连接设备”,选择名为 `FoloPassport-XXXX` 的设备,输入屏幕上的配对码。
4. 扫描周边 Wi-Fi、选择网络、输入密码并保存。页面通过状态事件实时显示
   `connecting` → `online`;失败时给出中文原因,如密码错误(reason 15)、
   找不到网络(reason 200/201)。
5. 在两级下拉中选择省份与城市并保存;设备写入 NVS 后触发天气刷新。

退出设备上的配网页即停止广播并断开手机连接。

## GATT 服务(固定 128 位 UUID)

| 角色 | UUID |
| --- | --- |
| 服务 | `7a110001-4f1a-8c20-50a7-b6e900000001` |
| RX(手机→设备,Write / Write Without Response) | `7a110002-4f1a-8c20-50a7-b6e900000002` |
| TX(设备→手机,Notify) | `7a110003-4f1a-8c20-50a7-b6e900000003` |
| Info(Read) | `7a110004-4f1a-8c20-50a7-b6e900000004` |

广播名 `FoloPassport-XXXX`,`XXXX` 为 BLE MAC 末两字节的大写十六进制,
与网址中的 `d` 参数一致。

## 分包协议

每帧格式为 `[flags:1][seq:1][payload]`:

- flags bit0=END,bit1=START;单包 `0x03`、首包 `0x02`、中间包 `0x00`、
  末包 `0x01`。
- START 帧重置重组会话;seq 逐帧递增,重复序号丢弃;半条消息 3 秒超时清空。
- 重组结果为一条 UTF-8 JSON 消息。
- 浏览器无法协商 ATT MTU,固定按 180 字节净荷发送(整帧 182 字节);
  设备端则按协商 MTU 自适应分片。

消息目录(以固件 `main/suite_prov_ble.c` 为准):

- 请求:`{"id":n,"cmd":"hello","pin":"123456"}`、`status`、`wifi_scan`、
  `{"id":n,"cmd":"wifi_set","ssid":"…","pass":"…"}`、`provinces`、
  `{"id":n,"cmd":"cities","p":0}`、`{"id":n,"cmd":"city_set","p":9,"c":4}`。
- 应答:`{"id":n,"ok":true,"data":{…}}` 或
  `{"id":n,"ok":false,"err":"bad_pin|bad_arg|scan_busy|…"}`。
- 事件(无 id):
  `{"ev":"status","data":{"wifi":"idle|connecting|online|fail","ssid":"ab***","ip":"…","rssi":-55,"reason":15,"time_synced":true,"city":{"n":"…","la":298683,"lo":1215440}}}`。

`wifi_scan` 最多返回 20 条同名去重后的网络
(`{"nets":[{"ssid","rssi","sec"}]}`,`sec` 取 `WIFI_AUTH_*` 枚举序号)。
配对码连续错误 3 次,设备主动断开连接。

## 安全基线

BLE 链路使用 Just Works,不防中间人;授权仅依赖屏幕上的 6 位配对码。这是
防止附近人员随手连入的基础防护,不是完整的授权与加密设计。固件日志不会
打印 Wi-Fi 密码,密码也不放入 URL;配对码只出现在设备屏幕上。

## 手动部署

页面为纯静态文件,任意支持 HTTPS 的静态服务器均可。项目服务器使用 Caddy:
把三个文件上传到 `/var/www/passport/`,在 `/etc/caddy/Caddyfile`(先确认实际
配置路径)中加入站点:

```caddy
passport.ywyz.tech {
    root * /var/www/passport
    encode gzip
    file_server
    @index not file
    header / Cache-Control "no-cache"
}
```

然后执行 `systemctl reload caddy`(或
`caddy reload --config /etc/caddy/Caddyfile`)。

验证:

- `curl -I https://passport.ywyz.tech/` 返回 `200` 且证书有效;
- `curl -I http://passport.ywyz.tech/` 自动跳转 HTTPS;
- Android Chrome 无痕窗口打开无混合内容告警。

幂等脚本 `deploy.sh` 会上传文件并 reload Caddy(可用 `DEPLOY_HOST`、
`REMOTE_DIR` 覆盖默认值);Caddy 站点块只需手动添加一次。
