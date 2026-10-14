#ifndef _UI_H
#define _UI_H

#include <stdint.h>

#include "OLED.h"     /* 为了拿到 OLED_8X16 / OLED_6X8 这些字号宏 */

/* ui.h —— OLED 共享访问层。

   对 OLED 的读写一律走这里，内部用互斥量串行化。

   为什么需要这一层：
     写 OLED 的有两个任务（hb 刷坐标、comm 收到云端命令时写"开/关"）。
     如果两边都直接调 OLED_xxx()，一旦在 OLED_Update() 的 I2C 传输中途被抢占，
     两次刷新的数据就会交错，屏幕上出现花屏或残影。

   为什么用互斥量而不是二值信号量：
     互斥量带优先级继承。低优先级任务正拿着锁时，高优先级任务来抢 ——
     内核会把低优先级任务临时提到同级先跑完放手，避免优先级反转。

   使用约定：
     · 任务里写 OLED 一律用 UI_xxx()，不要再直接调 OLED_xxx()，否则互斥形同虚设
     · UI_Init() 在 main() 里、启动调度器之前调用
     · 本层只保证"单次调用"的原子性。像 ShowString + Update 这样的多步序列，
       中间仍可能被抢占 —— 顶多屏幕闪一下，不会花屏。
*/

/* 创建 OLED 互斥量。必须在建任务之前、启动调度器之前调用。 */
void UI_Init(void);

/* 以下接口与 OLED.h 一一对应，区别是每个都带锁，可以跨任务安全调用。
   参数含义与 OLED.h 中的同名函数完全一致。 */
void UI_Clear(void);
void UI_Update(void);
void UI_ShowChar(int16_t X, int16_t Y, char Char, uint8_t FontSize);
void UI_ShowString(int16_t X, int16_t Y, char *String, uint8_t FontSize);
void UI_ShowNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize);
void UI_ShowChinese(int16_t X, int16_t Y, char *Chinese);

#endif
