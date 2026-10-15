#ifndef _ALARM_H
#define _ALARM_H

#include "stm32f10x.h"
#include "FreeRTOS.h"
#include "task.h"


#define ALARM_TASK_PRIO     3U     /* 与 lock 同级：比刷屏(2)要紧，但真正紧的是通信(5) */
#define ALARM_TASK_STACK    224U   /* 896 B —— 每轮只做位运算和开关，但 LOG 的
                                    *          vsnprintf 跑在本任务栈上 */

/* 断连告警总开关。
   0 = 屏蔽（当前值）：链路断了不会置 SYS_BIT_ALARM —— 蜂鸣器不响、OLED 的 A 位不亮、
       "重连后补报告警解除"也不触发，看上去就像这个功能不存在。
       【代码整条保留】，启用时把这一个 0 改成 1 即可，不必改任何调用点。
   1 = 启用。

   默认关闭的原因：它与云端下发的 BUZ 命令共用同一个蜂鸣器，两条路径同时触发时
   现象不易区分。因此默认只走远程路径，本地路径用这个宏按需启停。

	  它与【云端下发的 BUZ 命令】互不影响：那是原有设计（客户端判超距 → 下发 BUZ:true），
      保持常开，走的是 SYS_BIT_REMOTE_BUZ，跟这个开关无关。 */
#define ALARM_ENABLE_LINK_ALARM   0

/* 鸣叫节奏：20 步 x 100 ms = 2 秒一周期，前 2 步（200 ms）响 */
#define ALARM_STEP_MS       100U
#define ALARM_ON_STEPS      2U
#define ALARM_CYCLE_STEPS   20U

/* 空闲时的等待上限。等待本身由事件组唤醒，"超时"只是兜底：
   万一某次置位发生在等待建立之后（理论上不会），最多 1 秒也能自纠。 */
#define ALARM_IDLE_WAIT_MS  1000U

void vTaskAlarm(void *pvParameters);

/* 云端下发 BUZ 命令的入口（取代原来直接调 BUZ_Open/BUZ_Stop）。
   on —— 非 0 = 请求鸣响，0 = 取消
   它只改状态位，真正的开关由 alarm_task 做 —— 因为同一时刻可能还有本地告警也想让
   蜂鸣器响，得由一处统一裁决。 */
void Alarm_SetRemoteBuzzer(uint8_t on);

/* 断连告警（comm_task 的 AT 探活连续失败时置位，重连成功时清除）。
   active —— 非 0 = 进入断连告警，0 = 解除
   受 ALARM_ENABLE_LINK_ALARM 控制：该宏为 0 时本函数【什么都不做】（连状态位都不置），
   调用点可以原样保留。 */
void Alarm_SetLinkAlarm(uint8_t active);

/* 观测用 */
uint8_t  Alarm_IsActive(void);    /* 当前是否有任何来源在要求鸣响 */
uint32_t Alarm_GetCount(void);    /* 累计进入过多少次告警（含远程请求），用于核对 */

#endif
