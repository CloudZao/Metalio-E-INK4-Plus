#pragma once
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 串行检查内部 RAM 预算并创建动态任务；失败时参数所有权仍归调用方。
 * @param core 核心编号或 tskNO_AFFINITY。
 * @param stack_caps 零使用内部栈；非零使用 WithCaps 创建，退出须用 vTaskDeleteWithCaps。
 * @note 与 JPEG 内部工作区共用预算锁，其他堆分配不受此锁保护。
 */
BaseType_t FirmwareCreateTask(TaskFunction_t entry, const char* name, uint32_t stack_bytes, void* argument,
                              UBaseType_t priority, TaskHandle_t* handle, BaseType_t core, uint32_t stack_caps);

#ifdef __cplusplus
}
#endif
