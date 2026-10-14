#ifndef _LOG_H
#define _LOG_H

#include <stdint.h>

/* log.h —— 日志基础设施。

   把调试输出从"各任务直接 printf"改成"投队列 → 由最低优先级的 log_task 串行输出"。

   为什么必须这么做：
     1) 调试口是 USART1(PA9) @9600，而 9600 是被 GPS 锁死的（TX/RX 必须同波特率）。
        9600 下一个字符约 1.04 ms —— 任务里直接 printf 一行 20 字符，
        就是把这 20 ms 整段忙等掉，等于把 CPU 白送给串口。
     2) 多个任务同时往一个串口写，字符会互相插入，日志乱行。
     3) 队列满时直接丢弃并计数，绝不阻塞调用者 ——
        日志是为了看清系统，不能反过来把业务任务拖住。

   用法：
     · 任务里一律用 LOG("...", ...)，不要再直接调用 printf
     · 初始化：main() 里、调度器启动之前调 Log_Init()，再建 log_task

   注意：
     · 本接口只在【任务】里安全。中断里要打日志得另开 Log_PrintfFromISR()
       （现在没有这个需求，等真需要再加）
     · 调度器启动之前调用 LOG() 不会崩，但队列还没建，消息会被丢弃
*/

#define LOG_QUEUE_LEN     8U     /* 队列深度：最多暂存 8 条日志 */
#define LOG_MSG_LEN       64U    /* 单条最大长度（含结尾 '\0'）—— 定长才好用队列 */

/* log_task 的栈大小，单位：字（word），即 1024 字节。
   注意 Log_Printf() 里的 vsnprintf 跑在【调用者】的栈上，
   所以凡是会打日志的任务，栈都要留出标准库格式化的开销。 */
#define LOG_TASK_STACK    256U

#define LOG_TASK_PRIO     1U     /* 只高于 IDLE，最低业务优先级 */

/* 创建日志队列。必须在建 log_task 之前、调度器启动之前调用。 */
void Log_Init(void);

/* 日志任务：全工程唯一直接写串口的地方。取到一条就发一条，队列空则永久等待。 */
void vTaskLog(void *pvParameters);

/* 声明成 printf 风格的函数，让编译器在编译期就校验 LOG("...", 参数) 的
   格式串与实参是否匹配（少参数、类型不符都会直接报出来）。
   ARM Compiler 6 基于 clang，支持这个 GNU 属性。 */
#if defined(__GNUC__) || defined(__clang__)
#define LOG_PRINTF_ATTR   __attribute__((format(printf, 1, 2)))
#else
#define LOG_PRINTF_ATTR
#endif

/* 格式化后投队列。任务里请用下面的 LOG() 宏。 */
void Log_Printf(const char *fmt, ...) LOG_PRINTF_ATTR;

/* 因队列满被丢弃的条数。持续增长说明串口跟不上，日志本身成了瓶颈。 */
uint16_t Log_GetDropped(void);

/* 任务里打日志统一用这个。
   做成宏而不是直接调 Log_Printf，是为了以后要加等级过滤
   （比如 LOG_D/LOG_I，或编译期关掉全部日志）时不用改任何调用点。 */
#define LOG(...)   Log_Printf(__VA_ARGS__)

#endif
