/*
 * provisioning/web/app.js —— AI Passport 手机 BLE 配网页(原生 JS,无构建)。
 *
 * 帧协议与设备端 main/suite_prov_proto.c 严格一致:
 *   每包 = [flags:1][seq:1][payload]
 *   flags bit0=END bit1=START(单包 0x03 / 首包 0x02 / 中间 0x00 / 末包 0x01)
 *   START 重置重组会话;seq 递增用于去重;半条消息 3s 超时丢弃。
 *
 * 安全说明:链路层 Just Works,授权仅靠设备屏幕上的 6 位配对码,
 * 属于基础防护,不是完整授权设计。
 */
'use strict';

// ---- 固定 UUID(与固件 main/suite_prov_ble.c 一致) ----
const SERVICE_UUID = '7a110001-4f1a-8c20-50a7-b6e900000001';
const RX_UUID = '7a110002-4f1a-8c20-50a7-b6e900000002'; // 手机 → 设备(写)
const TX_UUID = '7a110003-4f1a-8c20-50a7-b6e900000003'; // 设备 → 手机(通知)
const INFO_UUID = '7a110004-4f1a-8c20-50a7-b6e900000004';

// Web Bluetooth 无法协商 ATT MTU,统一按 180 字节净荷分片(含帧头 182B)。
const CHUNK_PAYLOAD = 180;
const FLAG_END = 0x01;
const FLAG_START = 0x02;
const REASM_TIMEOUT_MS = 3000;

const $ = (id) => document.getElementById(id);

// ============ 分包发送 ============

function buildFrames(jsonText) {
  const bytes = new TextEncoder().encode(jsonText);
  const frames = [];
  const total = bytes.length;
  if (total === 0) return frames;
  const frameCount = Math.ceil(total / CHUNK_PAYLOAD);
  let offset = 0;
  for (let i = 0; i < frameCount; i++) {
    const chunk = bytes.subarray(offset, Math.min(offset + CHUNK_PAYLOAD, total));
    let flags = 0;
    if (frameCount === 1) flags = FLAG_START | FLAG_END;
    else if (i === 0) flags = FLAG_START;
    else if (i === frameCount - 1) flags = FLAG_END;
    const frame = new Uint8Array(chunk.length + 2);
    frame[0] = flags;
    frame[1] = i & 0xff;
    frame.set(chunk, 2);
    frames.push(frame);
    offset += CHUNK_PAYLOAD;
  }
  return frames;
}

// ============ 分包重组(镜像设备端状态机) ============

class Reassembler {
  constructor() {
    this.resetSession();
    this.chunks = [];
  }
  resetSession() {
    this.chunks = [];
    this.seq = 0;
    this.haveSeq = false;
    this.active = false;
    this.startMs = 0;
  }
  // 收到一帧;返回组装好的 Uint8Array,或 null(继续等/已丢弃)。
  feed(nowMs, data) {
    if (data.byteLength < 2) return null;
    const flags = data.getUint8(0);
    const seq = data.getUint8(1);
    const payload = new Uint8Array(data.buffer, data.byteOffset + 2,
                                   data.byteLength - 2);

    if (flags & ~(FLAG_START | FLAG_END)) return null; // 保留位非法
    const isStart = (flags & FLAG_START) !== 0;
    const isEnd = (flags & FLAG_END) !== 0;

    if (isStart) {
      // START(含单包)总是开新会话。
      this.chunks = [];
      this.seq = seq;
      this.haveSeq = true;
      this.active = !isEnd;
      this.startMs = nowMs;
    } else {
      if (!this.active || !this.haveSeq) return null;
      if (seq === this.seq) return null; // 重复包幂等忽略
      if (seq !== ((this.seq + 1) & 0xff)) {
        this.resetSession(); // 乱序/丢包:会话无法续上
        return null;
      }
      this.seq = seq;
    }

    this.chunks.push(payload);
    if (isEnd) {
      const out = concatChunks(this.chunks);
      this.resetSession();
      return out;
    }
    return null;
  }
  tick(nowMs) {
    if (this.active && nowMs - this.startMs > REASM_TIMEOUT_MS) {
      this.resetSession();
    }
  }
}

