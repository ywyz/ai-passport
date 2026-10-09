// main/suite_app.h —— 五合一应用外壳:页面栈 + 输入分发。
// 页面在 enter/exit 中自管 LVGL 对象与 timer/任务(删页前先停 producer)。
// 输入经 suite_input reducer 归约为语义动作后投递给当前页。
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#include "suite_input.h"

typedef struct suite_page {
    const char *name;
    void (*enter)(void);                 // 持 LVGL 锁创建 UI 并载入
    void (*exit)(void);                  // 已停 producer 后,持 LVGL 锁删除 UI
    void (*action)(suite_action_t act);  // 输入任务上下文;页面自管 LVGL 锁
} suite_page_t;

// 页面栈深度预算:首页(1)→启动器(2)→设置(3)→省份(4)→城市(5),
// 预留 1 层给后续子页。每层仅一个页面指针,增大成本可忽略。
#define SUITE_PAGE_STACK_MAX 6

// 初始化外壳并进入 boot 页(持 LVGL 锁由内部处理)。
bool suite_app_start(const suite_page_t *boot_page);

// 初始化按键队列、分发任务与 BSP 按键;须在 suite_app_start 之前调用。
esp_err_t suite_app_input_init(void);

// 压入新页;超过栈深返回 false。当前页 exit → 新页 enter。
bool suite_app_push(const suite_page_t *page);

// 回退一页;已在栈底返回 false。
bool suite_app_back(void);

const suite_page_t *suite_app_current(void);
