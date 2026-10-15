/* Uart.c —— USART1（GPS 模块 + 调试输出）的寄存器配置、发送，以及 printf 重定向。

   接收路径整体在 gps.c，本文件不再做任何缓冲或解析。

   相对裸机版删掉的东西（都已失效或无人调用）：
     uint8_t Usart_RxD, Usart_Status, head, tail;
     char    Uart_String[200];
     GPS_Data_t GPS_Data;
     is_has_data() / Read_Uart_data() / GPS_ReadLine() / convert_dm_to_deg() /
     ParseGGA() / GPS_Parse()*/

#include "Uart.h"                  // Device header
#include "gps.h"                   /* Gps_RxByteFromISR —— USART1 的 ISR 用它拼行 */
#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>

/* USART1 初始化：PA9 = TX（复用推挽）、PA10 = RX（下拉输入），9600 / 8N1 / 无流控，
   并打开 RXNE 接收中断。

   注意 PA9 那根线实际是【调试输出口】—— GPS 只用 PA10 往 MCU 发，从不用 PA9，
   所以日志和 GPS 不会互相干扰（详见文件末尾 printf 重定向的说明）。 */
void Uart_Iint(){
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1,ENABLE);
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA,ENABLE);
	GPIO_InitTypeDef GPIO_Struct;
	GPIO_Struct.GPIO_Mode = GPIO_Mode_AF_PP;
	GPIO_Struct.GPIO_Pin = GPIO_Pin_9;
	GPIO_Struct.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOA,&GPIO_Struct);
	
	GPIO_Struct.GPIO_Mode = GPIO_Mode_IPD;
	GPIO_Struct.GPIO_Pin = GPIO_Pin_10;
	GPIO_Struct.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOA,&GPIO_Struct);
	
	USART_InitTypeDef Usart_Struct;
	Usart_Struct.USART_BaudRate = 9600;
	Usart_Struct.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
	Usart_Struct.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
	Usart_Struct.USART_Parity = USART_Parity_No;
	Usart_Struct.USART_StopBits = USART_StopBits_1;
	Usart_Struct.USART_WordLength = USART_WordLength_8b;
	USART_Init(USART1,&Usart_Struct);
	USART_ITConfig(USART1,USART_IT_RXNE,ENABLE);
	
	/* 原来这里有一句 NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2)，已删除：
	 * 它会覆盖 main() 里设置的 Group_4，破坏 FreeRTOS 的优先级约定。
	 * 分组只允许在 main() 里统一设置一次。
	 *
	 * 抢占优先级由 2 改为 6：FreeRTOS 规定"会调用 xxxFromISR() 的中断，
	 * 优先级数值必须 >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY(=5)"。
	 * 数值 2 比 5 更紧急，这种中断里调用队列/信号量 API 属于非法操作。
	 * Group_4 下子优先级字段不生效，填 0。
	 *
	 * 这个中断里会调用 xQueueSendFromISR()（经 Gps_RxByteFromISR），
	 * 所以优先级设置是【必须】而不是"建议"—— 填错的话 configASSERT 会在
	 * 第一次收数据的瞬间直接断言停机。 */
	NVIC_InitTypeDef Nvic_Srtuct;
	Nvic_Srtuct.NVIC_IRQChannel = USART1_IRQn;
	Nvic_Srtuct.NVIC_IRQChannelCmd = ENABLE;
	Nvic_Srtuct.NVIC_IRQChannelPreemptionPriority = 6;
	Nvic_Srtuct.NVIC_IRQChannelSubPriority = 0;
	NVIC_Init(&Nvic_Srtuct);
	
	
	
	USART_Cmd(USART1, ENABLE);
	
}


/* 发送一个字节。发送后等 TXE（发送寄存器空）才返回，所以是【阻塞式】的，
   一个字节在 9600 下约 1.04 ms。 */
void  Uart_Send_bit(uint8_t bit){
	
	USART_SendData(USART1,bit);
	
	while(USART_GetFlagStatus(USART1,USART_FLAG_TXE) == RESET);
}


/* 发送一个以 '\0' 结尾的字符串。 */
void Uart_Send_Str(char *Str){
	uint8_t i;
	for(i = 0;Str[i] != '\0';i++){
		Uart_Send_bit(Str[i]);
	}
}

/* 算 10 的 X 次方，给 Uart_Send_Num 拆位用。 */
uint32_t count_pow(uint8_t X){
	uint32_t Result = 1;
	while(X!=0){
		Result = Result*10;
		X--;
	}
	return Result;
}

/* 按固定位数发送一个整数（不足位补 0）。
   number —— 要发的数；length —— 要发的位数 */
void Uart_Send_Num(uint32_t number,uint8_t length){
	
	for(;length !=0; length--){
		Uart_Send_bit(number/count_pow(length-1)%10);
	}
 
}

