/*
 * FreeRTOS configuration to measure the task layer on a Cortex-M0
 * (`make target-rt`): the kernel's generic ARM_CM0 port, static allocation
 * only. Only alfs_rtos.c is compiled against it.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#define configUSE_PREEMPTION 1
#define configUSE_IDLE_HOOK 0
#define configUSE_TICK_HOOK 0
#define configCPU_CLOCK_HZ 125000000
#define configTICK_RATE_HZ 1000
#define configMAX_PRIORITIES 3
#define configMINIMAL_STACK_SIZE 256
#define configMAX_TASK_NAME_LEN 16
#define configTICK_TYPE_WIDTH_IN_BITS TICK_TYPE_WIDTH_32_BITS

#define configSUPPORT_STATIC_ALLOCATION 1
#define configSUPPORT_DYNAMIC_ALLOCATION 0
#define configKERNEL_PROVIDED_STATIC_MEMORY 1

#define configUSE_MUTEXES 1
#define configUSE_TASK_NOTIFICATIONS 1
#define configUSE_TIMERS 0
#define configENABLE_MPU 0

#define INCLUDE_vTaskDelete 1
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_xTaskGetCurrentTaskHandle 1

#endif
