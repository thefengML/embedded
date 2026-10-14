/* gps.c —— GPS 链路：中断拼行 + 队列传递 + 任务解析。设计说明见 gps.h 顶部。 */

#include "gps.h"
#include "log.h"
#include "sys_state.h"   /* SYS_BIT_GPS_FIX —— 定位状态的对外可见副本 */

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ---------------- 行拼装缓冲 ----------------
   只被 USART1 的中断读写，天然不会并发（同一个中断不会重入自己），
   所以不需要加锁、也不需要 volatile 修饰数组内容。 */
static char     s_rx_line[GPS_LINE_MAX];   /* 一行 NMEA 的暂存区 */
static uint16_t s_rx_idx = 0U;             /* 当前写到了第几个字节（0 = 还没开始） */

/* 观测计数。中断写、任务读 —— 两个上下文都会访问，所以要 volatile。
   32 位在 Cortex-M3 上单次读写是原子的，不需要额外加锁。 */
static volatile uint32_t s_line_cnt = 0U;   /* 收到的完整行数 */
static volatile uint32_t s_ovf_cnt  = 0U;   /* 因超长被丢弃的残行数 */

/* ---------------- 队列 / 锁 / 共享数据 ---------------- */

/* 接收队列：中断往里投整行，gps_task 从里面取。 */
static QueueHandle_t     s_gps_q    = NULL;

/* 保护下面 s_fix 的互斥量。 */
static SemaphoreHandle_t s_gps_lock = NULL;

/* 最新的定位数据。只由 gps_task 写，别的任务一律通过 Gps_GetFix() 加锁读。
   注意它是结构体（含两个 double），非原子 —— 不加锁可能读到
   "半个经度 + 半个纬度"的撕裂数据。
   5 个字段全部显式列出，是为了避开 clang 的 -Wmissing-field-initializers 告警。 */
static GPS_Data_t        s_fix = { {0}, {0}, 0.0, 0.0, 0U };

/* 取锁 / 放锁。与 ui.c 同一套约定：互斥量没建、或调度器还没启动时直接放行。
   （调度器启动前调用 xSemaphoreTake 是 FreeRTOS 不支持的用法。） */
static void Gps_Lock(void)
{
    if ((s_gps_lock != NULL) &&
        (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED))
    {
        (void)xSemaphoreTake(s_gps_lock, portMAX_DELAY);
    }
}

static void Gps_Unlock(void)
{
    if ((s_gps_lock != NULL) &&
        (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED))
    {
        (void)xSemaphoreGive(s_gps_lock);
    }
}

/* 建接收队列和互斥量。必须在建 gps_task 之前、且早于 Uart_Iint() 调用。
   建失败会走 vApplicationMallocFailedHook（main.c 里会打印并停机），
   所以这里不处理返回值。 */
void Gps_Init(void)
{
    s_gps_q    = xQueueCreate(GPS_Q_DEPTH, GPS_LINE_MAX);
    s_gps_lock = xSemaphoreCreateMutex();

    s_rx_idx   = 0U;
    s_line_cnt = 0U;
    s_ovf_cnt  = 0U;
}

/* ============================ 中断侧：拼行 + 入队 ============================ */

/* 喂进一个字节。遇到换行符说明一行拼完了：去掉尾部 '\r'、补 '\0'、
   整行投进队列，然后索引归零准备下一行。
   参数：c      —— 收到的字节
         pWoken —— 出口参数，原样转交给 xQueueSendFromISR（本函数自己不看不改） */
