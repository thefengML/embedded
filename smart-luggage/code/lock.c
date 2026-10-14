/* lock.c —— 指纹锁链路：PA1 EXTI 唤醒 + 中断成帧 + 队列 + 任务解析。设计说明见 lock.h。 */

#include "lock.h"
#include "ZW101.h"      /* Uart_2_SendByte / ZW101_SendCommand */
#include "key.h"        /* Open_Clock / Close_Clock / Clock_State */
#include "ui.h"         /* UI_ShowChinese / UI_Update */
#include "log.h"        /* LOG() */
#include "WIFI.h"       /* Net_RequestStatePublish() —— 锁状态变化时通知 comm_task 上报 */
#include "sys_state.h"  /* SYS_BIT_UNLOCKED —— 锁状态的对外可见副本 */

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include <string.h>

/* ============================================================================
 * ZW101 指令
 *
 * 原来的定义在 module/ZW101_Command.h 里，但那个头文件是【直接定义数组变量】
 * 而不是声明 extern —— 任何第二个 .c 去 include 它都会造成重复定义，
 * 链接期报 multiple definition。所以这里改成 static const 收进本文件。
 * （原头文件保留不动，因为裸机版还在用。）
 * ========================================================================== */
static const uint8_t s_cmd_auto_identify[17] = {
    0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x08,
    0x32, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x02, 0x39
};

/* ============================================================================
 * 帧成帧状态机（只在 USART2 的 ISR 里访问，天然无并发）
 *
 * ZW101 帧格式：EF 01 | 地址(4) | 标识(1) | 长度(2) | 数据(长度-2 字节) | 校验和(2)
 *   总长 = 2 + 4 + 1 + 2 + data_len = 9 + data_len
 *   其中 data_len 含末尾的 2 字节校验和
 *   （这套算法与裸机版一致，未改动）
 * ========================================================================== */
#define FRM_IDLE    0U
#define FRM_GOT_EF  1U
#define FRM_HEADER  2U
#define FRM_DATA    3U

static uint8_t  s_frm_buf[FINGER_FRAME_MAX];
static uint8_t  s_frm_idx;
static uint8_t  s_frm_state;
static uint16_t s_frm_data_len;
static uint16_t s_frm_total;

/* 计数：ISR 写、任务读。32 位在 Cortex-M3 上单次读写原子，够用。 */
static volatile uint32_t s_frm_cnt;    /* 收好的完整帧     */
static volatile uint32_t s_drop_cnt;   /* 超长 / 队列满丢弃 */
static volatile uint32_t s_badsum_cnt; /* 校验和不通过     */

/* ============================================================================
 * 队列 / 信号量
 * ========================================================================== */
static QueueHandle_t     s_finger_q  = NULL;   /* 项类型 uint8_t[FINGER_FRAME_MAX] */
static SemaphoreHandle_t s_touch_sem = NULL;   /* PA1 上升沿（手指放上）*/

void Lock_Init(void)
{
    s_finger_q  = xQueueCreate(FINGER_Q_DEPTH, FINGER_FRAME_MAX);
    s_touch_sem = xSemaphoreCreateBinary();

    s_frm_state = FRM_IDLE;
    s_frm_idx   = 0U;

    /* 建失败会走 vApplicationMallocFailedHook（main.c 里，会打印并停机），
     * 这里不重复处理。 */
}

/* ============================================================================
 * PA1 EXTI —— 替代原来的"主循环轮询 + while 死等"
 *
 * 【为什么用上升沿】触摸感应模块有手指时输出高电平，所以"低 → 高"就是
 * "手指放上"。中断只负责把任务叫醒，判断和动作都在任务里做 ——
 * 中断服务函数越短越好，这是 FreeRTOS 的基本纪律。
 * ========================================================================== */
