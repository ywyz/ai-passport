[English](README.md) · **简体中文**

# 应用规划文档

本目录保存应用所有者的产品规划，不改变上游硬件或 BSP 能力约定。

当前依据为 [AI Passport 多合一固件规划](passport-suite-plan.zh_CN.md)（2026-10-05）：翻译、OTP、合并票夹、Nextcloud 待办、三体时钟和天气，以及电脑网页 BLE 连接与设备 Wi-Fi。其余原功能已取消。

以下 2026-10-03 文档仅保留为历史记录，不能直接用于实施：

- [旧旅行 PRD](passport-travel-prd.zh_CN.md)。
- [旧旅行数据与同步约定](passport-travel-contract.zh_CN.md)。
- [旧 OpenCode 阶段提示词](passport-travel-prompts.zh_CN.md)。
- [旧 D0 设计评审](passport-design-review.zh_CN.md)。

2026-10-05 的详细设计与重编任务为：

- [五合一 PRD：基线差距、需求、页面与按键](passport-suite-prd.zh_CN.md)。
- [五合一 contract：消息、模型、安全、资源与保存语义](passport-suite-contract.zh_CN.md)。
- [五合一阶段提示词：依赖、Codex 复审、映射和决策门槛](passport-suite-prompts.zh_CN.md)。

范围仍由 suite-plan 定义；技术机制及上限为建议，不代表批准或功能实现。详细文档分别标明用户确认、设计建议、基线代码已实现和未验证。新 SU/SC/ST/SA 编号替代历史任务。后续固件仅在对应阶段获准后，从核对基线的 feature 分支开始。