void Gps_RxByteFromISR(uint8_t c, BaseType_t *pWoken)
{
    if (s_gps_q == NULL)
    {
        return;                 /* 队列还没建（初始化阶段），直接丢弃 */
    }

    if (c == '\n')
    {
        /* 行尾：去掉 '\r'，补 '\0'，整行入队 */
        if ((s_rx_idx > 0U) && (s_rx_line[s_rx_idx - 1U] == '\r'))
        {
            s_rx_idx--;
        }
        s_rx_line[s_rx_idx] = '\0';

        if (s_rx_idx > 0U)
        {
            s_line_cnt++;
            /* 队列满就丢弃（不阻塞、不重试）—— 中断里不能等，
               而且 GPS 是周期输出的，下一行马上就到。 */
            (void)xQueueSendFromISR(s_gps_q, s_rx_line, pWoken);
        }
        s_rx_idx = 0U;
    }
    else
    {
        if (s_rx_idx < (GPS_LINE_MAX - 1U))
        {
            s_rx_line[s_rx_idx] = (char)c;
            s_rx_idx++;
        }
        else
        {
            /* 缓冲满了还没见到行尾 —— 收到的不是 NMEA 行（或模块乱码）。
               明确丢弃这个残行并计数，不让它无限增长。 */
            s_ovf_cnt++;
            s_rx_idx = 0U;
        }
    }
}

/* ============================ 解析 ============================ */

/* NMEA 的 ddmm.mmmm 格式 → 十进制度。
   例：2936.738N → 29 + 36.738/60 = 29.6123
   参数：dm_str —— 形如 "2936.738" 的度分字符串
         dir    —— 方向字符 N/S/E/W，S 和 W 取负值 */
static double convert_dm_to_deg(const char *dm_str, char dir)
{
    double dm      = atof(dm_str);
    int    degrees = (int)(dm / 100);
    double minutes = dm - degrees * 100;
    double decimal = degrees + minutes / 60.0;

    if ((dir == 'S') || (dir == 'W'))
    {
        decimal = -decimal;
    }
    return decimal;
}

/* 把解析结果写进共享结构（加锁）。 */
static void Gps_SetFix(const GPS_Data_t *fix)
{
    Gps_Lock();
    s_fix = *fix;
    Gps_Unlock();
}

/* 解析一行。
   返回 1：GGA 且 quality > 0，已更新定位
   返回 0：GGA 但 quality == 0（搜星中，valid 置 0），或这一行不是 GGA（不动数据）
   参数：line —— 一整行 NMEA 文本（以 '\0' 结尾） */
static uint8_t ParseGGA(const char *line)
{
    char    buffer[GPS_LINE_MAX];
    char    lat_str[12] = {0};
    char    lon_str[12] = {0};
    char    lat_dir = 0;
    char    lon_dir = 0;
    uint8_t quality = 0;
    char   *token;
    int     field = 0;

    if (strstr(line, "GGA") == NULL)
    {
        return 0U;              /* 不是 GGA（RMC/GSA/GSV…），本次不解析 */
    }

    /* strtok 会把分隔符就地替换成 '\0'，会改原字符串，
       所以必须先在本地拷一份（入参是 const，本来也不许改）。 */
    strncpy(buffer, line, sizeof(buffer) - 1U);
    buffer[sizeof(buffer) - 1U] = '\0';

    token = strtok(buffer, ",");
    while ((token != NULL) && (field <= 10))
    {
        switch (field)
        {
            case 2:     /* 纬度 ddmm.mmmm */
                strncpy(lat_str, token, sizeof(lat_str) - 1U);
                lat_str[sizeof(lat_str) - 1U] = '\0';
                break;

            case 3:     /* 纬度方向 N/S */
                lat_dir = token[0];
                break;

            case 4:     /* 经度 dddmm.mmmm */
                strncpy(lon_str, token, sizeof(lon_str) - 1U);
                lon_str[sizeof(lon_str) - 1U] = '\0';
                break;

            case 5:     /* 经度方向 E/W */
                lon_dir = token[0];
                break;

            case 6:     /* 定位质量：0 = 无效，1 = 单点，2 = 差分… */
                quality = (uint8_t)atoi(token);
                break;

            default:
                break;
        }
        token = strtok(NULL, ",");
        field++;
    }

    if ((quality > 0U) && (lat_str[0] != '\0') && (lon_str[0] != '\0'))
    {
        GPS_Data_t tmp;

        /* 用 snprintf 而不是 sprintf：便于编译器做边界检查，
           且 12 字符的坐标串 + 方向 + '\0' 一定落在 lat/lon[15] 里 */
        (void)snprintf(tmp.lat, sizeof(tmp.lat), "%s%c", lat_str, lat_dir);
        (void)snprintf(tmp.lon, sizeof(tmp.lon), "%s%c", lon_str, lon_dir);

        tmp.lat_dec = convert_dm_to_deg(lat_str, lat_dir);
        tmp.lon_dec = convert_dm_to_deg(lon_str, lon_dir);
        tmp.valid   = 1U;

        Gps_SetFix(&tmp);
        return 1U;
    }

    /* 收到 GGA 但还没定位（刚上电搜星时会持续几十秒）。
       保留上一次的坐标，只把 valid 打下去。 */
    Gps_Lock();
    s_fix.valid = 0U;
    Gps_Unlock();

    return 0U;
}

