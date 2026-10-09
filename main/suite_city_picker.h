// main/suite_city_picker.h —— 省市两级城市选择页。
// 先选省、再选市;城市页确认后保存 NVS 并触发天气刷新。
#pragma once

#include "suite_app.h"

// 省份列表页(OK 进入 SUITE_PAGE_CITY,长按返回设置页)。
extern const suite_page_t SUITE_PAGE_PROVINCE;

// 城市列表页(展示 SUITE_PAGE_PROVINCE 选中省的城市,OK 保存并返回设置页)。
extern const suite_page_t SUITE_PAGE_CITY;
