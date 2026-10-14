#ifndef _SYS_STATE_H
#define _SYS_STATE_H

#include "stm32f10x.h"
#include "FreeRTOS.h"
#include "event_groups.h"

/* sys_state.h —— 系统状态中心：用事件组承载各子系统的布尔状态，作为单一事实来源。

   为什么需要这一层：
     "系统现在什么状态"会有三四个地方各问一次 —— hb 任务刷 OLED 要知道 GPS 有没有
     定位、网络通没通；comm 任务决定要不要上报；alarm 任务决定响不响；日志要打状态摘要。
     如果各自去问各模块，会立刻撞上两个问题：
       1) comm_task 的 online 是它循环里的局部量，外部根本读不到，只能新开全局变量 ——
          状态又多一份副本，迟早不一致；
       2) "等到 GPS 有定位【并且】网络在线再动作"这类组合条件，靠各自的全局变量只能
          反复轮询，轮询间隔就是响应延迟。

   为什么用事件组，而不是几个全局 uint8_t：
     单看"存状态"这一个动作，两者差不多（单字节在 Cortex-M3 上读写原子，不会撕裂）。
     事件组不可替代的地方在【等待】：alarm_task 要"没有告警时完全不耗 CPU，一旦有告警
     立刻醒来"。全局变量做不到"醒来"，只能周期轮询 —— 间隔就是延迟，调短又白白唤醒 CPU
     （本设备锂电池供电，空闲唤醒要算电）。Sys_WaitBits() 就是给这个用途准备的。

   位与"谁置谁清"：
     SYS_BIT_GPS_FIX      gps_task 解析出有效定位     gps_task（无定位 / 收不到行）
     SYS_BIT_NET_ONLINE   comm_task 连接状态机跑完    comm_task（探活连续失败）
     SYS_BIT_UNLOCKED     Open_Clock() 之后          Close_Clock() 之后
     SYS_BIT_ALARM        comm_task 判定链路已断      comm_task 重连成功
     SYS_BIT_REMOTE_BUZ   云端下发 BUZ:true           云端下发 BUZ:false

   纪律：位只由"知道这件事的模块"写，别处只读。想读用 Sys_GetBits()，想等用
         Sys_WaitBits()，不要自己另存一份。
*/

/* 事件位。
   用 (1UL << n) 而不是 0x01/0x02...，是为了加位时不用回头数位置。
   EventBits_t 在 32 位 tick 配置下是 uint32_t，高 8 位被内核留作控制位，
   理论上有 24 位可用，当前只用了低 5 位，余量充足。 */
#define SYS_BIT_GPS_FIX     ( ( EventBits_t ) ( 1UL << 0 ) )   /* GPS 当前有有效定位 */
#define SYS_BIT_NET_ONLINE  ( ( EventBits_t ) ( 1UL << 1 ) )   /* 云端链路在线       */
#define SYS_BIT_UNLOCKED    ( ( EventBits_t ) ( 1UL << 2 ) )   /* 箱锁处于"开"       */
#define SYS_BIT_ALARM       ( ( EventBits_t ) ( 1UL << 3 ) )   /* 本地告警进行中     */
#define SYS_BIT_REMOTE_BUZ  ( ( EventBits_t ) ( 1UL << 4 ) )   /* 云端请求蜂鸣       */

/* 会触发"响蜂鸣器"的位。alarm_task 等的就是这一组。 */
#define SYS_BITS_BUZZER     ( SYS_BIT_ALARM | SYS_BIT_REMOTE_BUZ )

/* 建事件组。必须在【调度器启动前】调用。
   建失败会走 vApplicationMallocFailedHook（main.c 里，会打印并停机）。 */
void Sys_Init(void);

/* 置位或清位，各模块唯一的写入口。
   bit —— 单个位（不要传多位掩码，语义会含糊）
   on  —— 非 0 = 置位，0 = 清位
   事件组的置位/清位内部自带临界区，可跨任务安全调用。
   本函数只在【任务】里被调用（初始化阶段各模块还不报状态）。 */
void Sys_UpdateBit(EventBits_t bit, uint8_t on);

/* 读一份当前所有位的快照（无阻塞）。 */
EventBits_t Sys_GetBits(void);

/* 等一组位里的【任意一位】被置起，返回事件位快照（不清除任何位 —— 位的清除归置位方负责）。
   bits       —— 要等的位掩码，如 SYS_BITS_BUZZER
   timeout_ms —— 超时（毫秒）。到点还没等到就返回当时的快照。
   【只在任务里调用】。这是本层存在的核心理由：无事的任务真正挂起，有事被立刻唤醒，
   不必周期轮询。 */
EventBits_t Sys_WaitBits(EventBits_t bits, uint32_t timeout_ms);

#endif
