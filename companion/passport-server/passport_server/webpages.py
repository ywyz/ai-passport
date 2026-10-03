"""Server-rendered responsive web pages (D1a, contract W01/W05/W06).

Minimal static pages; all state changes go through the JSON API with the
cookie session and CSRF. Layout targets a 360 px viewport and desktop.
Never renders passwords, binding codes or tokens after creation.
"""
from __future__ import annotations

import os
import secrets

HTML = "text/html; charset=utf-8"

_CSS = ""
with open(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "web", "styles.css"), encoding="utf-8") as f:
    _CSS = f.read()


def _page(title: str, body: str) -> bytes:
    html = f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title} · Passport Server</title>
<style>{_CSS}</style></head>
<body><header><h1>Passport Server (D1 test instance)</h1>
<p class="warn">Local single-user test deployment. Fixture data is fictional.</p>
</header>{body}<footer>D1a · passport-travel-v1-draft</footer></body></html>"""
    return html.encode("utf-8")


def login_page(error: str = "") -> bytes:
    err = f'<p class="error">{error}</p>' if error else ""
    return _page("Login", f"""<main><section>{err}
<form id="login">
<label>Admin password<input type="password" name="password" autocomplete="off" required></label>
<button type="submit">Log in</button>
</form></section>
<script>document.getElementById('login').addEventListener('submit', async e => {{
  e.preventDefault();
  const r = await fetch('/api/v1/web/session', {{method:'POST',
    headers:{{'Content-Type':'application/json'}},
    body: JSON.stringify({{password: e.target.password.value}})}});
  if (r.ok) {{ location.href = '/app'; }} else {{
    const b = await r.json(); document.querySelector('.x')?.remove();
    document.querySelector('section').insertAdjacentHTML('afterbegin',
      `<p class="error">${{b.user_message}}</p>`); }}
}});</script>""")


def app_page(csrf: str) -> bytes:
    # csrf is delivered to the page via meta tag; forms use fetch.
    return _page("Dashboard", f"""<main>
<meta name="csrf" content="{csrf}">
<nav><a href="#packs">Packs</a><a href="#devices">Devices</a>
<a href="#records">Records</a><a href="#backups">Backups</a></nav>
<section id="packs"><h2>Publish fixture pack</h2>
<form id="publish">
<label>Label<input name="label" value="Fixture Jiangsu test pack" required></label>
<fieldset><legend>Files</legend>
<div id="files">
<label>Path<input name="path" placeholder="text/stop-001.txt"></label>
<label>Text<textarea name="text" rows="3"></textarea></label>
</div>
<button type="button" id="addfile">Add file</button>
<button type="button" id="glyphs">Fill glyph inventory from texts</button>
</fieldset>
<p><label><input type="checkbox" name="fixture" checked> Fixture label</label></p>
<button type="submit">Publish</button><output id="presult"></output></form>
<script>
const inv = () => {{ const set = new Set();
  document.querySelectorAll('textarea[name=text]').forEach(t =>
    [...t.value].forEach(ch => {{
      const cp = ch.codePointAt(0);
      if (cp > 127) set.add(cp); }}));
  return [...set]; }};
document.getElementById('glyphs').onclick = () =>
  document.getElementById('inv').value = JSON.stringify(inv());
document.getElementById('addfile').onclick = () => {{
  document.getElementById('files').insertAdjacentHTML('beforeend',
   '<label>Path<input name="path"></label><label>Text<textarea name="text" rows="2"></textarea></label>');}};
document.getElementById('publish').addEventListener('submit', async e => {{
  e.preventDefault(); const f = e.target; const files = [];
  const paths = [...f.querySelectorAll('input[name=path]')];
  const texts = [...f.querySelectorAll('textarea[name=text]')];
  paths.forEach((p, i) => files.push({{path: p.value, text: texts[i].value}}));
  const payload = {{schema_version: 1, pack_id: 'fixture-js-trip-001',
    kind: 'guide', label: f.label.value, fixture: f.fixture.checked,
    glyph_inventory: inv(), files}};
  const r = await fetch('/api/v1/web/packs', {{method: 'POST',
    headers: {{'X-CSRF': document.querySelector('meta[name=csrf]').content}},
    body: JSON.stringify(payload)}});
  document.getElementById('presult').textContent = JSON.stringify(await r.json());}});
</script></section>

<section id="devices"><h2>Device bindings</h2>
<form id="createbinding"><button type="submit">Create binding code</button>
<output id="bres"></output></form><div id="devices-table"></div>
<script>
const refresh = async () => {{
  const s = await (await fetch('/api/v1/web/status', {{headers:{{'X-CSRF':document.querySelector('meta[name=csrf]').content}}}})).json();
  const records = await (await fetch('/api/v1/web/records', {{headers:{{'X-CSRF':document.querySelector('meta[name=csrf]').content}}}})).json();
  document.getElementById('devices-table').innerHTML =
    '<table><tr><th>Device</th><th>Label</th><th>Status</th><th></th></tr>' +
    records.devices.map(d => `<tr><td><code>${{d.device_id}}</code></td><td>${{d.display_label}}</td><td>${{d.status}}</td><td>${{d.status === 'active' ? `<button onclick="revoke('${{d.device_id}}')">Revoke</button>` : ''}}</td></tr>`).join('') + '</table>';
}};
const revoke = async id => {{
  await fetch(`/api/v1/web/devices/${{id}}/revoke`, {{method:'POST',
    headers:{{'X-CSRF':document.querySelector('meta[name=csrf]').content}},
    body: JSON.stringify({{csrf: document.querySelector('meta[name=csrf]').content}})}});
  refresh();}};