void Lock_GpioInit(void)
{
    EXTI_InitTypeDef EXTI_InitStruct;
    NVIC_InitTypeDef NVIC_InitStruct;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);

    /* PA1 的输入模式已在 Uart_2_init() 里配好（GPIO_Mode_IN_FLOATING），
     * 这里只做"把 PA1 接到 EXTI1 线上"。 */
    GPIO_EXTILineConfig(GPIO_PortSourceGPIOA, GPIO_PinSource1);

    EXTI_InitStruct.EXTI_Line    = EXTI_Line1;
    EXTI_InitStruct.EXTI_Mode    = EXTI_Mode_Interrupt;
    EXTI_InitStruct.EXTI_Trigger = EXTI_Trigger_Rising;   /* 手指放上 = 低→高 */
    EXTI_InitStruct.EXTI_LineCmd = ENABLE;
    EXTI_Init(&EXTI_InitStruct);

    /* 优先级同样要 >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY(=5)，
     * 因为中断里要调 xSemaphoreGiveFromISR()。 */
    NVIC_InitStruct.NVIC_IRQChannel = EXTI1_IRQn;
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 6;
    NVIC_InitStruct.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStruct.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStruct);
}

void EXTI1_IRQHandler(void)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (EXTI_GetITStatus(EXTI_Line1) != RESET)
    {
        EXTI_ClearITPendingBit(EXTI_Line1);

        if (s_touch_sem != NULL)
        {
            /* 二值信号量的语义正合适：连续多次触摸合并成"有一次触摸待处理"，
             * 不会堆积。信号量已满时 give 失败，忽略返回值即可。 */
            (void)xSemaphoreGiveFromISR(s_touch_sem, &xHigherPriorityTaskWoken);
        }
    }

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/* ============================================================================
 * 中断成帧
 * ========================================================================== */
void Lock_FingerRxByteFromISR(uint8_t c, BaseType_t *pWoken)
{
    if (s_finger_q == NULL)
    {
        return;                 /* 队列还没建（初始化阶段），丢弃 */
    }

    switch (s_frm_state)
    {
        case FRM_IDLE:
            if (c == 0xEFU)
            {
                s_frm_buf[0] = c;
                s_frm_idx    = 1U;
                s_frm_state  = FRM_GOT_EF;
            }
            break;

        case FRM_GOT_EF:
            if (c == 0x01U)
            {
                s_frm_buf[1] = c;
                s_frm_idx    = 2U;
                s_frm_state  = FRM_HEADER;
            }
            else
            {
                /* 第二个字节不是 0x01，说明前面那个 0xEF 是噪声 */
                s_frm_state = FRM_IDLE;
                s_frm_idx   = 0U;
            }
            break;

        case FRM_HEADER:
            /* 收地址(4) + 标识(1) + 长度(2)，收满 9 字节就能算出总长 */
            s_frm_buf[s_frm_idx] = c;
            s_frm_idx++;

            if (s_frm_idx == 9U)
            {
                s_frm_data_len = ((uint16_t)s_frm_buf[7] << 8) | (uint16_t)s_frm_buf[8];
                s_frm_total    = 9U + s_frm_data_len;

                /* 【补的上界校验】原代码没有这一道：如果模块发来一个长度字段
                 * 异常的包（或线路噪声把长度字节改错），总长会远大于缓冲区，
                 * 后面的写入就是越界 —— 这正是核查报告里 S 类隐患的第 1 条。
                 * 现在直接丢弃并计数。
                 * 另外 data_len 至少 3（含确认码 + 2 字节校验和），更小的也判为异常。 */
                if ((s_frm_data_len < 3U) || (s_frm_total > FINGER_FRAME_MAX))
                {
                    s_drop_cnt++;
                    s_frm_state = FRM_IDLE;
                    s_frm_idx   = 0U;
                }
                else
                {
                    s_frm_state = FRM_DATA;
                }
            }
            break;

        case FRM_DATA:
            s_frm_buf[s_frm_idx] = c;
            s_frm_idx++;

            if (s_frm_idx >= s_frm_total)
            {
                /* 一帧收齐 → 整帧投队列。xQueueSendFromISR 是拷贝语义，
                 * 所以投完就可以立刻复位、准备接下一帧。
                 * （在 ISR 里拷 64 字节是微秒级，可以接受。） */
                s_frm_cnt++;
                if (xQueueSendFromISR(s_finger_q, s_frm_buf, pWoken) != pdPASS)
                {
                    s_drop_cnt++;   /* 队列满：任务还没取走 */
                }
                s_frm_state = FRM_IDLE;
                s_frm_idx   = 0U;
            }
            break;

        default:
            s_frm_state = FRM_IDLE;
            s_frm_idx   = 0U;
            break;
    }
}

