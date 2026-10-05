[简体中文](passport-suite-prd.zh_CN.md) · **English**

# Five-feature Suite PRD

## 1 Authority, status and baseline

Review date: 2026-10-05, Asia/Shanghai. Scope authority: [suite plan](passport-suite-plan.md). Protocol: [contract](passport-suite-contract.md). Work and acceptance: [stage prompts](passport-suite-prompts.md). This revision authorizes documentation only, with no firmware, website or server implementation.

Status notation throughout the three documents: **U** = user-confirmed requirement; **D** = design proposal requiring the stated review; **C** = implemented in the inspected baseline; **V** = not verified. U does not mean implemented; D does not mean approved. Unless explicitly marked C in the baseline table, all new suite behavior is D/V; scope and required outcomes are U/V. Numeric limits are proposed testable ceilings, not measured capacities.

Initial `git status --short --branch`: `main...origin/main`, with twelve pre-existing untracked Markdown files under `docs/assets/`; no tracked modification. `git rev-parse HEAD` exactly matches `0b9e4c81ee4421c0bac39ca3561d65a8285acd4a`; no baseline delta or reset. Preserve those files and all later user changes. All five passport skills are available through readable `.agents/skills/passport-*/SKILL.md` links and the session skill catalog. No installation was needed. Codebase-memory `list_projects` returned zero indexed projects; no `.codegraph/` exists. Findings below are bounded source checks, not graph verification. No index was created.

Historical travel documents and the old design review remain history: their D1-D9, acceptance numbers and storage/provisioning schemes are inactive. This PRD introduces independent SU requirements and SA acceptance IDs.

## 2 Code-to-plan gap

| Inspected source | C: current behavior | U/D: required addition | V: evidence still needed |
| --- | --- | --- | --- |
| [main/main.c](../../main/main.c), [demo.h](../../main/demo.h) | `app_main` builds hardware tests via DEMOS and ui_pixel; input queue depth 8, nonblocking enqueue | One integrated app, new home/launcher/pages and lifecycle/input reducers in main | Startup/navigation, queue saturation, teardown |
| [demo_ble.c](../../main/demo_ble.c) | Non-connectable `BLE_GAP_CONN_MODE_NON` advertising, GAP/GATT standard initialization only | Connectable custom GATT, authenticated encrypted browser session, provisioning and protected OTP permission | Actual computer interoperability/security/coexistence |
| [demo_wifi.c](../../main/demo_wifi.c) | STA scan, no connection or credential storage | Transactional credentials, connection/reconnect, HTTPS/time and revocation | Wrong password, reboot, TLS and network faults |
| [demo_audio.c](../../main/demo_audio.c), [bsp_audio.h](../../components/bsp/include/bsp_audio.h) | Blocking PCM APIs usable in workers; three-second 16 kHz mono recording allocates 96,000 bytes as one buffer | Bounded capture/upload/playback queues; serialize codec use | New product peak RAM and stop latency; demo buffer is not a product budget |
| [bsp_button.h](../../components/bsp/include/bsp_button.h), [bsp_pins.h](../../components/bsp/include/bsp_pins.h) | PRESS, CLICK, DOUBLE, LONG; no independent RELEASE; long threshold 500 ms | Click-to-start/click-to-stop recording; gesture suppression in app | Event ordering and overload tests; no release-to-stop promise |
| [BSP public headers](../../components/bsp/include/bsp_display.h) | 240 × 320 display, LVGL lock; audio, battery and shared I2C APIs | App UI/state/workers in main; reusable hardware extensions only in BSP | Glyphs, rendering, audio/radio peaks; no new pin assumptions |
| [sdkconfig.defaults](../../sdkconfig.defaults) | 8 MB, no PSRAM, Montserrat 14/20, 24 KiB LVGL pool, 20 ms refresh, one NimBLE connection; bonding persistence disabled | Font assets, application-specific radio/security/memory settings after review | Generated config, fit and real timing |
| [partitions.csv](../../partitions.csv) | NVS 0x6000, PHY 0x1000, factory 0x7F0000 at 0x10000; no cache/OTA partition | Separate application storage/migration design after measurement | Fit, power-cut recovery, upgrade preservation; no old 5 MiB/dual resource slots |
| [validate.sh](../../tools/validate.sh) | Static repo/workflow/host checks; firmware build, merged-image/layout and archive checks | New pure-logic/protocol tests and real service/device acceptance | Existing gates do not establish any suite feature |

