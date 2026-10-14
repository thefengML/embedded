/* alarm.c —— 告警链路：蜂鸣器仲裁 + 断连告警的本地执行。设计说明见 alarm.h。 */

#include "alarm.h"
#include "sys_state.h"
#include "BUZ.h"
#include "log.h"

/* 蜂鸣器当前的物理状态。用来做"变化才写寄存器"的过滤 ——
 * 鸣叫节奏每 100 ms 走一步，但实际只在开↔关的边界上才去动 TIM1。
 * （不改也可以，只是每秒十来次无谓的寄存器写没有意义。） */
static uint8_t s_buz_on = 0U;

/* 累计进入告警的次数（含云端请求）。每次从"不响"变为"要响"时 +1。 */
static uint32_t s_alarm_cnt = 0U;

/* ============================================================================
 * 蜂鸣器底层
 * ========================================================================== */

static void Buzzer_Set(uint8_t on)
{
    if (on == s_buz_on)
    {
        return;
    }
    s_buz_on = on;

    if (on != 0U)
    {
        BUZ_Open();
    }
    else
    {
        BUZ_Stop();
    }
}

/* ============================================================================
 * 对外入口
 * ========================================================================== */

void Alarm_SetRemoteBuzzer(uint8_t on)
{
    /* 只改位，不碰寄存器 —— 开关动作统一由 alarm_task 裁决后执行。
     * 这样"云端让它响、同时本地在告警"时不会互相覆盖。 */
    Sys_UpdateBit(SYS_BIT_REMOTE_BUZ, on);

    LOG("[alarm] remote buzzer %s\r\n", (on != 0U) ? "ON" : "OFF");
}

void Alarm_SetLinkAlarm(uint8_t active)
{
#if (ALARM_ENABLE_LINK_ALARM != 0)
    Sys_UpdateBit(SYS_BIT_ALARM, active);
    LOG("[alarm] link alarm %s\r\n", (active != 0U) ? "ON" : "OFF");
#else
    /* 屏蔽中：连状态位都不置。
     * 这样 OLED 的 A 位、日志里的 al= 计数、以及 comm_task 里
     * "重连后补报一条告警解除"都不会发生 —— 对外表现就是这条功能不存在。
     * 保留参数和函数体，是为了不改调用点签名：启用只需把 alarm.h 里
     * ALARM_ENABLE_LINK_ALARM 改成 1。 */
    (void)active;
#endif
}

uint8_t Alarm_IsActive(void)
{
    return ((Sys_GetBits() & SYS_BITS_BUZZER) != 0U) ? 1U : 0U;
}

uint32_t Alarm_GetCount(void)
{
    return s_alarm_cnt;
}

/* ============================================================================
 * 鸣叫驱动
 * ========================================================================== */

/* 鸣叫循环：只要还有任一告警来源成立，就按节奏驱动蜂鸣器。
   本函数是"有事才进"的，退出条件是两个位都清干净 —— 所以正常运行时，alarm_task
   绝大部分时间阻塞在下面的长等待里，一次都不进这里，空闲开销为零。 */
static void Alarm_SirenLoop(void)
{
    uint8_t step = 0U;

    for (;;)
    {
        EventBits_t bits = Sys_GetBits();

        if ((bits & SYS_BIT_REMOTE_BUZ) != 0U)
        {
            /* 云端请求：持续响。用户在用手机找箱子，一会儿一停会显得没在工作。
             * 节奏游标清零，这样等它撤销后若还在本地告警，节奏从头开始、听得出来。 */
            Buzzer_Set(1U);
            step = 0U;
        }
        else if ((bits & SYS_BIT_ALARM) != 0U)
        {
            /* 本地告警：间断鸣叫（响 200 ms / 停 1800 ms）。
             * 10% 占空比既省电，又比长鸣更容易被辨认成"报警"。 */
            Buzzer_Set((step < ALARM_ON_STEPS) ? 1U : 0U);

            step++;
            if (step >= ALARM_CYCLE_STEPS)
            {
                step = 0U;
            }
        }
        else
        {
            /* 两个来源都撤了 → 静音、回长等待 */
            Buzzer_Set(0U);
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(ALARM_STEP_MS));
    }
}

/* 告警任务。
   结构是"长等待 → 有事才进鸣叫循环"两段式：

       for (;;) {
           Sys_WaitBits(SYS_BITS_BUZZER, 1000ms);   ← 无事时真正挂起
           if (没有位)  { 静音; continue; }          ← 超时兜底，不耗 CPU
           Alarm_SirenLoop();                        ← 有位才按节奏驱动
       }

   为什么不写成"每 100 ms 醒一次看看该不该响"：
   那样即使一整天没有任何告警，CPU 也会被醒来 86 万次。本设备是锂电池供电，这种白唤醒
   是要算进续航的。事件组让"无事挂起、有事即醒"变成一行代码 —— 这也正是它相对
   "全局变量 + 周期轮询"唯一不可替代的价值（详见 sys_state.h 的说明）。 */
void vTaskAlarm(void *pvParameters)
{
    (void)pvParameters;

    LOG("[alarm] ready\r\n");

    for (;;)
    {
        EventBits_t bits = Sys_WaitBits(SYS_BITS_BUZZER, ALARM_IDLE_WAIT_MS);

        if ((bits & SYS_BITS_BUZZER) == 0U)
        {
            /* 1 秒超时且无任何告警：确认静音，继续长等待。
             * 不在这里做任何别的事 —— 本任务的职责就只有告警。 */
            Buzzer_Set(0U);
            continue;
        }

        /* 有告警：计数并进入鸣叫循环，直到告警全部解除 */
        s_alarm_cnt++;
        LOG("[alarm] active (remote=%u local=%u) #%lu\r\n",
            (unsigned)((bits & SYS_BIT_REMOTE_BUZ) != 0U),
            (unsigned)((bits & SYS_BIT_ALARM) != 0U),
            (unsigned long)s_alarm_cnt);

        Alarm_SirenLoop();

        LOG("[alarm] cleared (total=%lu)\r\n", (unsigned long)s_alarm_cnt);
    }
}
