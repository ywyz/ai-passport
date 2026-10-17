// tests/test_prov_web_contract.mjs —— 配网页与固件应答的契约测试。
//
// 直接运行 provisioning/web/app.js 的真实处理逻辑(Node vm + 最小 DOM 桩),
// 用设备实际会发出的 JSON 应答驱动:
//   - wifi_scan 返回一个/多个网络时正确渲染(含转义与多帧重组);
//   - 可以选择 SSID 并提交 wifi_set 凭据;
//   - 空列表与错误应答正确呈现;
//   - provinces / cities / city_set 等现有应答不受破坏。
//
// 运行:node tests/test_prov_web_contract.mjs(validate.sh 会执行)。
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const appSource = readFileSync(
  new URL('../provisioning/web/app.js', import.meta.url), 'utf8');
const decoder = new TextDecoder();

// ---- 最小 DOM/页面环境 ----

function makeElement(tag) {
  const el = {
    tagName: tag,
    textContent: '',
    value: '',
    disabled: false,
    children: [],
    listeners: Object.create(null),
    classList: {
      set: new Set(),
      contains(name) { return this.set.has(name); },
      add(name) { this.set.add(name); },
      remove(name) { this.set.delete(name); },
      toggle(name, force) {
        if (force === undefined) {
          if (this.set.has(name)) this.set.delete(name);
          else this.set.add(name);
        } else if (force) {
          this.set.add(name);
        } else {
          this.set.delete(name);
        }
      },
    },
    appendChild(child) {
      this.children.push(child);
      // 原生 <select> 追加第一个 option 后 value 自动选中它。
      if (this.tagName === 'select' && this.value === '' &&
          child.tagName === 'option') {
        this.value = String(child.value);
      }
      return child;
    },
    addEventListener(type, fn) {
      (this.listeners[type] ||= []).push(fn);
    },
    focus() {},
    click() {
      for (const fn of this.listeners.click || []) fn();
    },
  };
  let html = '';
  Object.defineProperty(el, 'innerHTML', {
    get: () => html,
    set(v) {
      html = v;
      if (v === '') el.children.length = 0;  // 清空列表语义
    },
  });
  return el;
}

function createPage() {
  const elements = new Map();
  const timers = new Map();
  let nextTimer = 1;
  const element = (id) => {
    if (!elements.has(id)) {
      elements.set(id, makeElement(id.endsWith('-select') ? 'select' : 'div'));
    }
    return elements.get(id);
  };

  const sandbox = {
    document: {
      getElementById: element,
      createElement: (tag) => makeElement(tag),
      querySelectorAll: () => [],
    },
    location: {
      search: '',
      protocol: 'https:',
      hostname: 'passport.example',
    },
    navigator: {
      bluetooth: {},
      userAgent: 'Mozilla/5.0 (Linux; Android 14) Chrome/126',
    },
    setTimeout: (fn, ms) => {
      const id = nextTimer++;
      timers.set(id, { fn, ms });
      return id;
    },
    clearTimeout: (id) => { timers.delete(id); },
    setInterval: () => 0,
    TextDecoder,
    TextEncoder,
    URLSearchParams,
    console,
  };
  const context = vm.createContext(sandbox);
  vm.runInContext(appSource, context, { filename: 'app.js' });

  // 出站帧记录与入站帧投递:走 app.js 自身的 onNotify/重组逻辑。
  vm.runInContext(`
    globalThis.__writes = [];
    globalThis.__deliver = (frames) => {
      for (const f of frames) {
        const bytes = Uint8Array.from(f);
        onNotify({ target: { value: new DataView(bytes.buffer) } });
      }
    };
    state.rxChar = {
      properties: { writeWithoutResponse: true },
      writeValueWithoutResponse: async (frame) => {
        globalThis.__writes.push(Array.from(frame));
      },
    };
    state.txChar = { startNotifications: async () => {} };
    state.writeWithoutResponse = true;
  `, context);

  return {
    eval: (expr) => vm.runInContext(expr, context),
    el: element,
    writes() {
      return vm.runInContext('__writes.map((f) => Array.from(f))', context);
    },
    deliver(frames) {
      vm.runInContext('__deliver', context)(frames);
    },
    pendingTimers: () => timers.size,
  };
}