function concatChunks(chunks) {
  const len = chunks.reduce((n, c) => n + c.length, 0);
  const out = new Uint8Array(len);
  let off = 0;
  for (const c of chunks) {
    out.set(c, off);
    off += c.length;
  }
  return out;
}

// ============ 连接与协议会话 ============

const state = {
  device: null,
  server: null,
  rxChar: null,
  txChar: null,
  writeWithoutResponse: false,
  writeBusy: false,
  writeQueue: Promise.resolve(),
  reasm: new Reassembler(),
  reasmTimer: null,
  nextId: 1,
  pending: new Map(), // id -> {resolve, reject, timer}
  authed: false,
  selectedSsid: null,
  selectedSec: 0,
  provinces: [],
};

function urlDeviceId() {
  return new URLSearchParams(location.search).get('d') || '';
}

function show(id, visible) {
  $(id).classList.toggle('hidden', !visible);
}

function setMsg(id, text, isError) {
  const el = $(id);
  el.textContent = text || '';
  el.classList.toggle('err', !!isError);
}

// 发送请求并等待应答(带超时)。
function request(cmd, extra, timeoutMs) {
  const id = state.nextId++;
  const payload = Object.assign({ id, cmd }, extra || {});
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      state.pending.delete(id);
      reject(new Error('设备响应超时,请重试'));
    }, timeoutMs || 15000);
    state.pending.set(id, { resolve, reject, timer });
    enqueueWrite(JSON.stringify(payload)).catch((e) => {
      clearTimeout(timer);
      state.pending.delete(id);
      reject(e);
    });
  });
}

// 写特征串行化(Write Without Response 不允许并发压栈)。
function enqueueWrite(text) {
  const job = state.writeQueue.then(() => writeFramesNow(text));
  state.writeQueue = job.catch(() => {});
  return job;
}

async function writeFramesNow(text) {
  const frames = buildFrames(text);
  for (const frame of frames) {
    if (state.writeWithoutResponse) {
      await state.rxChar.writeValueWithoutResponse(frame);
    } else {
      await state.rxChar.writeValueWithResponse(frame);
    }
  }
}

function onNotify(event) {
  const data = event.target.value; // DataView
  const msg = state.reasm.feed(Date.now(), data);
  if (msg) handleMessage(msg);
}

function handleMessage(bytes) {
  let obj;
  try {
    obj = JSON.parse(new TextDecoder().decode(bytes));
  } catch (e) {
    return; // 非法 JSON 静默丢弃,等下一帧
  }
  if (obj.ev === 'status') {
    renderStatus(obj.data || {});
    for (let i = statusWaiters.length - 1; i >= 0; i--) {
      const w = statusWaiters[i];
      if (w.predicate(obj.data || {})) {
        clearTimeout(w.timer);
        statusWaiters.splice(i, 1);
        w.resolve(obj.data);
      }
    }
    return;
  }
  if (typeof obj.id !== 'number') return;
  const p = state.pending.get(obj.id);
  if (!p) return;
  clearTimeout(p.timer);
  state.pending.delete(obj.id);
  if (obj.ok) p.resolve(obj.data);
  else p.reject(new Error(deviceErrText(obj.err)));
}

// 设备错误码 → 面向普通用户的中文。
function deviceErrText(err) {
  switch (err) {
    case 'bad_pin': return '配对码错误,请核对设备屏幕';
    case 'need_pin': return '请先完成配对码验证';
    case 'bad_arg': return '参数有误,请重试';
    case 'scan_busy': return '设备正在扫描,请稍候再试';
    case 'scan_failed': return '设备读取扫描结果失败,请重试';
    case 'conn_busy': return '设备连接忙,请稍后重试';
    case 'storage': return '设备保存失败,请重试';
    case 'no_mem': return '设备内存不足,请退出页面重进';
    case 'bad_json': return '数据错误,请重连';
    case 'bad_cmd': return '不支持的操作';
    default: return '设备返回错误: ' + (err || '未知');
  }
}

