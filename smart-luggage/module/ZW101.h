#ifndef _ZW101_H
#define _ZW101_H
#include "stm32f10x.h"                  // Device header
#include <stdio.h>

/* ZW101.h —— ZW101 指纹模块驱动（USART2 @9600，PA2 = TX / PA3 = RX）+ PA1 触摸感应。

     发送接口的 buffer 参数加了 const
         —— 命令是常量表（见 lock.c 的 s_cmd_auto_identify），加 const 后
            常量数组可以直传，不必做强制类型转换

   接收、成帧、解析、PA1 处理全部在 lock.h / lock.c 里。 */

void Uart_2_init(void);
void Uart_2_SendByte(uint8_t Data);
void ZW101_SendCommand(const uint8_t *command, uint16_t length);

#endif
