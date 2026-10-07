// main/suite_pages.h —— 应用页面注册与共享 UI 元件。
// 干线阶段提供连接状态页(诊断 Wi-Fi/校时/天气/电池);
// 功能页面随各功能分支加入。
#pragma once

#include "suite_app.h"
#include "lvgl.h"

// 干线状态页(开机页):连接基础的自检与验收界面。
extern const suite_page_t SUITE_PAGE_STATUS;

// 共享:右上角电池标签(读 bsp_battery_soc;-1 时显示 "--")。
// 由页面在持锁状态下调用于自己的父容器上。
lv_obj_t *suite_ui_battery_create(lv_obj_t *parent);
void suite_ui_battery_update(lv_obj_t *label);