// 按文档帧格式切分设备应答(独立于 app.js 实现;可验证多帧重组)。
function frameJson(text, payload = 242) {
  const bytes = new TextEncoder().encode(text);
  const count = Math.max(1, Math.ceil(bytes.length / payload));
  const frames = [];
  for (let i = 0; i < count; i++) {
    const chunk = bytes.subarray(i * payload, (i + 1) * payload);
    const flags = count === 1 ? 0x03 : i === 0 ? 0x02 : i === count - 1 ? 0x01 : 0x00;
    frames.push([flags, i & 0xff, ...chunk]);
  }
  return frames;
}

// 解析手机 → 设备的出站帧,返回完整 JSON 消息(校验 flags/seq 帧协议)。
function outgoingMessages(page) {
  const messages = [];
  let chunks = [];
  let seq = -1;
  for (const frame of page.writes()) {
    const flags = frame[0];
    const frameSeq = frame[1];
    if (flags & ~0x03) throw new Error(`非法 flags: ${flags}`);
    if (flags & 0x02) {  // START(含单包)重置会话
      chunks = [];
      seq = frameSeq;
    } else {
      assert.equal(frameSeq, (seq + 1) & 0xff, '出站 seq 必须逐帧递增');
      seq = frameSeq;
    }
    chunks.push(...frame.slice(2));
    if (flags & 0x01) {
      messages.push(JSON.parse(decoder.decode(Uint8Array.from(chunks))));
      chunks = [];
    }
  }
  assert.equal(chunks.length, 0, '出站帧必须收尾');
  return messages;
}

// 等待请求写入、应答处理等微任务/宏任务完成。
const flush = () => new Promise((resolve) => setImmediate(resolve));

function reply(page, id, data) {
  page.deliver(frameJson(JSON.stringify({ id, ok: true, data })));
}

function replyError(page, id, err) {
  page.deliver(frameJson(JSON.stringify({ id, ok: false, err })));
}

function lastId(page) {
  return page.eval('state.nextId - 1');
}

// ---- 用例 ----

async function testSingleNetwork() {
  const page = createPage();
  const scan = page.eval('scanWifi()');
  await flush();
  const request = outgoingMessages(page).at(-1);
  assert.equal(request.cmd, 'wifi_scan');
  reply(page, lastId(page), { nets: [{ ssid: 'Cafe-5G', rssi: -42, sec: 3 }] });
  await scan;

  const list = page.el('net-list');
  assert.equal(list.children.length, 1);
  assert.match(list.children[0].innerHTML, /Cafe-5G/);
  assert.match(list.children[0].innerHTML, /🔒/);
  assert.match(list.children[0].innerHTML, /-42 dBm/);
  assert.equal(page.el('scan-msg').textContent, '找到 1 个网络,请点选');
  assert.equal(page.el('scan-msg').classList.contains('err'), false);
  assert.equal(page.pendingTimers(), 0);
}

