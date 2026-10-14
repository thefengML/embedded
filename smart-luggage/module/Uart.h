#ifndef  _Uart_H
#define  _Uart_H
#include "stm32f10x.h" 
                 // Device header
#include <stdio.h>

/* uart.h —— USART1（接 GPS 模块，同时兼作调试输出）的配置与发送接口。

   接收与解析不在这里，请看 gps.h（Gps_GetFix / Gps_RxByteFromISR / vTaskGps）。

   相对裸机版删掉的东西（都已失效或无人调用）：
     GPS_Data_t / extern GPS_Data         → 已移到 gps.h（GPS 链路归 gps 模块管）
     is_has_data() / Read_Uart_data()     → 环形缓冲读取，已删除
     GPS_Parse()                          → 轮询式解析，已删除
     GET_Usart_Status() / GET_Usart_RxD() → 全工程无调用点，已删除
     extern char Uart_Recive_data[]       → 只有声明没有定义，悬空，已删除
*/

/* 关于 Uart_Iint 的写法：C 里空参数列表 `()` 是 K&R 风格的"参数未指定"，不是
   "无参数"，clang 会为每个包含本头文件的编译单元报一条 -Wstrict-prototypes
   （本工程是 3 条）。加上 `void` 就明确了。 */
void Uart_Iint(void);
void  Uart_Send_bit(uint8_t bit);
void Uart_Send_Str(char *Str);
void Uart_Send_Num(uint32_t number,uint8_t length);


#endif
