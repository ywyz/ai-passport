[简体中文](README.zh_CN.md) · **English**

# Application Planning Documents

This directory holds the application owner's product plans without changing upstream hardware or BSP contracts.

The current authority is the [AI Passport integrated firmware plan](passport-suite-plan.md), updated 2026-10-19: translation, OTP, the merged wallet, Nextcloud tasks, the three-body clock/weather, browser BLE and device Wi-Fi. Its current checkpoint records the implemented home/provisioning features, optional Android Edge acceptance, pending computer support and the next authorized task-reading increment. Other former features are cancelled.

These 2026-10-03 documents remain historical references and must not directly drive implementation:

- [Previous travel PRD](passport-travel-prd.md).
- [Previous travel data and synchronization contract](passport-travel-contract.md).
- [Previous OpenCode stage prompts](passport-travel-prompts.md).
- [Previous D0 design review](passport-design-review.md).

The 2026-10-05 detailed design and replacement work order are:

- [Suite PRD: baseline gaps, requirements, pages and controls](passport-suite-prd.md).
- [Suite contract: messages, models, security, resource and save semantics](passport-suite-contract.md).
- [Suite stage prompts: dependencies, Codex review, traceability and decision gates](passport-suite-prompts.md).

Scope and current acceptance remain in suite-plan; unimplemented technical mechanisms and limits remain proposals. The detailed documents distinguish user-confirmed requirements, design proposals, the historical baseline and current evidence. The SU/SC/ST/SA IDs replace historical tasks. Continue the existing application on its intended branch; new stages require user authorization.