document.getElementById('createbinding').addEventListener('submit', async e => {{
  e.preventDefault();
  const r = await fetch('/api/v1/web/devices', {{method:'POST',
    headers: {{'X-CSRF':document.querySelector('meta[name=csrf]').content}},
    body: JSON.stringify({{csrf: document.querySelector('meta[name=csrf]').content}})}});
  const b = await r.json();
  if (b.code) {{
    document.getElementById('bres').innerHTML =
      `Binding code <b>${{b.code}}</b> expires ${{b.expires_at}} — enter it on the device provisioning page.`;
  }} else document.getElementById('bres').textContent = JSON.stringify(b);
}});
refresh(); setInterval(refresh, 5000);
</script></section>

<section id="records"><h2>Records and conflicts</h2><div id="records-table"></div>
<script>
const refreshRec = async () => {{
  const b = await (await fetch('/api/v1/web/records', {{headers:{{'X-CSRF':document.querySelector('meta[name=csrf]').content}}}})).json();
  document.getElementById('records-table').innerHTML =
    '<table><tr><th>Record</th><th>Kind</th><th>Context</th><th>Target</th><th>Rev</th><th>State</th></tr>' +
    b.records.map(r => `<tr><td><code>${{r.record_id}}</code></td><td>${{r.kind}}</td><td>${{r.context_id ?? ''}}</td><td>${{r.target_id}}</td><td>${{r.revision}}</td><td>${{r.state ?? ''}}</td></tr>`).join('') + '</table>' +
    `<h3>Server receipt vs backup</h3><ul><li>Queued jobs: ${{s.upload_jobs.filter(j=>j.state==='queued').length}}</li>` +
    `<li>Failed jobs: ${{s.upload_jobs.filter(j=>j.state==='failed').length}}</li>` +
    `<li>Last complete snapshot: ${{s.snapshots.complete?.[0]?.snapshot_id ?? 'none'}}</li></ul>` +
    (b.conflicts.length ? `<h3>Conflicts</h3><table><tr><th>Event</th><th>Status</th></tr>${{
      b.conflicts.map(c => `<tr><td>${{c.event_id}}</td><td>${{c.result}}@${{c.result_revision ?? ''}}</td></tr>`).join('')}}` : '');
}};
refreshRec(); setInterval(refreshRec, 5000);
</script></section>

<section id="backups"><h2>Backups and restore</h2>
<form id="davform"><fieldset><legend>WebDAV</legend>
<label>Base URL<input name="base_url" placeholder="http://localhost:8647/dav/"></label>
<label>User<input name="username"></label>
<label>Password<input name="password" type="password"></label>
<button type="button" id="davtest">Configure &amp; test</button><output id="davout"></output></fieldset></form>
<form id="snapform"><button type="submit">Start snapshot</button><output id="snapout"></output></form>
<form id="restoreform"><fieldset><legend>Restore (empty server only)</legend>
<label>Snapshot<input name="snapshot_id" placeholder="snap-..."></label>
<button type="button" id="previewbtn">Preview</button><output id="prevout"></output><br>
<label><input type="checkbox" name="confirm"> Confirmed restore</label>
<button type="submit">Restore</button><output id="resout"></output></fieldset></form>
<script>
document.getElementById('davtest').onclick = async () => {{
  const f = document.getElementById('davform');
  const r = await fetch('/api/v1/web/webdav', {{method:'POST',
    headers:{{'X-CSRF':document.querySelector('meta[name=csrf]').content}},
    body: JSON.stringify({{base_url: f.base_url.value, username: f.username.value,
      password: f.password.value}})}});
  document.getElementById('davout').textContent = JSON.stringify(await r.json());}};
document.getElementById('snapform').addEventListener('submit', async e => {{
  e.preventDefault();
  const r = await fetch('/api/v1/web/backups', {{method:'POST',
    headers:{{'X-CSRF':document.querySelector('meta[name=csrf]').content}},
    body: JSON.stringify({{csrf: document.querySelector('meta[name=csrf]').content}})}});
  const b = await r.json();
  document.getElementById('snapout').textContent = `Started job ${{b.job_id}} (state queued — not complete)`;
}});
document.getElementById('previewbtn').onclick = async () => {{
  const f = document.getElementById('restoreform');
  const r = await fetch('/api/v1/web/restore-previews', {{method:'POST',
    headers:{{'X-CSRF':document.querySelector('meta[name=csrf]').content}},
    body: JSON.stringify({{csrf: document.querySelector('meta[name=csrf]').content, snapshot_id: f.snapshot_id.value}})}});
  document.getElementById('prevout').textContent = JSON.stringify(await r.json());}};
document.getElementById('restoreform').addEventListener('submit', async e => {{
  e.preventDefault(); const f = e.target;
  const r = await fetch('/api/v1/web/restore', {{method:'POST',
    headers:{{'X-CSRF':document.querySelector('meta[name=csrf]').content}},
    body: JSON.stringify({{csrf: f.querySelector('meta')?.content || document.querySelector('meta[name=csrf]').content,
      snapshot_id: f.snapshot_id.value, confirm: f.confirm.checked}})}});
  document.getElementById('resout').textContent = JSON.stringify(await r.json());}});
</script></section></main>""")


def serve_static(path: str) -> tuple[bytes, str] | None:
    if path in ("/styles.css",):
        return _CSS.encode(), "text/css; charset=utf-8"
    return None