All BSP public headers (audio, battery, button, display, I2C and pins) were read. Reuse hardware APIs and isolated lifecycle logic, not entire demo branches. A renamed, recolored or copied hardware menu or `ui_pixel` visual shell fails SU-01.

## 3 Requirements and acceptance identity

| Requirement | U: confirmed outcome | D: proposed implementation | C | V / acceptance |
| --- | --- | --- | --- | --- |
| SU-01 | One firmware, online computer management, new UI, five-feature scope | Home plus focused launcher and shared state owner | No suite code | SA-01 |
| SU-02 | Computer BLE; device Wi-Fi; no phone dependency | SC-01/02 custom GATT and transactional setup | Advertising/scan only | SA-02, SA-03 |
| SU-03 | Physical confirmation, identity, confidentiality, revocation, TLS validation | SC-02/03 authenticated session, signed time bootstrap | Not implemented | SA-04, SA-05 |
| SU-04 | Complete page/button behavior and safe asynchronous lifetime | SC-04 generation tokens, cancellation priority, stop acknowledgement | Baseline examples only | SA-06 |
| SU-05 | Reviewed train/flight/performance/other tickets; original valid code | SC-05 typed models and atomic published revisions | Not implemented | SA-07 |
| SU-06 | Offline cache, honest code boundary, real-device scanning | SC-05 fit rejection and separate scan acceptance | Not implemented | SA-08, SA-09 |
| SU-07 | Real Nextcloud list discovery/read/select | SC-06 CalDAV adapter and bounded snapshots | Not implemented | SA-10, SA-11 |
| SU-08 | Device completion, durable offline retry, no newer edit overwrite | SC-06 conditional updates and conflict review | Not implemented | SA-12 |
| SU-09 | Default three-body clock/weather home | SC-07 bounded scene, city and independent freshness | Not implemented | SA-13 |
| SU-10 | Chinese ↔ English short-sentence voice translation | SC-08 bounded streaming recognition/translation/synthesis | PCM API only | SA-14, SA-15 |
| SU-11 | Source/translation separately; cancel, privacy and Chinese glyphs | SC-08 text caps, deletion, font policy | No Chinese suite fonts | SA-16 |
| SU-12 | Local OTP with protected import and trustworthy time | SC-09 TOTP, local unlock, separate BLE capability | Not implemented | SA-17, SA-18 |
| SU-13 | Seed preservation/deletion and independent encrypted recovery | SC-09 vault and offline recovery materials | Not implemented | SA-19 |
| SU-14 | Resource bounds, durable saves and empty-server recovery | SC-10 budgets, journals and three persistence receipts | Default layout only | SA-20, SA-21 |
| SU-15 | Complete gate, matching image/debug artifacts and authorized device handoff in future firmware stages | ST-06 integration and evidence ledger | Gate tooling exists | SA-22 |

Cancelled entirely: quota, coding assistant control, guide, China check-ins, QQ Music, PPT remote, routes, narration, province packs, maps, stamps, achievements and associated entrances/models. They are not deferred features. No task creation; reopening a completed task remains decision Q-07, with no active control. No FIDO2/Passkey/U2F/password vault/SSH signing. No phone/SoftAP workflow. OCR, automatic language detection and translation history are not required deliverables; adding them requires a new scope decision.

## 4 New UI and universal input rules

D/V: Design a quiet vertical layout: status strip (battery top-right, unavailable as a dash), large content region, fixed bottom key hints; separate full code canvas. Home has a procedural scene, not demo cards/mascot/clouds. Launcher is a single textual list. Details use scrollable labeled fields and explicit provenance/freshness. No inherited menu/screens/ui_pixel helpers. Reserve rounded-screen corners; ticket-code canvas is a centered 216 × 216 safe square.