/* USART1 接收中断 —— 只把字节交给 GPS 模块拼行，其余什么都不做。

   原来这里是往 Uart_String[200] 环形缓冲里塞字节，再由主循环用
   is_has_data()/Read_Uart_data() 轮询取走。三个问题：
     1) head/tail 在 ISR 与主循环之间无保护共享
     2) 200 字节 @9600 只够 0.2 秒，主循环被拖住就静默溢出
     3) 主循环被 while(PA1==SET) 之类的死等占住时，GPS 数据直接积压/丢弃

   现在：拼行 → 整行投 gps_q → gps_task 里解析（成帧逻辑在 gps.c）。
   中断里只做"存字节 / 投一次队列"，时间恒定且极短。

   优先级已在 Uart_Iint() 里设为 6 —— 只有 >= 5 才允许在中断里调用带 FromISR
   后缀的 FreeRTOS API，否则会触发断言。 */
void USART1_IRQHandler(void)
{
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;

	if (USART_GetITStatus(USART1, USART_IT_RXNE) != RESET)
	{
		uint8_t c = (uint8_t)USART_ReceiveData(USART1);
		Gps_RxByteFromISR(c, &xHigherPriorityTaskWoken);
	}

	/* 溢出错误（ORE）：9600 下长时间没被取走就可能出现。置位后 RXNE 不再
	 * 被置起，链路会"假死"—— 现象是 GPS 从此再无数据、而 CPU 一切正常。
	 * 清除办法是"先读 SR 再读 DR"，这两步由下面两句恰好完成。 */
	if (USART_GetFlagStatus(USART1, USART_FLAG_ORE) != RESET)
	{
		(void)USART_ReceiveData(USART1);
	}

	/* 上面若唤醒了优先级更高的任务（比如刚从队列上挂起的 gps_task），
	 * 在这里请求一次上下文切换，让它能立刻运行，而不是等到下一个节拍。
	 * 这就是"中断直接激活任务"的标准写法。 */
	portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}


/* ============================ printf 重定向 ============================
 * 本工程用的是【标准 C 库】（useUlib=0），不是 MicroLIB。
 * 两者差别：MicroLIB 下只写一个 fputc 就够了；标准 C 库下还必须：
 *   1) 声明 __use_no_semihosting，阻止链接器拉进半主机（semihosting）代码
 *   2) 提供 __FILE 结构、__stdout、_sys_exit、_ttywrch 这几个桩
 * 缺任何一项，链接期会报 undefined symbol，或者烧进去后卡死在半主机调用。
 *
 * 输出通道：USART1 的 TX（PA9）@9600 —— 这是【专用的调试输出口】。
 * GPS（ATGM336H）只用 PA10 往 MCU 发 NMEA，PA9 从来没有真接过 GPS 的 RX，
 * 所以这条线不会被 GPS 干扰，也不必担心日志"污染"GPS。
 * 唯一的代价是波特率被 GPS 锁在 9600：单字符约 1.04 ms，一行 13 字符 ≈ 13 ms。
 * 任务里直接 printf 会把这 13 ms 忙等掉 —— 所以业务任务一律走 LOG() 投队列，
 * 由最低优先级的 log_task 串行输出（见 log.h）。
 * ==================================================================== */
__asm(".global __use_no_semihosting\n\t");

/* ARM Compiler 6 的 stdio.h 在【严格标准模式】(__STRICT_ANSI__，即 -std=c99)
 * 下会自己定义 struct __FILE —— 一个 84 字节、与库内部布局完全一致的占位结构。
 * 此时不能再定义一遍，否则报 `redefinition of '__FILE'`。
 * 而在 gnu 扩展模式下手头文件里的 __FILE 是不完整的，才需要像 Keil 传统配方
 * 那样自己补一个。所以这里加条件保护，两种模式都能编过。 */
#if !defined(__STRICT_ANSI__)
struct __FILE
{
	int handle;
};
#endif

FILE __stdout;

void _sys_exit(int x)
{
	(void)x;
	while (1) { }
}

void _ttywrch(int ch)
{
	(void)ch;
}

/* 标准 C 库启动时会为 argv 调用这个函数去问"命令行字符串"。
 * 裸机里没有命令行，返回 NULL 即可 —— 否则链接期报
 *   L6915E: __use_no_semihosting was requested, but _sys_command_string was referenced */
char *_sys_command_string(char *cmd, int len)
{
	(void)cmd;
	(void)len;
	return NULL;
}

/* printf 的底层出口：把字符从 USART1 发出去。
   遇到 '\n' 先补一个 '\r'，这样串口助手能正确换行。 */
int fputc(int ch, FILE *f)
{
	(void)f;
	if (ch == '\n')
	{
		Uart_Send_bit('\r');   /* 串口助手换行需要 \r\n */
	}
	Uart_Send_bit((uint8_t)ch);
	return ch;
}
