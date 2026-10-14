#ifndef _LOCK_H
#define _LOCK_H

#include "stm32f10x.h"
#include "FreeRTOS.h"
#include "task.h"

/* lock.h —— 指纹锁链路（ZW101 · USART2 @9600 + PA1 触摸感应 + PB12 继电器）。

   裸机版的做法与问题（全工程最典型的一处最值）：

       if (PA1 == SET) {                       // 检测到手指
           ZW101_SendCommand(PS_AutoIdentify); // 发"自动识别"命令
           while (PA1 == SET) { }              // ← 死等手指移开！
       }
       if (isFingerBuf_has_Data()) {
           process_byte(GET_FingerBuf_Data()); // 逐字节喂给帧解析状态机
       }

     · 那个 while 死等期间【整个主循环停摆】，而这期间照样有数据在进来：
       GPS 缓冲只有 200 字节，9600 bps 下 0.2 秒就写满 → 丢帧；
       WiFi 缓冲 512 字节，115200 bps 下 0.045 秒就写满 → 丢帧。
       实际后果：手指按在传感器上的那几秒，手机远程开锁是失灵的。
       这不是理论推测，是结构必然 —— 也是"为什么要上 RTOS"最有说服力的例子。
     · 帧解析靠主循环轮询喂字节，主循环一忙就漏字节 → 指纹帧被截断。
     · frame_buf[64] 没有上界校验，长度字段异常时会越界写。

   现在：
     · PA1 改 EXTI 上升沿中断 → 给信号量 → 唤醒 lock_task
     · 指纹帧在 ISR 里按 ZW101 协议成帧（EF 01 + 地址 + 标识 + 长度 + 数据 + 校验）
       → 整帧投队列 → lock_task 解析
     · 【不再死等手指移开】—— 发完命令立刻回去收帧，手指按多久都不影响别的任务
     · 补上帧长上界校验（原代码缺），畸形帧直接丢弃并计数

   ⚠️ PA1 走 EXTI 之后，别的地方就【不能再用 GPIO_ReadInputDataBit 轮询它】来代替
      "检测触摸"了 —— 那样既会漏掉短促的触摸，也失去了中断的意义。
*/

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
