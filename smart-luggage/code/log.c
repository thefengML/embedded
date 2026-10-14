/* log.c —— 日志基础设施：队列 + log_task。设计说明见 log.h 顶部。 */

#include "log.h"

#include <stdarg.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "Uart.h"     /* 输出走驱动层的 Uart_Send_Str，不走 stdio */

/* 日志队列。所有任务的 LOG() 都往这里投，log_task 从里面取。 */
static QueueHandle_t s_log_q   = NULL;

/* 因队列满被丢弃的条数。只有任务会写它（中断不写），所以不用 volatile。 */
static uint16_t      s_dropped = 0;

/* 创建日志队列。必须在建 log_task 之前、调度器启动之前调用。 */
void Log_Init(void)
{
    s_log_q   = xQueueCreate(LOG_QUEUE_LEN, LOG_MSG_LEN);
    s_dropped = 0;
}

/* 把一条日志格式化后投进队列。任务里请用 LOG() 宏，不要直接调它。
   参数：fmt 及其后的可变参数，用法与 printf 相同。 */
void Log_Printf(const char *fmt, ...)
{
    char    buf[LOG_MSG_LEN];
    va_list ap;

    if (s_log_q == NULL)
    {
        return;                   /* 队列还没建起来，丢弃 */
    }

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* 超时给 0：投不进去就丢。
       这里【故意】不用 portMAX_DELAY —— 打日志把业务任务卡住是本末倒置。 */
    if (xQueueSend(s_log_q, buf, 0) != pdPASS)
    {
        s_dropped++;
    }
}

/* 查询因队列满被丢弃的日志条数。 */
uint16_t Log_GetDropped(void)
{
    return s_dropped;
}

/* 日志任务 —— 全工程唯一"直接写串口"的地方。
   优先级最低（1，仅高于 IDLE）：串口再慢也挡不住业务任务，
   代价是队列满时丢日志，这是有意的取舍。

   这里没有用 printf，而是直接调驱动层的 Uart_Send_Str()。原因：
   printf 走标准 C 库的 stdio，缓冲行为由库内部实现决定；而本工程为了让
   标准库能链接，自己定义了 `struct __FILE`，再交给库去操作这块内存是不安全的
   （库的 FILE 布局可能更大）。直接走驱动层，输出时机确定、肉眼可预期。
   （printf/fputc 的重定向仍然保留在 Uart.c 里，供零散调试使用。） */
void vTaskLog(void *pvParameters)
{
    char msg[LOG_MSG_LEN];

    (void)pvParameters;

    for (;;)
    {
        if (xQueueReceive(s_log_q, msg, portMAX_DELAY) == pdPASS)
        {
            Uart_Send_Str(msg);
        }
    }
}
