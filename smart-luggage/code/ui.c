/* ui.c —— OLED 共享访问层。设计说明见 ui.h 顶部。 */

#include "ui.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

/* OLED 互斥量。拿到它才能写屏幕，保证同一时刻只有一个任务在操作 I2C。
   建不出来（堆不足）会走 vApplicationMallocFailedHook，这里不重复处理。 */
static SemaphoreHandle_t s_oled_mutex = NULL;

/* 创建 OLED 互斥量。必须在建任务之前、启动调度器之前调用。 */
void UI_Init(void)
{
    s_oled_mutex = xSemaphoreCreateMutex();
}

/* 取锁。以下两种情况直接放行，不加锁：
     1) 互斥量还没建（UI_Init() 还没调用）
     2) 调度器还没启动 —— 此时本来就没有任务并发；
        而且调度器启动前调用 xSemaphoreTake 是 FreeRTOS 不支持的用法
        （内部会去操作尚未建立的当前任务控制块）。
   加上这两道判断后，main() 的初始化段也能放心用同一套 UI_ 接口。 */
static void UI_Lock(void)
{
    if ((s_oled_mutex != NULL) &&
        (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED))
    {
        (void)xSemaphoreTake(s_oled_mutex, portMAX_DELAY);
    }
}

/* 放锁，与 UI_Lock() 配对使用。 */
static void UI_Unlock(void)
{
    if ((s_oled_mutex != NULL) &&
        (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED))
    {
        (void)xSemaphoreGive(s_oled_mutex);
    }
}

/* ============================ 带锁的 OLED 接口 ============================
   每个函数都是"取锁 → 调用 OLED_xxx → 放锁"三步。
   参数含义与 OLED.h 里的同名函数完全一致，这里只补充没说清的部分。
   ====================================================================== */

/* 清屏（只清内存缓冲，需再调 UI_Update() 才会显示）。 */
void UI_Clear(void)
{
    UI_Lock();
    OLED_Clear();
    UI_Unlock();
}

/* 把内存缓冲刷到屏幕。软件 I2C，一次刷 1024 字节，比较慢（几十毫秒）。 */
void UI_Update(void)
{
    UI_Lock();
    OLED_Update();
    UI_Unlock();
}

/* 在指定位置显示一个字符。
   X, Y     —— 左上角坐标（像素）
   Char     —— 要显示的字符
   FontSize —— 字号：OLED_8X16 或 OLED_6X8 */
void UI_ShowChar(int16_t X, int16_t Y, char Char, uint8_t FontSize)
{
    UI_Lock();
    OLED_ShowChar(X, Y, Char, FontSize);
    UI_Unlock();
}

/* 在指定位置显示一个字符串。
   String —— 字符串首地址，需以 '\0' 结尾
   其余参数同 UI_ShowChar */
void UI_ShowString(int16_t X, int16_t Y, char *String, uint8_t FontSize)
{
    UI_Lock();
    OLED_ShowString(X, Y, String, FontSize);
    UI_Unlock();
}

/* 在指定位置显示一个数字。
   Number —— 要显示的数值
   Length —— 显示几位，不足的前面补 0
   其余参数同 UI_ShowChar */
void UI_ShowNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize)
{
    UI_Lock();
    OLED_ShowNum(X, Y, Number, Length, FontSize);
    UI_Unlock();
}

/* 在指定位置显示一个汉字。
   Chinese —— 汉字字符串（点阵来自 OLED_Data.c 的字库） */
void UI_ShowChinese(int16_t X, int16_t Y, char *Chinese)
{
    UI_Lock();
    OLED_ShowChinese(X, Y, Chinese);
    UI_Unlock();
}
