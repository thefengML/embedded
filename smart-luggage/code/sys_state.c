/* sys_state.c —— 系统状态中心：事件组的持有者与唯一写入口。设计说明见 sys_state.h。 */

#include "sys_state.h"
#include "task.h"

/* 事件组句柄。只在 Sys_Init() 里赋值一次，之后只读，所以不需要保护。 */
static EventGroupHandle_t s_eg = NULL;

void Sys_Init(void)
{
    s_eg = xEventGroupCreate();
}

/* 置位 / 清位。
   bit —— 要改的位；on —— 非 0 = 置位，0 = 清位

   三种情况直接返回不做事：
     1) Sys_Init() 还没跑（s_eg 为 NULL）
     2) bit 传了 0
     3) 状态没有变化 —— 跳过是为了避免"无变化也去动事件组"，那会让正阻塞在
        Sys_WaitBits() 上的任务白白醒一次。代价只是多读一次位快照，很划算。

   关于"调度器还没启动"：xEventGroupSetBits/ClearBits 在没有任务挂在该事件组上时
   不会触碰任何 TCB，是安全的；而调度器启动前本来也没有任务在等。所以不必像
   ui.c / gps.c 那样加调度器判断。 */
void Sys_UpdateBit(EventBits_t bit, uint8_t on)
{
    EventBits_t cur;

    if ((s_eg == NULL) || (bit == 0U))
    {
        return;
    }

    cur = xEventGroupGetBits(s_eg);

    if (on != 0U)
    {
        if ((cur & bit) == 0U)          /* 原本未置位才动 */
        {
            (void)xEventGroupSetBits(s_eg, bit);
        }
    }
    else
    {
        if ((cur & bit) != 0U)          /* 原本已置位才动 */
        {
            (void)xEventGroupClearBits(s_eg, bit);
        }
    }
}

EventBits_t Sys_GetBits(void)
{
    if (s_eg == NULL)
    {
        return 0U;
    }
    return xEventGroupGetBits(s_eg);
}

EventBits_t Sys_WaitBits(EventBits_t bits, uint32_t timeout_ms)
{
    if (s_eg == NULL)
    {
        return 0U;
    }

    /* xWaitForAllBits = pdFALSE ：任意一位满足即返回（"有告警就处理"的语义）。
     * xClearOnExit    = pdFALSE ：不在返回时清位。位的生命周期归"置位方"管，
     *                            由它显式调 Sys_UpdateBit(..., 0) 清除 ——
     *                            否则调用方一读就把状态抹了，别处再读就看不到了。 */
    return xEventGroupWaitBits(s_eg,
                               bits,
                               pdFALSE,
                               pdFALSE,
                               pdMS_TO_TICKS(timeout_ms));
}