/* ============================================================================
 * 解析
 * ========================================================================== */

/* 校验和：从标识字节(buf[6])累加到数据区最后一个字节(buf[len-3])，
 * 与末尾两字节(buf[len-2..len-1]，高字节在前)比较。
 * 算法与裸机版一致，未改动。 */
static uint8_t Frame_CheckSum(const uint8_t *buf, uint16_t len)
{
    uint16_t sum = 0U;
    uint16_t i;

    for (i = 6U; i < (uint16_t)(len - 2U); i++)
    {
        sum += (uint16_t)buf[i];
    }

    return ((((uint16_t)buf[len - 2U] << 8) | (uint16_t)buf[len - 1U]) == sum) ? 1U : 0U;
}

/* 锁状态【刚被改变】之后的统一收尾。
   指纹开锁和物理开关关锁都要做同样三件事，所以打包成一个函数：
     ① 刷 OLED 的"开/关"字
     ② 同步 SYS_BIT_UNLOCKED（状态中心的对外副本）
     ③ 请求 comm_task 把新状态上报云端

   打包的意义在于【不会漏】—— 以后再多一处改锁的地方，只要记得调这一个函数就够了。
   漏掉 ② 会让 OLED 与实际不一致；漏掉 ③ 会让云端一直显示旧状态（而且极难发现，
   因为两边各自看都对）。 */
static void Lock_AfterStateChange(void)
{
    uint8_t unlocked = Lock_IsLocked();

    UI_ShowChinese(72, 36, (unlocked != 0U) ? "开" : "关");
    UI_Update();

    Sys_UpdateBit(SYS_BIT_UNLOCKED, unlocked);

    Net_RequestStatePublish();
}

/* 处理一个完整的应答帧。
   字段含义（与原代码一致）：
     buf[6]  包标识，0x07 = 应答包
     buf[9]  确认码   0x00 = OK
     buf[10] 参数     0x01 = 采图成功（中间步骤）
                      0x05 = 比对成功（最终结果）
     buf[11..12] 指纹 ID
     buf[13..14] 匹配得分 */
static void HandleFrame(const uint8_t *buf, uint16_t len)
{
    uint8_t  confirm;
    uint8_t  param;
    uint16_t score;

    if (buf[6] != 0x07U)
    {
        return;                 /* 不是应答包，忽略 */
    }

    /* 【补的长度校验】下面要读到 buf[14]，必须先确认帧有 15 字节。
     * 原代码直接读，短帧会越界。 */
    if (len < 15U)
    {
        return;
    }

    confirm = buf[9];
    param   = buf[10];
    score   = ((uint16_t)buf[13] << 8) | (uint16_t)buf[14];

    if (confirm == 0x00U)
    {
        if (param == 0x05U)
        {
            /* ---- 比对成功 → 开锁 ---- */
            Open_Clock();

            LOG("[lock] MATCH score=%u -> UNLOCK\r\n", (unsigned)score);

            /* 刷屏 + 同步状态位 + 通知 comm_task 上报云端。
             * 【不在这里直接发 AT 指令】—— USART3 和 AT 状态机归 comm_task 独占，
             * 跨任务抢同一个外设是会出事的。 */
            Lock_AfterStateChange();
        }
        else if (param == 0x01U)
        {
            /* 采图成功（中间步骤），按原逻辑忽略 */
        }
        else
        {
            /* 其它中间步骤（合法检测等），忽略 */
        }
    }
    else if (confirm == 0x09U)
    {
        LOG("[lock] not matched (score=%u)\r\n", (unsigned)score);
    }
    else
    {
        LOG("[lock] error code=0x%02X\r\n", (unsigned)confirm);
    }
}

/* ============================================================================
 * 任务
 * ========================================================================== */