// 等待下一条满足条件的状态事件。
const statusWaiters = [];
function waitForStatus(predicate, timeoutMs) {
  return new Promise((resolve, reject) => {
    const entry = { predicate, resolve, timer: null };
    entry.timer = setTimeout(() => {
      const i = statusWaiters.indexOf(entry);
      if (i >= 0) statusWaiters.splice(i, 1);
      reject(new Error('等待设备状态超时'));
    }, timeoutMs);
    statusWaiters.push(entry);
  });
}

// ============ 连接流程 ============

async function connectDevice() {
  setMsg('connect-msg', '正在打开蓝牙选择器…');
  try {
    if (!state.device) {
      state.device = await navigator.bluetooth.requestDevice({
        filters: [{ services: [SERVICE_UUID] }],
        optionalServices: [],
      });
      state.device.addEventListener('gattserverdisconnected', onDisconnected);
    }
    setMsg('connect-msg', '正在连接 ' + (state.device.name || '设备') + ' …');
    state.server = await state.device.gatt.connect();
    const svc = await state.server.getPrimaryService(SERVICE_UUID);
    state.rxChar = await svc.getCharacteristic(RX_UUID);
    state.txChar = await svc.getCharacteristic(TX_UUID);
    state.writeWithoutResponse =
      state.rxChar.properties.writeWithoutResponse === true;
    await state.txChar.startNotifications();
    state.txChar.addEventListener('characteristicvaluechanged', onNotify);

    // 读静态信息并核对 URL 里的设备 ID(仅提醒,不强制)。
    try {
      const infoChar = await svc.getCharacteristic(INFO_UUID);
      const info = JSON.parse(new TextDecoder().decode(
        await infoChar.readValue()));
      $('device-id').textContent = '设备 ' + (info.id || '?') +
        (info.fw ? ' · 固件 ' + info.fw : '');
      const want = urlDeviceId();
      if (want && info.id && want.toUpperCase() !== info.id.toUpperCase()) {
        setMsg('connect-msg', '提示:连接的设备与二维码 ID 不同,请确认', false);
      }
    } catch (e) { /* 信息特征读失败不阻塞 */ }

    state.authed = false;
    state.reasm.resetSession();
    setMsg('connect-msg', '已连接,请输入配对码');
    show('pin-card', true);
    $('pin-input').value = '';
    $('pin-input').focus();
    show('disconnect-bar', false);
  } catch (e) {
    if (e && e.name === 'NotFoundError') {
      setMsg('connect-msg', '未选择设备');
    } else {
      setMsg('connect-msg', '连接失败:' + friendlyError(e), true);
    }
  }
}

function onDisconnected() {
  state.authed = false;
  show('dashboard', false);
  show('pin-card', false);
  show('disconnect-bar', true);
  for (const [, p] of state.pending) {
    clearTimeout(p.timer);
    p.reject(new Error('连接已断开'));
  }
  state.pending.clear();
}

async function sendHello() {
  const pin = $('pin-input').value.trim();
  if (!/^\d{6}$/.test(pin)) {
    setMsg('pin-msg', '请输入 6 位数字配对码', true);
    return;
  }
  setMsg('pin-msg', '验证中…');
  try {
    await request('hello', { pin }, 8000);
    state.authed = true;
    show('pin-card', false);
    show('dashboard', true);
    setMsg('connect-msg', '配对成功');
    await enterDashboard();
  } catch (e) {
    // 连续 3 次错误时设备会主动断开,断开提示条随后自动出现。
    setMsg('pin-msg', e.message, true);
  }
}

async function enterDashboard() {
  try {
    const st = await request('status', {}, 8000);
    renderStatus(st);
  } catch (e) { /* 状态事件稍后会推送 */ }
  await loadProvinces();
}

// ============ 状态卡渲染 ============

function wifiText(wifi) {
  return ({
    idle: '待配网',
    connecting: '连接中',
    online: '已联网',
    fail: '连接失败',
  })[wifi] || wifi || '-';
}

