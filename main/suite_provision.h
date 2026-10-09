// main/suite_provision.h —— 手机扫码 BLE 配网页。
// 进入页面开始 BLE 广播,展示二维码与 6 位配对码;退出页面停止广播。
#pragma once

#include "suite_app.h"

// 配网页(扫码配对,长按返回设置页)。
extern const suite_page_t SUITE_PAGE_PROVISION;