Use only UP/DOWN/OK independently. CLICK is the short-press action. PRESS starts an application gesture epoch; LONG consumes that epoch, opens the action menu (or cancels a busy operation as below), and suppresses every later CLICK/DOUBLE from that same press until a fresh PRESS. DOUBLE performs no action; test rapid double clicks rather than inventing a release event. Carry key, gesture ID, monotonic timestamp and page generation through input. If event ordering cannot be proven with the current component, ST-01 must resolve it before shipping; hold-to-record would require a separately reviewed BSP RELEASE extension and host/device tests, not an inferred ADC release.

Action menu: UP/DOWN select; OK chooses; long OK closes the menu to its originating page. Back is always first. Destructive/remote actions open a Yes/Cancel screen with Cancel selected; UP/DOWN select, OK commits once; long OK cancels. UP/DOWN long presses otherwise have no action. A long OK causing a page change must not trigger an OK action on the new page.

Input queue proposal: depth 16, normal movement events coalesced, full queue reports Busy and drops navigation rather than duplicating an action. Keep cancellation and stop requests in a separate atomic flag/worker notification so they survive queue saturation; retain LONG consumption before dropping an event. Durable operations are accepted only after journal admission; full completion queue shows Not saved, never Pending. See SC-04 for lifecycle ownership.

## 5 Page and button matrix

All rows are D/V implementing SU-01/04; states in section 6 apply to each row. Long OK is the stated escape; other long keys and DOUBLE are ignored. No page waits for simultaneous keys or RELEASE.

| Page / entry | Content; UP/DOWN short | OK short | Long OK / return and exit |
| --- | --- | --- | --- |
| Home / boot after ST-03 | Scene, date/time trust, city/weather freshness; scroll weather details | Launcher | Launcher; scene timer stops when hidden |
| Launcher / home | Translation, OTP, Wallet, Tasks, Settings/Sync; move selection | Open selected page | Home via action menu Back |
| Settings/Sync / launcher | Wi-Fi/binding/city/language/sync and three save receipts; select item | Item details or refresh confirmation | Back to launcher; background sync continues |
| Provision/bind / settings | Countdown, verified identity fingerprint/SAS, candidate network/status; select Confirm/Cancel | Physical confirm once, not automatic on browser request | Cancel candidate, close BLE, retain old config; back settings |
| Translation ready / launcher | Direction zh-CN→en or en→zh-CN; UP swaps direction, DOWN scrolls last result | Start capture only when online/time/TLS ready | Back to launcher via action menu |
| Translation capture / ready | Recording timer and bytes; UP cancels/discards, DOWN no action | Stop capture and finalize upload; limit also stops | Cancel/discard and return ready; no release-to-stop |
| Translation processing / capture | Upload/recognition/translation/synthesis with elapsed time; UP cancels, DOWN scrolls available source | No action until result | Cancel and return ready |
| Translation result / processing | Separate source and translation; UP/DOWN scroll both labeled sections | Play/stop synthesized voice | Menu: Back, replay, new sentence; Back stops playback and returns ready |
| OTP locked/PIN / launcher | Locked prompt or selected PIN digit; UP/DOWN change digit 0-9 | Start unlock / append digit, submit after 8 digits | Cancel, wipe digits, launcher; no code visible |
| OTP PIN setup / first import | Set and repeat 8-digit PIN; UP/DOWN select digit | Append/submit, matching repeat required | Cancel staged import and wipe PIN; return locked |
| OTP operation approval / local OTP menu | Verified peer, scope, account or restore summary, expiry and Confirm/Cancel; UP/DOWN choose | Grant one-use import/delete/backup/restore capability after unlock | Deny, wipe staged data, return accounts and lock if leaving |
| OTP accounts / unlocked | Issuer/account, trusted-time indicator; UP/DOWN select | Open local code, or no code if time untrusted | Menu Back/Lock/import mode; Back locks and returns launcher |
| OTP code / account | Digits, remaining period, account; UP/DOWN no action | Accounts | Menu Back/Lock/Delete confirmation; idle/page exit locks |
| Wallet list / launcher | Date/type/name, cache age/code availability; UP/DOWN select | Details | Menu Back/refresh; Back launcher |
| Ticket detail / list | Labeled full dates/zones, fields, source/review/missing reasons; scroll | Code if valid supported original exists; otherwise reason | Menu Back/delete request/cache clear separately; Back list |
| Ticket code / detail | Original code only, validity/cache warnings outside quiet zone; no UP/DOWN action | Details | Details; stop code-page timers and restore normal brightness |
| Task lists / launcher | Selected collections, sync age/count; UP/DOWN select | Tasks in collection | Menu Back/select collections on website/refresh; Back launcher |
| Task items / list | Title, due kind/date/zone/status/pending/conflict; UP/DOWN select | Task detail | Menu Back/refresh; Back collections |
| Task detail / items | Title, note paging, original due, cache labels; UP/DOWN scroll | Complete confirmation if actionable; completed items show status only | Back to items through menu |
| Complete confirmation / task | Explicit UID/account/list and Complete/Cancel; UP/DOWN choose | Save durable completion or cancel | Cancel to detail; no task creation/reopen |
| Task conflict / sync/detail | Cached intention and newer remote summary; UP/DOWN scroll | Review menu: retain remote (cancel intention) or refresh and reconfirm completion against new ETag | Back detail; unresolved intent stays visible, no overwrite |
| Generic action/confirmation/error / any page | Cause, safe action, selection; UP/DOWN choose | Retry only if permitted, acknowledge or confirm | Cancel/Back to origin; generation guard still applies |