function renderStatus(d) {
  const wifi = d.wifi || 'idle';
  $('st-wifi').textContent = wifiText(wifi);
  $('st-wifi').className = 'val ' +
    (wifi === 'online' ? 'ok' : wifi === 'fail' ? 'err' : 'warn');
  $('st-ssid').textContent = d.ssid || '-';
  $('st-ip').textContent = d.ip || '-';
  $('st-rssi').textContent = typeof d.rssi === 'number' ? d.rssi + ' dBm' : '-';
  $('st-time').textContent = d.time_synced ? '已同步' : '未同步';
  $('st-time').className = 'val ' + (d.time_synced ? 'ok' : 'warn');
  $('st-city').textContent = (d.city && d.city.n) || '-';
}

// ============ Wi-Fi 扫描与保存 ============

async function scanWifi() {
  setMsg('scan-msg', '正在扫描周边 Wi-Fi(最多 12 秒)…');
  $('net-list').innerHTML = '';
  try {
    const data = await request('wifi_scan', {}, 12000);
    const nets = data.nets || [];
    if (nets.length === 0) {
      setMsg('scan-msg', '没有发现可用网络,可换个位置再试', true);
      return;
    }
    setMsg('scan-msg', '找到 ' + nets.length + ' 个网络,请点选');
    nets.forEach((n) => {
      const li = document.createElement('li');
      li.className = 'net-item';
      const lock = n.sec ? '🔒' : '';
      li.innerHTML = '<span class="ssid">' + escapeHtml(n.ssid) + '</span>' +
        '<span class="meta">' + lock + ' ' + n.rssi + ' dBm</span>';
      li.addEventListener('click', () => selectNet(li, n));
      $('net-list').appendChild(li);
    });
  } catch (e) {
    setMsg('scan-msg', e.message || '扫描失败', true);
  }
}

function selectNet(li, net) {
  document.querySelectorAll('.net-item.sel')
    .forEach((el) => el.classList.remove('sel'));
  li.classList.add('sel');
  state.selectedSsid = net.ssid;
  state.selectedSec = net.sec;
  $('pass-wrap').classList.remove('hidden');
  $('btn-wifi-save').classList.remove('hidden');
  $('pass-input').value = '';
  $('pass-label').textContent = net.sec
    ? ('密码(' + secName(net.sec) + ')')
    : '开放网络(可不填)';
  setMsg('wifi-msg', '已选择 ' + net.ssid);
}

function secName(sec) {
  // 数值对应设备端 WIFI_AUTH_* 枚举序号。
  if (sec === 0) return '开放';
  if (sec === 1) return 'WEP';
  if (sec === 2) return 'WPA';
  if (sec === 3) return 'WPA2';
  if (sec === 4) return 'WPA/WPA2';
  if (sec === 6) return 'WPA3';
  if (sec === 7) return 'WPA2/WPA3';
  return '加密';
}

function failReasonText(reason) {
  // 与设备屏幕、wifi_event reason 数值一致。
  if (reason === 15 || reason === 16 || reason === 14) return '密码错误';
  if (reason === 200 || reason === 201) return '找不到网络';
  if (reason === 4 || reason === 8 || reason === 9) return '信号太差,请靠近路由器';
  return '连接失败(代码 ' + reason + ')';
}

async function saveWifi() {
  if (!state.selectedSsid) return;
  const pass = $('pass-input').value;
  if (state.selectedSec && !pass) {
    setMsg('wifi-msg', '该网络已加密,请输入密码', true);
    return;
  }
  setMsg('wifi-msg', '正在保存并连接…');
  try {
    await request('wifi_set', { ssid: state.selectedSsid, pass }, 8000);
  } catch (e) {
    setMsg('wifi-msg', e.message, true);
    return;
  }
  // 随后靠状态事件判定结果:成功拿到 IP=online;失败带 reason。
  try {
    const d = await waitForStatus((s) => s.wifi === 'online' || s.wifi === 'fail',
                                  30000);
    if (d.wifi === 'online') {
      setMsg('wifi-msg', '配网成功,设备已联网 ✓', false);
    } else {
      setMsg('wifi-msg', failReasonText(d.reason) + ',可修改后重试', true);
    }
  } catch (e) {
    setMsg('wifi-msg', '设备仍在尝试连接,请看设备屏幕状态', true);
  }
}

