#ifndef _ZW101_H
#define _ZW101_H
#include "stm32f10x.h"                  // Device header
#include <stdio.h>

/* ZW101.h —— ZW101 指纹模块驱动（USART2 @9600，PA2 = TX / PA3 = RX）+ PA1 触摸感应。

   本头文件从"驱动 + 环形缓冲 + PA1 电平查询"收缩成【纯驱动】：

     已删除 isFingerBuf_has_Data() / GET_FingerBuf_Data()
         —— 接收改成"ISR 按协议成帧 → 整帧投队列 → vTaskLock 解析"

     已删除 Cheak_Uart_state() 以及 extern UART_2_State
         —— PA1 改成 EXTI 中断唤醒，不再需要轮询电平；
            而且这两个函数全工程从来没有调用点（死代码）

     发送接口的 buffer 参数加了 const
         —— 命令是常量表（见 lock.c 的 s_cmd_auto_identify），加 const 后
            常量数组可以直传，不必做强制类型转换

   接收、成帧、解析、PA1 处理全部在 lock.h / lock.c 里。 */

void Uart_2_init(void);
void Uart_2_SendByte(uint8_t Data);
void ZW101_SendCommand(const uint8_t *command, uint16_t length);

#endif
