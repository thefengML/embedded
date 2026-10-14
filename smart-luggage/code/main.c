/* main.c —— 智能行李箱 · FreeRTOS 版主程序（重庆城市科技学院 毕业设计）。

   本文件只做三件事：硬件初始化 → 创建 6 个任务 → 启动调度器。

   六个任务（优先级数值越大越高）：
     log(1)   日志串行化 —— 全工程唯一直接写串口的地方
     hb(2)    OLED 刷新 + 每秒状态日志
     lock(3)  指纹帧解析 / 开锁
     alarm(3) 蜂鸣器仲裁
     gps(4)   GPS 的 NMEA 解析
     comm(5)  AT 状态机 / 位置上报 / 探活

   硬件：STM32F103C8（20 KB RAM / 64 KB Flash）
   工具链：标准外设库 V3.5 + Arm Compiler 6 + FreeRTOS V11.1.0
*/

#include "stm32f10x.h"
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "Uart.h"       /* USART1 —— GPS 的寄存器配置与发送（同时是调试输出口） */
#include "gps.h"        /* GPS 链路：队列 / 解析 / gps_task */
#include "ZW101.h"      /* USART2 —— 指纹 的寄存器配置与发送 */
#include "lock.h"       /* 指纹锁链路：PA1 EXTI / 帧队列 / lock_task */
#include "WIFI.h"       /* USART3 —— WiFi  */
#include "key.h"        /* 继电器 / 物理开关 */
#include "BUZ.h"        /* 蜂鸣器（开关动作由 alarm 模块仲裁） */
#include "ui.h"         /* OLED 共享访问层（内部带互斥量，含 OLED.h） */
#include "log.h"        /* 日志队列 + log_task */
#include "sys_state.h"  /* 状态中心（事件组） */
#include "alarm.h"      /* 告警任务 / 蜂鸣器仲裁 */

/* ---------------- 任务栈大小（单位：字 word，1 word = 4 字节）----------------
   取偏保守的值，原因有两点：
     1) LOG() 是宏 → Log_Printf() 里的 vsnprintf 跑在【调用者】的栈上，
        所以凡是会打日志的任务都要为格式化留出余量；
     2) 目前还没法上机测栈水位，宁可先给宽一点。

   注：GPS 任务的栈定义在 gps.h（GPS_TASK_STACK），因为那部分开销跟着那个模块走。 */
#define COMM_TASK_STACK   448U   /* 1792 B —— MQTT_Publish 拼 JSON + 日志格式化 */
#define HB_TASK_STACK     256U   /* 1024 B —— OLED 刷新 + 日志格式化            */

/* ---------------- 任务优先级（数值越大越高）----------------
   log(1) < hb(2) < lock(3) = alarm(3) < gps(4) < comm(5)

   排法依据是"截止时间"，不是"谁重要"：
     · comm 最紧 —— 云端心跳一超时就会被判离线
     · gps 次之  —— 定位数据要喂给上报，但不必抢在通信前面
     · lock / alarm 再次 —— 都是"人在等结果"的交互场景，几百毫秒级即可
     · hb 最松   —— 只刷屏，晚一点没人看得出来

   各任务的优先级宏分别定义在 log.h / lock.h / alarm.h / gps.h 里。

   为什么 alarm 不给更高：告警动作是"响蜂鸣器"，人对声音的感知阈值在数百毫秒
   量级 —— 晚 20 ms 没人听得出来。真正紧的是通信。别把优先级按"感觉上重要"排。 */
#define COMM_TASK_PRIO    5U     /* 通信：截止时间最紧 */
#define HB_TASK_PRIO      2U     /* 心跳：只做显示刷新，低于通信 */

static void vTaskHeartbeat(void *pvParameters);
/* vTaskComm() 不在这里声明 —— 它整条属通信链路，声明在 WIFI.h、实现在 WIFI.c */