async function testMultipleNetworksAndEscaping() {
  const page = createPage();
  const scan = page.eval('scanWifi()');
  await flush();
  const nets = [
    { ssid: 'HomeNet', rssi: -40, sec: 4 },
    { ssid: '<b>&"\'>evil', rssi: -70, sec: 0 },
    { ssid: 'Neighbor', rssi: -80, sec: 7 },
  ];
  // 设备按协商 MTU 分片:用较小的净荷确保多帧投递,验证网页重组。
  page.deliver(frameJson(
    JSON.stringify({ id: lastId(page), ok: true, data: { nets } }), 40));
  await scan;

  const children = page.el('net-list').children;
  assert.equal(children.length, 3);
  assert.match(children[0].innerHTML, /HomeNet/);
  // SSID 必须转义,不能注入 HTML。
  assert.match(children[1].innerHTML, /&lt;b&gt;&amp;&quot;&#39;&gt;evil/);
  assert.doesNotMatch(children[1].innerHTML, /<b>/);
  assert.match(children[2].innerHTML, /🔒/);
  assert.equal(page.el('scan-msg').textContent, '找到 3 个网络,请点选');
  assert.equal(page.pendingTimers(), 0);
}

async function testSelectAndSubmitCredentials() {
  const page = createPage();
  const scan = page.eval('scanWifi()');
  await flush();
  reply(page, lastId(page), {
    nets: [{ ssid: 'HomeNet', rssi: -50, sec: 4 }],
  });
  await scan;
  assert.equal(typeof page.el('btn-scan').listeners.click?.[0], 'function');

  // 点选网络:密码框与保存按钮出现。
  page.el('net-list').children[0].click();
  assert.equal(page.eval('state.selectedSsid'), 'HomeNet');
  assert.equal(page.eval('state.selectedSec'), 4);
  assert.equal(page.el('pass-wrap').classList.contains('hidden'), false);
  assert.equal(page.el('btn-wifi-save').classList.contains('hidden'), false);
  assert.match(page.el('pass-label').textContent, /WPA\/WPA2/);
  assert.match(page.el('wifi-msg').textContent, /已选择 HomeNet/);

  // 加密网络不填密码:阻止提交且不发送请求。
  const before = page.writes().length;
  page.eval('saveWifi()');
  await flush();
  assert.match(page.el('wifi-msg').textContent, /请输入密码/);
  assert.equal(page.writes().length, before);

  // 提交凭据:wifi_set 携带所选 SSID 与密码。
  page.el('pass-input').value = 'secret123';
  const save = page.eval('saveWifi()');
  await flush();
  const msg = outgoingMessages(page).at(-1);
  assert.equal(msg.cmd, 'wifi_set');
  assert.equal(msg.ssid, 'HomeNet');
  assert.equal(msg.pass, 'secret123');
  reply(page, msg.id, {});

  // 固件随后推送的 status 事件决定结果展示。
  await flush();
  page.deliver(frameJson(JSON.stringify({
    ev: 'status',
    data: { wifi: 'online', ip: '192.168.1.23' },
  })));
  await save;
  assert.match(page.el('wifi-msg').textContent, /配网成功/);
  assert.equal(page.el('st-ip').textContent, '192.168.1.23');
  assert.equal(page.pendingTimers(), 0);
}

async function testEmptyAndErrorResponses() {
  // 空列表:明确提示且不显示任何网络。
  const page = createPage();
  let scan = page.eval('scanWifi()');
  await flush();
  reply(page, lastId(page), { nets: [] });
  await scan;
  assert.equal(page.el('net-list').children.length, 0);
  assert.match(page.el('scan-msg').textContent, /没有发现可用网络/);
  assert.equal(page.el('scan-msg').classList.contains('err'), true);

  // 错误应答:设备错误码渲染为面向用户的中文提示。
  scan = page.eval('scanWifi()');
  await flush();
  replyError(page, lastId(page), 'scan_busy');
  await scan;
  assert.equal(page.el('net-list').children.length, 0);
  assert.equal(page.el('scan-msg').textContent, '设备正在扫描,请稍候再试');
  assert.equal(page.el('scan-msg').classList.contains('err'), true);
  assert.equal(page.pendingTimers(), 0);

  // 扫描结果获取失败(设备侧驱动结果读取失败)不得被当作空列表成功。
  scan = page.eval('scanWifi()');
  await flush();
  replyError(page, lastId(page), 'scan_failed');
  await scan;
  assert.equal(page.el('net-list').children.length, 0);
  assert.equal(page.el('scan-msg').textContent, '设备读取扫描结果失败,请重试');
  assert.equal(page.el('scan-msg').classList.contains('err'), true);
  assert.equal(page.pendingTimers(), 0);
}

async function testProvinceCityReplies() {
  const page = createPage();
  const load = page.eval('loadProvinces()');
  await flush();
  const provincesReq = outgoingMessages(page).at(-1);
  assert.equal(provincesReq.cmd, 'provinces');
  reply(page, provincesReq.id, [
    { i: 0, n: '北京市' },
    { i: 1, n: '广东省' },
  ]);

  await flush();
  const citiesReq = outgoingMessages(page).at(-1);
  assert.equal(citiesReq.cmd, 'cities');
  assert.equal(citiesReq.p, 0);
  reply(page, citiesReq.id, [
    { i: 0, n: '东城区', la: 399000, lo: 1164000 },
  ]);
  await load;

  const province = page.el('province-select');
  assert.equal(province.children.length, 2);
  assert.equal(province.children[0].textContent, '北京市');
  assert.equal(province.disabled, false);
  const city = page.el('city-select');
  assert.equal(city.children.length, 1);
  assert.equal(city.children[0].textContent, '东城区');

  const save = page.eval('saveCity()');
  await flush();
  const cityReq = outgoingMessages(page).at(-1);
  assert.equal(cityReq.cmd, 'city_set');
  assert.equal(cityReq.p, 0);
  assert.equal(cityReq.c, 0);
  reply(page, cityReq.id, {});
  await save;
  assert.match(page.el('city-msg').textContent, /城市已保存/);
  assert.equal(page.pendingTimers(), 0);
}

await testSingleNetwork();
await testMultipleNetworksAndEscaping();
await testSelectAndSubmitCredentials();
await testEmptyAndErrorResponses();
await testProvinceCityReplies();
console.log('provisioning web contract tests: PASS');