Website D/V flow: login with independent recovery access; Connect invokes browser chooser, verify physical SAS, provision candidate, show committed Wi-Fi and binding separately. Tickets: manual form or pasted text → draft with highlighted uncertain/missing fields → human review → publish revision → server receipt → device download/local activation → code/scan state. Tasks: privately configure Nextcloud → discover collections → select lists → connection/read evidence → device snapshot/completion receipt. Settings expose city/direction/revocation and receipt timestamps. OTP opens a separate isolated local parser/import/recovery view; ordinary settings cannot invoke seed commands. No sensitive URI in URLs, logs, analytics or normal server requests.

## 6 States, ownership and failure presentation

| State family | D/V behavior and safe exit |
| --- | --- |
| Every page: entering/ready/leaving | Enter from validated immutable model; leave invalidates page generation before stopping owned workers/timers/subscriptions; stop acknowledgement precedes object deletion |
| Loading | Show phase and elapsed time; cancel supported; no duplicate OK submissions; retained cached content labeled separately |
| Offline | Wallet/tasks show complete cached revision and age; task completion can queue; translation cannot start; trusted OTP can work until time uncertainty limit; weather stays stale |
| Empty | No tickets/tasks/accounts or no chosen collection has a distinct explanation and website/import action; Back remains usable |
| Error | Show safe reason and retry/back; Wi-Fi password/TLS/auth/time/resource errors differ; no secret content in error text |
| Stale/conflict | Show age and remote/local difference; preserve usable revision and newer remote data; no silent overwrite |
| Queue full/stop timeout | Show Busy/Not saved; cancellation flag survives. If worker can still access UI, retain page and allow stop retry rather than delete it |

The page owner owns LVGL objects, scene timers, recording/playback job and view subscription. A background sync coordinator owns Wi-Fi reconnect, tickets/task caches, durable completion queue and weather refresh. Leaving a page cancels its audio job but does not cancel an already durably admitted task completion. Background services never hold raw LVGL pointers: publish models tagged with request ID/page generation to the UI owner; reject late callbacks after exit, including notifications arriving after cancel. Cross-task LVGL use takes `bsp_lvgl_lock()`; callbacks enqueue only. Each phase deadline and cache age is specified in SC-03/06/07/08/10.

Before firmware work, review Q decisions in prompts. This design contains full translation and OTP detail now, irrespective of their later implementation order. This document's delivery is not feature completion or a device-test invitation; firmware has not changed.