int main(void)
{
    /* ---- 1. 中断优先级分组：FreeRTOS 要求 4 位全部作抢占优先级 ---- */
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);

    /* ---- 2. 硬件初始化 ----
       全部在调度器启动前完成，只做寄存器级配置。

       ⚠️ 这里绝对不能调用 vTaskDelay() —— 调度器还没跑，pxCurrentTCB 为空，
          调用会直接 HardFault。需要等待的初始化（WiFi 接入）已挪进 comm_task。

       ⚠️ Gps_Init() / Lock_Init() 必须排在 Uart_*_init() 之前：
          它们建的是接收队列，而那些 init 一开 RXNE 中断就可能来数据，
          队列没建好，那段数据会被丢掉。
          （这两个 Init 只建队列/信号量，不碰寄存器，放这里不会破坏初始化顺序。） */
    Gps_Init();
    Lock_Init();      /* 指纹帧队列 + PA1 触摸信号量 */

    Uart_Iint();      /* USART1 @9600   —— GPS（ATGM336H）   */
    Uart_2_init();    /* USART2 @9600   —— 指纹（ZW101）     */
    Uart_3_Init();    /* USART3 @115200 —— WiFi（ESP8266）   */

    /* ⚠️ 必须排在 Uart_2_init() 之后：PA1 得先被配成输入态，
       才能把 EXTI1 线接到它上面。 */
    Lock_GpioInit();  /* PA1 = 指纹触摸感应 → EXTI 上升沿   */

    Key_init();       /* PB12 继电器 / PA0 物理开关          */
    BUZ_init();       /* TIM1_CH1N PWM                       */
    BUZ_Stop();       /* 上电静音                            */
    OLED_Init();      /* 软件 I2C（PB6/PB7）—— 驱动自身初始化，必须最先 */
    UI_Clear();

    /* ---- 3. 画固定标签（上电时画一次，之后只刷数值）----
          经度 : <值>          (y = 0)
          纬度 : <值>          (y = 16)
          开关状态 : 开/关      (y = 36)

       坐标值由 hb 任务每 5 秒刷一次，这里的冒号位置和值的起点（x = 40 / 72）
       都按屏幕坐标定好，改动时注意别和 hb 的刷新区重叠。 */
    UI_ShowChinese(0, 0,  "经度");
    UI_ShowChinese(0, 16, "纬度");
    UI_ShowChinese(0, 36, "开关状态");
    UI_ShowChar(32, 0,  ':', OLED_8X16);
    UI_ShowChar(32, 16, ':', OLED_8X16);
    UI_ShowChar(64, 32, ':', OLED_8X16);
    UI_ShowChinese(72, 36, "关");     /* 上电默认锁闭 */
    UI_Update();

    /* ---- 4. 中间件 / 共享资源层 ----
       日志队列、OLED 互斥量、状态事件组。必须在建任务之前建好：
       Log_Init() 没跑之前调 LOG() 不会崩，但消息会被直接丢弃。 */
    Log_Init();       /* 建 log_q（log_task 在下面统一创建） */
    UI_Init();        /* 建 OLED 互斥量                      */
    Sys_Init();       /* 建状态事件组                        */

    /* 上电默认锁闭 —— 与上面写进 OLED 的"关"字保持一致。
       Clock_State 是未初始化全局，本来就是 0；这里显式同步一次，
       是为了让"状态位与显示一致"有个明确的落点，而不是靠"碰巧是 0"。 */
    Sys_UpdateBit(SYS_BIT_UNLOCKED, Lock_IsLocked());

    /* ---- 5. 建任务（优先级 log < hb < lock = alarm < gps < comm）----
       ⚠️ 这里的返回值没有检查。堆不足时 xTaskCreate 会返回失败，
          该任务会静默地建不起来 —— 编译、链接都正常，只是某个功能完全不动，
          属于比较难查的一类故障。后续可加断言。 */
    xTaskCreate(vTaskLog,       "log",   LOG_TASK_STACK,   NULL, LOG_TASK_PRIO,   NULL);
    xTaskCreate(vTaskHeartbeat, "hb",    HB_TASK_STACK,    NULL, HB_TASK_PRIO,    NULL);
    xTaskCreate(vTaskLock,      "lock",  LOCK_TASK_STACK,  NULL, LOCK_TASK_PRIO,  NULL);
    xTaskCreate(vTaskAlarm,     "alarm", ALARM_TASK_STACK, NULL, ALARM_TASK_PRIO, NULL);
    xTaskCreate(vTaskGps,       "gps",   GPS_TASK_STACK,   NULL, GPS_TASK_PRIO,   NULL);
    xTaskCreate(vTaskComm,      "comm",  COMM_TASK_STACK,  NULL, COMM_TASK_PRIO,  NULL);

    /* ---- 6. 启动调度器（正常情况下不会返回）---- */
    vTaskStartScheduler();

    /* 内存不足导致调度器没能启动时才会走到这里 */
    while (1)
    {
    }
}

