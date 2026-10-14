#ifndef _GPS_H
#define _GPS_H

#include "stm32f10x.h"
#include "FreeRTOS.h"
#include "task.h"

/* gps.h —— GPS 链路：模块 ATGM336H / 串口 USART1 @9600。

   数据流：USART1 中断逐字节拼行 → 整行投入队列 → gps_task 取出解析 → 写入共享快照。

   为什么这么拆：
     · 中断里只做"拼行 + 投队列"，耗时极短；解析（strtok / atof）留给任务去干，
       否则会把同优先级和更低优先级的中断全部挡住。
     · 拔掉 GPS 时，任务只是阻塞在队列上，其它任务完全不受影响。

   为什么不用 IDLE 中断成帧：
     IDLE 适合"帧之间必有总线空闲"的二进制协议。而 NMEA 0183 是 \r\n 结尾的文本
     行协议、模块连续输出，行内和行间都不保证有一个字符时间的空闲 —— 用 IDLE 反而
     会把一行切成两半。对行协议，按行尾字符成帧最稳。
*/

#define GPS_LINE_MAX      96U     /* 单行缓冲上限（含结尾 '\0'）。GGA 最长约 82 字符 */
#define GPS_Q_DEPTH       4U      /* 队列深度：4 行 ≈ 0.5~1 秒的缓冲 */
#define GPS_TASK_PRIO     4U      /* 优先级：高于 hb(2)，低于 comm(5) */

/* GPS 任务的栈大小，单位：字（word），384 字 = 1536 字节。
   给这么大是因为 ParseGGA 里的 strtok / strncpy / atof，
   加上 LOG() 的 vsnprintf，都跑在本任务的栈上。 */
#define GPS_TASK_STACK    384U

#define GPS_RX_TIMEOUT_MS 3000U   /* 队列等待超时：3 秒收不到任何一行即判为断线 */

/* 定位数据快照。由 gps_task 写，其它任务一律通过 Gps_GetFix() 加锁读。 */
typedef struct {
    char    lat[15];      /* 纬度 + 方向，如 "2936.738N" */
    char    lon[15];      /* 经度 + 方向，如 "10631.223E" */
    double  lat_dec;      /* 十进制纬度（已由度分格式换算） */
    double  lon_dec;      /* 十进制经度 */
    uint8_t valid;        /* 1 = 当前定位有效（GGA 的 quality > 0） */
} GPS_Data_t;

/* 建队列 + 互斥量。必须在【调度器启动前】、且【早于 Uart_Iint()】调用：
   Uart_Iint() 一开 RXNE 中断就可能来数据，队列没建好那段数据会被丢掉。 */
void Gps_Init(void);

/* GPS 任务：从队列取出一整行 → 解析 → 状态跳变时记日志并更新状态位。
   队列空时阻塞等待，最长 GPS_RX_TIMEOUT_MS。 */
void vTaskGps(void *pvParameters);

/* 由 USART1 的中断服务函数调用：喂进一个字节，内部负责拼行 + 整行入队。
   参数：c      —— 收到的字节
         pWoken —— 出口参数，把"是否唤醒了更高优先级任务"带回给 ISR，
                   交给 portYIELD_FROM_ISR 使用 */
void Gps_RxByteFromISR(uint8_t c, BaseType_t *pWoken);

/* 取一份定位快照（内部加锁，可跨任务安全调用）。
   参数：out —— 存放快照的地方
   返回：1 表示当前定位有效；0 表示无效（此时 out 里是上一次的残留坐标） */
uint8_t Gps_GetFix(GPS_Data_t *out);

/* 观测用：累计收到的完整行数。 */
uint32_t Gps_GetLineCount(void);

/* 观测用：因超长被丢弃的残行数。持续增长说明收到的不是正常 NMEA。 */
uint32_t Gps_GetOverflowCount(void);

#endif
