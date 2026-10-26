[简体中文](README.zh_CN.md) · **English**

# Application Planning Documents

This directory holds the application owner's product plans without changing
upstream hardware or BSP contracts. [Suite plan section 0](passport-suite-plan.md#0-current-implementation-and-acceptance)
is the single current implementation/acceptance authority.

## Current state, 2026-10-26

| Question | Answer |
| --- | --- |
| Known source | The authoritative native `/home/ywyz/code/ai-passport-oc-20261022/repo` on the intended `feature/feature10-ble-provisioning` branch line; read the current branch, commit and dirty state from Git. Windows holds documentation mirrors but stale application code; its code cannot establish native feature absence, though its own flash logs remain valid for its artifact. |
| Last verified backend | Nextcloud Tasks companion deployed behind `/suite/v1/` with a five-minute refresh timer; translation T1 companion deployed, with one authorized synthetic MiMo EN->zh-CN smoke PASS (7.445 s, one ASR + one MT claim). |
| Installed artifact | Post-recording response-cache application `79a0d3b5...` / ELF `9d71323f...` from package `d2975d36...`, 3,697,184 bytes, installed 2026-10-26 by one guarded app-only write at `[0x10000,0x397000)`. Guard rc0 PASS `APP_ONLY_WRITE_DIGEST_AND_CLOSE`, single 78.57 s execute, one read plus one write bridge session each closed, COM3 release confirmed; the fresh live 4,096-byte table sector matched `ba728028...` before the app write and the pinned esptool digest comparison matched with success marker 1. Full package is not a whole-Flash clone: bootloader/table/data/key partitions were not written. A bounded no-reset startup observation of 100.95 s received 0 serial bytes, 0 TRDIAG events and 0 fault markers, with no boot ELF line or trace; that is dated startup evidence, not a clean, stable or startup PASS. After the human declared readiness, two guided manual trials (one EN->zh, one zh->EN) passed the operator-reported recognition/translation/display check with no obvious stutter; the operator found speed not very fast but acceptable. Device tests are PASS only for that one-trial-per-direction subset, and the latency cause is undetermined. |
| Previous installed snapshot (superseded history) | TLS memory-lifetime application `a71b9c3e...` / ELF `0352858d...` from package `902a0930...`, installed app-only 2026-10-26 and superseded by the current `d2975d36...` flash. Its operator-reported ready-plus-record-entry plus UP-cancel round was normal; its later actual EN->zh-CN attempt failed after recording and OK stop with the translation-cache OOM message and no A/F/L. The failed HTTP call (admit, upload or status) and onboard heap largest/free block are unknown. Its 100.91 s / 112-byte startup window belongs only to `902a...`. |
| Earlier installed snapshot (superseded history) | TLS snapshot application `d0396c22...` / ELF `0ac23853...` from package `cb8bd517...`, installed 2026-10-25. Its one EN->zh-CN readiness attempt stopped at TLS preflight (`R7 S12 X11` / `E801A T3000 V0`) and one runtime `task_wdt` occurred during weather certificate work. Both are superseded history whose relationship to the current install is unknown; they are not current observations. |
| Accepted subset | Tasks read/complete/offline cache/normal-reboot replay and voice preview/default-cancel/create replay; two operator-reported single-trial EN<->zh translations on the current install; earlier home, provisioning, clock and weather slices. |
| Current blocker | The remaining translation work is localization of the reported slow response, whose cause is undetermined, plus other unaccepted scenarios: repeat cycles, long recordings, cancel/timeout/offline recovery and power cuts on this image, provider billing and SA-15. The two guided one-trial-per-direction manual translations on the installed `d2975d36...` passed the operator-reported functional subset; that result needs no further flash, monitoring or provider call. The 2026-10-26 complete-gate Build/Host PASS is reused and no firmware rerun occurred. The prior `902a...` ready/cancel subset remains dated history, and its real EN->zh-CN attempt failed after recording with the cache-OOM message; the failed HTTP call and heap largest/free block are unknown. T2/TTS is unimplemented, and the older `cb8bd517` TLS-preflight and weather `task_wdt` records remain superseded history with unproven relation. See the [T1 current checkpoint](passport-translation-t1-core.md#current-checkpoint-2026-10-26). |
| Placeholders | OTP and wallet have no implementation. |

The current OC-11 app-only range is `[0x10000,0x397000)` with a 3,697,184-byte
application and partition-table SHA-256 `34899c03...`. On 2026-10-26 the
`d2975d36...` application was written app-only; the live table matched `ba728028...`
and only application sectors were written, so the device Flash as a whole is not the
merged archive `d2975d36...`. The full package is the application-source package
identity, not a full physical-Flash clone; settings, keys, cache and task journals are
protected by range separation. A 100.95 s bounded no-reset startup window received
0 serial bytes, 0 TRDIAG events and 0 fault markers, with no boot ELF line or trace;
that absence is dated startup evidence and not a stable or startup PASS. The two
guided one-trial-per-direction manual translations followed on this install and passed
the operator-reported functional subset; the latency cause remains undetermined. The
prior
`902a0930...` install and its 100.91 s / 112-byte window are superseded dated history.
The `cb8bd517...` application and its TLS-preflight and weather-`task_wdt` history are
also superseded and preserved as dated history. The `8e86d041...` recovery at
`[0x10000,0x396000)` remains rollback/historical. The mistaken 2026-10-25 older
Windows build `4b0187a0...` (app `ecbbdc44...`) and its startup watchdog are a
distinct dated incident, not the current install. The `119f73c2...` archive from
2026-10-24 (ELF `f4f4f154...`) was never flashed, and the 2026-10-25 documentation-only
gate archive `c3b37953...` (ELF `abce9bd8...`) is VALIDATION_ONLY_NOT_FLASHED. The
memory-lifetime change defers 8,193 bytes per HTTP call and 4,098 bytes at ready in
source; the actual onboard heap gain is unmeasured, and the existing capacities and
strict TLS behavior are unchanged. The preflash gate-stage record for `d2975d36...`
(VALIDATED_NOT_FLASHED) remains historical; the actual installation above supersedes
it for current status. Full identities and the incident record are in
[suite-plan section 0](passport-suite-plan.md#artifact-identities-2026-10-25).

## Index

- Tasks: [device reader](passport-device-tasks.md) and
  [Nextcloud companion](passport-nextcloud.md).
- Translation: [approved plan](passport-translation-plan.md),
  [storage and configuration](passport-translation-device.md),
  [provider and HTTP contract](passport-translation-provider.md) and
  [T1 core](passport-translation-t1-core.md).
- Reviewed historical reliability evidence:
  [OC-01/02/03 review](passport-reliability-review.md).
- Design baseline and work orders: [suite PRD](passport-suite-prd.md),
  [suite contract](passport-suite-contract.md) and
  [stage prompts](passport-suite-prompts.md).

The 2026-10-05 documents still define unmet requirements and limits; anything there
marked as a proposal is not implemented. SU/SC/ST/SA IDs replace historical task
numbers. The 2026-10-03 travel PRD, travel contract, prompts and D0 review covered
cancelled guide/check-in scope and have been deleted; do not recreate or link them.
Continue the existing application on its intended branch; new stages require user
authorization.