/* 心跳任务：每秒打一行状态日志；每 5 秒刷新一次 OLED 上的经纬度。

   为什么 5 秒刷一次而不是每秒：OLED 是软件 I2C，刷满 1024 字节要几十毫秒，
   每秒一次纯属白耗 CPU，而且数字每秒跳也不比 5 秒跳更有用。

   本任务仍是 OLED 的【唯一数值写者】；"开/关"那两个汉字由 lock.c（指纹 /
   物理开关）和 WIFI.c（云端命令）写，位置固定在 (72,36)，
   与坐标区（x = 40..104, y = 0/16）不重叠，互不干扰。
   所有访问都走 UI_xxx()，由互斥量串行化。

   日志里几个缩写的含义：
     g     GPS 累计收到的完整行数
     frm   指纹累计收到的完整帧数
     drop  日志队列丢弃条数（持续增长说明串口跟不上）
     st    状态位快照（见 sys_state.h）
     al    告警累计次数        bz  当前蜂鸣器该不该响 */
static void vTaskHeartbeat(void *pvParameters)
{
    char       text[20];
    GPS_Data_t fix;
    uint32_t   seconds   = 0U;
    uint8_t    oled_tick = 0U;

    (void)pvParameters;

    for (;;)
    {
        seconds++;

        /* --- 每 5 秒刷新一次经纬度 --- */
        oled_tick++;
        if (oled_tick >= 5U)
        {
            oled_tick = 0U;

            if (Gps_GetFix(&fix) != 0U)
            {
                (void)snprintf(text, sizeof(text), "%.4f", fix.lon_dec);
                UI_ShowString(40, 0, text, OLED_8X16);
                (void)snprintf(text, sizeof(text), "%.4f", fix.lat_dec);
                UI_ShowString(40, 16, text, OLED_8X16);
            }
            else
            {
                /* 没有有效定位时显示 "--"。
                   不能显示 0.0000 —— 那是几内亚湾的真实坐标，
                   看上去像定位成功了，反而误导。
                   这与 comm_task 里"没有定位就不上报"是同一条原则。 */
                UI_ShowString(40, 0, "--", OLED_8X16);
                UI_ShowString(40, 16, "--", OLED_8X16);
            }

            UI_Update();
        }

        LOG("[hb] %lu s g=%lu frm=%lu drop=%u st=%02lX al=%lu bz=%u\r\n",
            (unsigned long)seconds,
            (unsigned long)Gps_GetLineCount(),
            (unsigned long)Lock_GetFrameCount(),
            (unsigned)Log_GetDropped(),
            (unsigned long)Sys_GetBits(),
            (unsigned long)Alarm_GetCount(),
            (unsigned)Alarm_IsActive());

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ============================ FreeRTOS 钩子 ============================
   栈溢出检测和堆耗尽检测的回调。

   为什么提前打开这两项：栈溢出 / 堆耗尽的默认表现是"跑着跑着莫名飞了"，
   是最难排查的一类故障。让它在第一次上机时就明确报出来，比事后猜省得多。

   ⚠️ 两个钩子里【绝对不能】调用任何 FreeRTOS API，也不能用 LOG()（要投队列）
      或 UI_()（要取互斥量）—— 出问题的时候内核状态本身就是可疑的。
      所以只用最原始的轮询串口输出，也不能用 printf（会走 stdio 缓冲）。
   ================================================================== */

/* 栈溢出钩子（configCHECK_FOR_STACK_OVERFLOW = 2 时被调用）。
   参数：xTask = 出事的任务句柄（这里用不到）；pcTaskName = 它的任务名，
   打在串口上用于定位是哪个任务爆了。 */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;

    Uart_Send_Str("\r\n!! STACK OVERFLOW in task: ");
    Uart_Send_Str(pcTaskName);
    Uart_Send_Str("\r\n");

    for (;;)
    {
    }
}

/* 堆耗尽钩子（configUSE_MALLOC_FAILED_HOOK = 1 时被调用）。
   任务 / 队列 / 互斥量创建失败都会走到这里，典型原因是
   configTOTAL_HEAP_SIZE（12 KB）不够了。 */
void vApplicationMallocFailedHook(void)
{
    Uart_Send_Str("\r\n!! MALLOC FAILED: FreeRTOS heap exhausted\r\n");

    for (;;)
    {
    }
}
