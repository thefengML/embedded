#ifndef _ALARM_H
#define _ALARM_H

#include "stm32f10x.h"
#include "FreeRTOS.h"
#include "task.h"

/* alarm.h —— 告警链路：蜂鸣器的仲裁者 + 断连告警的落点。

   蜂鸣器的两个来源：
     ① 距离超限：手机/网页端算距离，超限后下发 BUZ:true，响到 BUZ:false。
                固件侧只把入口从"直接调 BUZ_Open()"改成 Alarm_SetRemoteBuzzer()，
                纳入统一仲裁。
     ② 断连：    这件事只能固件侧判 —— 手机不可能知道箱子是掉线还是关机。
                comm_task 的 AT 探活连续失败 → Alarm_SetLinkAlarm(1) → 本地蜂鸣；
                重连成功 → 清除，并上报云端一条"已恢复"。
                ⚠️ 当前由 ALARM_ENABLE_LINK_ALARM 屏蔽（= 0），见下面那个宏的说明。
                   调用点仍在、代码整条保留，打开开关即生效。

   为什么需要一个任务，而不是在命令处理里直接开关蜂鸣器：
     1)【多个来源需要仲裁】蜂鸣器是单份物理资源，有两个来源要它的控制权。各自在自己的
        调用点写 BUZ_Open/Stop，后调用的会盖掉前一个的效果 —— 表现为"云端让它响，
        过一会儿断连了反而安静了"。仲裁必须有一个统一的地方做。
     2)【告警要有时间维度】断连告警不是"一直长响"，而是间断鸣叫，这是随时间演进的
        状态，得有个循环去驱动它。

   鸣叫策略：
     云端请求（找箱子）→ 【持续响】，直到收到 BUZ:false。语义是用户在主动找箱子，
                        一会儿一停会让人以为没在工作。
     本地告警（断连）  → 【间断响】响 200 ms / 停 1800 ms，周期 2 秒。占空比只有 10%，
                        锂电池供电下省电，也比连续长鸣更容易被听出是报警而不是故障。

   本模块【不判断告警条件】，只负责"被告知有告警之后怎么做"。判断条件的是知道情况的
   那个任务（comm_task 判链路、客户端判距离）—— 这样告警模块不必知道 GPS 怎么解析、
   AT 怎么发，换硬件它不用改。
*/

#define ALARM_TASK_PRIO     3U     /* 与 lock 同级：比刷屏(2)要紧，但真正紧的是通信(5) */
#define ALARM_TASK_STACK    224U   /* 896 B —— 每轮只做位运算和开关，但 LOG 的
                                    *          vsnprintf 跑在本任务栈上 */

/* 断连告警总开关。
   0 = 屏蔽（当前值）：链路断了不会置 SYS_BIT_ALARM —— 蜂鸣器不响、OLED 的 A 位不亮、
       "重连后补报告警解除"也不触发，看上去就像这个功能不存在。
       【代码整条保留】，启用时把这一个 0 改成 1 即可，不必改任何调用点。
   1 = 启用。

   当前为何屏蔽：这条链路（探活失败 → 本地鸣响 → 恢复后上报）依赖"拔掉 ESP8266 电源"
   来验证，而手上没有杜邦线。先屏蔽是为了避免上机时"蜂鸣器自己响起来"这种一时不好
   判断的现象 —— 那会把注意力从真正要验证的东西上引开。

   ⚠️ 它与【云端下发的 BUZ 命令】互不影响：那是原有设计（客户端判超距 → 下发 BUZ:true），
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