/* ============================ 任务 ============================ */

/* GPS 任务：从队列取出整行 → 解析 → 只在状态跳变时记日志。

   两条刻意的设计：
     1) 不刷 OLED —— OLED 统一由 hb 任务刷（单写者），这里只更新共享数据。
        少了并发写屏，也少了无谓的整屏刷新（软件 I2C 刷 1024 字节不便宜）。
     2) 不每次解析都打日志 —— GPS 每秒吐 5~10 行，9600 下每条日志要占十几毫秒
        串口时间，全打会把日志队列刷爆。只在"定位有无"跳变时打一条。 */
void vTaskGps(void *pvParameters)
{
    char    line[GPS_LINE_MAX];
    uint8_t state = 0U;         /* 上一次对外可见的定位状态（用于只在跳变时报） */

    (void)pvParameters;

    for (;;)
    {
        if (xQueueReceive(s_gps_q, line, pdMS_TO_TICKS(GPS_RX_TIMEOUT_MS)) == pdPASS)
        {
            uint8_t now = ParseGGA(line);

            if (now != state)
            {
                if (now != 0U)
                {
                    GPS_Data_t fix;

                    (void)Gps_GetFix(&fix);
                    LOG("[gps] FIX lat=%.5f lon=%.5f\r\n",
                        fix.lat_dec, fix.lon_dec);
                }
                else
                {
                    LOG("[gps] GGA without fix (searching)\r\n");
                }
                state = now;

                /* 定位状态只在这里对外发布一次（跳变时）。
                   别处想知道"GPS 现在有没有定位"，读这一位就够了，
                   不必各自去调 Gps_GetFix() —— 那还会顺带拷一份 double 结构。 */
                Sys_UpdateBit(SYS_BIT_GPS_FIX, now);
            }
        }
        else
        {
            /* 队列超时 = 这 3 秒内一行都没收到：拔线 / 模块掉电 / 波特率不符。
               把共享状态标成无效，让 UI 显示 "--"、上报任务知道没数据。 */
            uint8_t was;

            Gps_Lock();
            was         = s_fix.valid;
            s_fix.valid = 0U;
            Gps_Unlock();

            /* 收不到行 = 定位肯定无效，状态位也要一起打下去 */
            Sys_UpdateBit(SYS_BIT_GPS_FIX, 0U);

            if ((was != 0U) || (state != 0U))
            {
                LOG("[gps] no line for %u ms -> FIX LOST (lines=%lu ovf=%lu)\r\n",
                    (unsigned)GPS_RX_TIMEOUT_MS,
                    (unsigned long)s_line_cnt,
                    (unsigned long)s_ovf_cnt);
            }
            state = 0U;
        }
    }
}

/* ============================ 对外查询 ============================ */

/* 取一份定位快照（内部加锁，可跨任务安全调用）。
   参数：out —— 存放快照的地方
   返回：1 表示当前定位有效，0 表示无效 */
uint8_t Gps_GetFix(GPS_Data_t *out)
{
    uint8_t valid;

    if (out == NULL)
    {
        return 0U;
    }

    Gps_Lock();
    *out = s_fix;
    valid = s_fix.valid;
    Gps_Unlock();

    return valid;
}

/* 累计收到的完整行数。 */
uint32_t Gps_GetLineCount(void)
{
    return s_line_cnt;
}

/* 因超长被丢弃的残行数。 */
uint32_t Gps_GetOverflowCount(void)
{
    return s_ovf_cnt;
}
