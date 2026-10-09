<p align="right"><a href="README.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Phone Web BLE Provisioning Page

Static, build-free provisioning web app for FoloToy AI Passport. The page runs
in a phone browser, connects to the device over Web Bluetooth, lets the user
scan nearby Wi-Fi networks, save credentials, and pick a province/city.

## Files

- `index.html` — mobile-first single screen (served with `no-cache`).
- `app.js` — Web Bluetooth logic, frame protocol, dashboard (no dependencies).
- `style.css` — dark mobile stylesheet.
- `deploy.sh` — upload the three files to the server and reload Caddy.

## Browser requirements

- Must be opened over **HTTPS** (Web Bluetooth is blocked on plain HTTP except
  on `localhost`).
- Requires `navigator.bluetooth`: **Android Chrome/Edge or another Chromium
  browser**. iOS/iPadOS Safari does not support Web Bluetooth and the page
  shows an explicit Chinese message asking the user to switch to an Android
  Chromium browser.
- Bluetooth and location permission may be required for the device chooser.

## User flow

1. On the device: menu → Settings → **Phone provisioning**. The screen shows a
   QR code (encoding the page URL with device id), a fresh 6-digit pairing
   code, and begins BLE advertising.
2. Scan the QR code to open `https://passport.ywyz.tech/?d=<device-id>`.
3. Tap **Connect**, choose the `FoloPassport-XXXX` device, enter the pairing
   code shown on the device screen.
4. Scan Wi-Fi, pick a network, enter the password, save. Live status events
   show `connecting` → `online`, or a localized failure such as wrong password
   (reason 15) / AP not found (reason 200/201).
5. Pick province/city and save; the device stores the city in NVS and requests
   a weather refresh.

Leaving the provisioning page on the device stops advertising and disconnects
the phone.

## GATT profile (fixed 128-bit UUIDs)

| Role | UUID |
| --- | --- |
| Service | `7a110001-4f1a-8c20-50a7-b6e900000001` |
| RX (phone → device, write / write-without-response) | `7a110002-4f1a-8c20-50a7-b6e900000002` |
| TX (device → phone, notify) | `7a110003-4f1a-8c20-50a7-b6e900000003` |
| Info (read) | `7a110004-4f1a-8c20-50a7-b6e900000004` |

Advertising name: `FoloPassport-XXXX`, where `XXXX` is the last two bytes of
the BLE MAC in uppercase hex, identical to the `d` query parameter in the URL.

## Framing protocol

Every frame is `[flags:1][seq:1][payload]`:

- flags bit0 = END, bit1 = START; single frame `0x03`, first `0x02`,
  middle `0x00`, last `0x01`.
- A START frame resets the reassembly session; `seq` increments per frame and
  duplicates are dropped; a half message is discarded after 3 seconds.
- The reassembled payload is one UTF-8 JSON message.
- The browser cannot negotiate ATT MTU, so it always sends 180-byte payloads
  (182 bytes per frame). The device adapts to the negotiated MTU.

Messages (see firmware `main/suite_prov_ble.c` for the authoritative list):

- Requests: `{"id":n,"cmd":"hello","pin":"123456"}`, `status`, `wifi_scan`,
  `{"id":n,"cmd":"wifi_set","ssid":"...","pass":"..."}`, `provinces`,
  `{"id":n,"cmd":"cities","p":0}`, `{"id":n,"cmd":"city_set","p":9,"c":4}`.
- Replies: `{"id":n,"ok":true,"data":{...}}` or
  `{"id":n,"ok":false,"err":"bad_pin|bad_arg|scan_busy|..."}`.
- Unsolicited event:
  `{"ev":"status","data":{"wifi":"idle|connecting|online|fail","ssid":"ab***","ip":"...","rssi":-55,"reason":15,"time_synced":true,"city":{"n":"...","la":298683,"lo":1215440}}}`.

`wifi_scan` returns up to 20 deduplicated networks
(`{"nets":[{"ssid","rssi","sec"}]}`, `sec` is the numeric `WIFI_AUTH_*`
order). Wrong pairing code three times makes the device terminate the link.

## Security baseline

The BLE link uses Just Works pairing with no MITM protection; authorization
relies only on the on-screen 6-digit pairing code. This is a basic guard
against casual neighbors, not a complete authorization or encryption design.
Wi-Fi passwords are never logged by the firmware and are not embedded in the
URL; the pairing code is shown only on the device screen.

## Manual deployment

The page is static; any HTTPS static host works. The project server uses
Caddy. Upload the three files to `/var/www/passport/` and add this site block
to `/etc/caddy/Caddyfile` (confirm the actual config path first):

```caddy
passport.ywyz.tech {
    root * /var/www/passport
    encode gzip
    file_server
    @index not file
    header / Cache-Control "no-cache"
}
```

Then reload: `systemctl reload caddy` (or
`caddy reload --config /etc/caddy/Caddyfile`).

Verify:

- `curl -I https://passport.ywyz.tech/` returns `200` with a valid certificate.
- `curl -I http://passport.ywyz.tech/` redirects to HTTPS.
- An Android Chrome incognito window shows no mixed-content warnings.

The idempotent `deploy.sh` uploads the files and reloads Caddy
(`DEPLOY_HOST` and `REMOTE_DIR` can override the defaults); adding the Caddy
site block itself is a one-time manual step.