/* 指纹锁任务。
   三件事按优先级排在一个循环里：
     ① 收指纹应答帧（主阻塞点，等 100 ms）
     ② 处理 PA1 触摸（EXTI 中断给了信号量）
     ③ PA0 物理开关的边沿检测（修 S7）
   100 ms 的队列超时顺便当周期心跳，让 ③ 能按点跑。 */
void vTaskLock(void *pvParameters)
{
    uint8_t frame[FINGER_FRAME_MAX];
    uint8_t last_sw = 1U;       /* 1 = 开关未拨到低位。初始给 1，
                                 * 这样"开机时开关已经处于低位"也会被当成一次跳变处理
                                 * —— 与裸机版行为一致（裸机版开机也会执行一次关锁）。 */

    (void)pvParameters;

    LOG("[lock] ready\r\n");

    for (;;)
    {
        /* ---- ① 等指纹应答帧（顺便当 100 ms 周期）---- */
        if (xQueueReceive(s_finger_q, frame, pdMS_TO_TICKS(100)) == pdPASS)
        {
            uint16_t len = ((uint16_t)frame[7] << 8) | (uint16_t)frame[8];
            len = 9U + len;

            if ((len <= FINGER_FRAME_MAX) && (Frame_CheckSum(frame, len) != 0U))
            {
                HandleFrame(frame, len);
            }
            else
            {
                s_badsum_cnt++;
            }
        }

        /* ---- ② 手指放上（PA1 EXTI → 信号量）---- */
        if (xSemaphoreTake(s_touch_sem, 0) == pdTRUE)
        {
            /* 防抖：20 ms 后再确认一次电平。触摸感应模块的输出有抖动，
             * 不确认会一次触摸触发多轮识别。 */
            vTaskDelay(pdMS_TO_TICKS(20));

            if (GPIO_ReadInputDataBit(GPIOA, GPIO_Pin_1) == SET)
            {
                LOG("[lock] touch -> AutoIdentify\r\n");
                ZW101_SendCommand(s_cmd_auto_identify, (uint16_t)sizeof(s_cmd_auto_identify));

                /* 【本步的核心收益】这里【没有】 while (PA1 == SET) {} 死等手指移开。
                 * 裸机版在这里死等，期间整个系统停摆 —— GPS 缓冲 0.2 秒就溢出，
                 * 效果就是"按指纹的这几秒，手机远程开锁失灵"。
                 * 现在发完命令就回去收帧，手指按多久都不影响其它任务。 */
            }
        }

        /* ---- ③ PA0 物理开关：边沿检测 ----
         * 修的是核查报告里的 S7：原来主循环每圈都读一次电平，开关停在低位时
         * 【每圈都发一次 MQTT】—— 几秒钟就能把 OneNET 配额刷爆、可能被限流。
         * 现在只在上/下沿跳变时动作一次。 */
        {
            uint8_t sw = (GPIO_ReadInputDataBit(GPIOA, GPIO_Pin_0) == RESET) ? 0U : 1U;

            if (sw != last_sw)
            {
                last_sw = sw;

                if (sw == 0U)
                {
                    /* 拨到低位 = 关锁 */
                    Close_Clock();
                    LOG("[lock] switch LOW -> lock\r\n");
                    Lock_AfterStateChange();
                }
                else
                {
                    LOG("[lock] switch HIGH\r\n");
                }
            }
        }
    }
}

/* ============================================================================
 * 对外查询
 * ========================================================================== */

uint8_t Lock_IsLocked(void)
{
    /* Clock_State 是 key.c 里的权威状态：云端下发和指纹开锁都会更新它，
     * 所以不需要在 lock.c 里再维护第二份（少一份状态就少一类不一致 bug）。 */
    return (Clock_State != 0U) ? 1U : 0U;
}

uint32_t Lock_GetFrameCount(void)
{
    return s_frm_cnt;
}

uint32_t Lock_GetDropCount(void)
{
    return s_drop_cnt;
}

uint32_t Lock_GetBadSumCount(void)
{
    return s_badsum_cnt;
}
