#ifndef _LOCK_H
#define _LOCK_H

#include "stm32f10x.h"
#include "FreeRTOS.h"
#include "task.h"



#define FINGER_FRAME_MAX   64U    /* 单帧上限。ZW101 应答帧通常 < 32 字节，GGA 那套不适用 */
#define FINGER_Q_DEPTH     4U     /* 队列深度：一次识别会回 3~4 帧（确认/采图/比对/结束） */
#define LOCK_TASK_PRIO     3U     /* 低于 gps(4)、comm(5)，高于 hb(2) */
#define LOCK_TASK_STACK    320U   /* 1280 B —— 解析里有 LOG 的 vsnprintf */

/* 软件资源（队列 / 信号量）。必须在【调度器启动前】、且【早于 Lock_GpioInit()】
 * 调用 —— EXTI 一开就可能触发中断，那时信号量还没建好会白丢一次触摸。 */
void Lock_Init(void);

/* PA1 的 EXTI 配置。必须在 Uart_2_init() 【之后】调用：
 * Uart_2_init() 负责把 PA1 配成输入模式（EXTI 需要引脚先处于输入态）。 */
void Lock_GpioInit(void);

void vTaskLock(void *pvParameters);

/* 由 USART2 的 ISR 调用：喂进一个字节，内部按 ZW101 协议成帧、整帧入队 */
void Lock_FingerRxByteFromISR(uint8_t c, BaseType_t *pWoken);

/* 当前锁状态（1 = 开）。直接读 key.c 的 Clock_State —— 那是权威来源，
 * 云端下发和指纹开锁都会更新它，不用再维护第二份状态。 */
uint8_t Lock_IsLocked(void);

/* 观测用 */
uint32_t Lock_GetFrameCount(void);      /* 收到的完整帧数 */
uint32_t Lock_GetDropCount(void);       /* 因超长/队列满被丢弃的帧数 */
uint32_t Lock_GetBadSumCount(void);     /* 校验和不通过的帧数 */

#endif