// ============ 省市选择 ============

async function loadProvinces() {
  const sel = $('province-select');
  try {
    const data = await request('provinces', {}, 10000);
    state.provinces = data || [];
    sel.innerHTML = '';
    state.provinces.forEach((p) => {
      const o = document.createElement('option');
      o.value = String(p.i);
      o.textContent = p.n;
      sel.appendChild(o);
    });
    sel.disabled = false;
    await loadCities();
  } catch (e) {
    sel.innerHTML = '<option>省市数据加载失败</option>';
  }
}

async function loadCities() {
  const p = parseInt($('province-select').value, 10);
  const sel = $('city-select');
  sel.innerHTML = '<option>加载中…</option>';
  try {
    const data = await request('cities', { p }, 10000);
    sel.innerHTML = '';
    (data || []).forEach((c) => {
      const o = document.createElement('option');
      o.value = String(c.i);
      o.textContent = c.n;
      sel.appendChild(o);
    });
    sel.disabled = false;
  } catch (e) {
    sel.innerHTML = '<option>城市加载失败</option>';
  }
}

async function saveCity() {
  const p = parseInt($('province-select').value, 10);
  const c = parseInt($('city-select').value, 10);
  if (isNaN(p) || isNaN(c)) return;
  setMsg('city-msg', '保存中…');
  try {
    await request('city_set', { p, c }, 8000);
    setMsg('city-msg', '城市已保存,设备将刷新天气');
  } catch (e) {
    setMsg('city-msg', e.message, true);
  }
}

// ============ 工具 ============

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, (m) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  }[m]));
}

function friendlyError(e) {
  if (!e) return '未知错误';
  if (e.name === 'SecurityError') return '请在 https 页面中使用蓝牙';
  if (e.name === 'NotAllowedError') return '蓝牙权限被拒绝';
  if (e.name === 'NetworkError') return '连接中断,请重试';
  return e.message || String(e);
}

// ============ 环境能力检测与绑定 ============

function checkSupport() {
  const reasons = [];
  if (location.protocol !== 'https:' &&
      location.hostname !== 'localhost' && location.hostname !== '127.0.0.1') {
    reasons.push('当前不是 HTTPS 环境(' + location.protocol + '//),Web Bluetooth 不可用。');
  }
  if (!navigator.bluetooth) {
    if (/iphone|ipad|ipod/i.test(navigator.userAgent)) {
      reasons.push('iPhone/iPad 的 Safari 与系统浏览器不支持 Web Bluetooth(苹果系统限制)。');
    } else {
      reasons.push('当前浏览器没有 Web Bluetooth 能力。');
    }
  }
  if (reasons.length) {
    show('unsupported', true);
    $('unsupported-reason').textContent = reasons.join(' ');
    show('app', false);
    return false;
  }
  return true;
}

function bind() {
  $('btn-connect').addEventListener('click', connectDevice);
  $('btn-reconnect').addEventListener('click', async () => {
    show('disconnect-bar', false);
    await connectDevice();
  });
  $('btn-hello').addEventListener('click', sendHello);
  $('pin-input').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') sendHello();
  });
  $('btn-scan').addEventListener('click', scanWifi);
  $('btn-wifi-save').addEventListener('click', saveWifi);
  $('province-select').addEventListener('change', loadCities);
  $('btn-city-save').addEventListener('click', saveCity);
}

// 重组半条消息超时维护。
setInterval(() => state.reasm.tick(Date.now()), 500);

(function init() {
  const d = urlDeviceId();
  $('device-id').textContent = d ? '二维码设备 ' + d : '';
  if (checkSupport()) bind();
})();
