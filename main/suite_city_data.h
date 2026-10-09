// main/suite_city_data.h —— 省市两级城市数据(纯逻辑,可主机测试)。
// 覆盖全国 34 个省级行政区及其地级行政区(地级市/州/盟/地区,常用简称),
// 附市中心近似坐标:×1e4 存 int32,精度对 Open-Meteo 网格足够。
// 全部为 const 数据,链接进 flash 的 .rodata。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *name;  // 展示名(中文,字符须在 suite_charset.txt 字集内)
    int32_t lat_x1e4;  // 纬度 ×1e4
    int32_t lon_x1e4;  // 经度 ×1e4
} suite_city_def_t;

typedef struct {
    const char *name;                // 省级行政区名(直辖市即市名)
    const suite_city_def_t *cities;  // 地级行政区数组(按常用顺序)
    int city_count;
} suite_province_def_t;

// 省级行政区数量(含直辖市/自治区/特别行政区/台湾省)。
int suite_city_province_count(void);

// 越界返回 NULL。
const suite_province_def_t *suite_city_province_at(int index);

// 按城市展示名查找所在省/市索引;未找到返回 false(输出不写)。
bool suite_city_find(const char *city_name, int *province, int *city);
