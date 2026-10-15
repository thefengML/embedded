/* ZW101.c —— ZW101 指纹模块驱动：USART2 的寄存器配置与发送。

   接收路径与 PA1 处理整体在 lock.c。本文件不再做任何缓冲或解析。
    */

#include "ZW101.h"
#include "lock.h"       /* Lock_FingerRxByteFromISR —— USART2 的 ISR 用它成帧 */

#include "FreeRTOS.h"
#include "task.h"

/* USART2 初始化：PA2 = TX（复用推挽）→ ZW101、PA3 = RX（浮空输入）← ZW101、
   PA1 = 触摸感应输入（浮空）。9600 / 8N1 / 无流控，打开 RXNE 中断。

   ⚠️ PA1 的 EXTI 配置不在这里 —— 在 lock.c 的 Lock_GpioInit() 里，
      而且必须在本次调用【之后】执行：引脚得先处于输入态，再把 EXTI 线接上去。 */
void Uart_2_init(){
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA,ENABLE);
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2,ENABLE);
	
	GPIO_InitTypeDef GPIO_Struct;
	GPIO_Struct.GPIO_Mode = GPIO_Mode_AF_PP;
	GPIO_Struct.GPIO_Pin = GPIO_Pin_2;          /* PA2 = USART2_TX → ZW101 */
	GPIO_Struct.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOA,&GPIO_Struct);
	
	GPIO_Struct.GPIO_Mode = GPIO_Mode_IN_FLOATING;
	GPIO_Struct.GPIO_Pin = GPIO_Pin_3;          /* PA3 = USART2_RX ← ZW101 */
	GPIO_Struct.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOA,&GPIO_Struct);
	
	/* PA1 = 指纹触摸感应输入（低电平空闲、手指导通时变高）。 */
	GPIO_Struct.GPIO_Mode = GPIO_Mode_IN_FLOATING;
	GPIO_Struct.GPIO_Pin = GPIO_Pin_1;
	GPIO_Struct.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOA,&GPIO_Struct);
	
	USART_InitTypeDef Usart_2_Struct;
	Usart_2_Struct.USART_BaudRate = 9600;
	Usart_2_Struct.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
	Usart_2_Struct.USART_Mode = USART_Mode_Rx |USART_Mode_Tx;
	Usart_2_Struct.USART_Parity = USART_Parity_No;
	Usart_2_Struct.USART_StopBits = USART_StopBits_1;
	Usart_2_Struct.USART_WordLength = USART_WordLength_8b;
	USART_Init(USART2,&Usart_2_Struct);
	
	USART_ITConfig(USART2,USART_IT_RXNE,ENABLE);
	/* 抢占优先级 2 -> 6（必须 >= FreeRTOS 的
	 * configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5，
	 * 否则中断里不能调用 xxxFromISR()）。Group_4 下子优先级填 0。
	 * 这个中断里会调 xQueueSendFromISR()（经 Lock_FingerRxByteFromISR），
	 * 所以优先级设置是【必须】而不是"建议"。 */
	NVIC_InitTypeDef USART_2_NVIC;
	USART_2_NVIC.NVIC_IRQChannel = USART2_IRQn;
	USART_2_NVIC.NVIC_IRQChannelCmd = ENABLE;
	USART_2_NVIC.NVIC_IRQChannelPreemptionPriority = 6;
	USART_2_NVIC.NVIC_IRQChannelSubPriority = 0;
	NVIC_Init(&USART_2_NVIC);
	
	USART_Cmd(USART2,ENABLE);	
}

/* 发送一个字节（阻塞式，等 TXE 才返回）。 */
void Uart_2_SendByte(uint8_t Data){
	USART_SendData(USART2,Data);
	while(USART_GetFlagStatus(USART2,USART_FLAG_TXE) == RESET); //发送一个字节
}

/* USART2 接收中断 —— 只把字节交给 lock 模块成帧，其余什么都不做。

   原来这里是塞进 finger_LoopBuff[200] 环形缓冲，由主循环用
   isFingerBuf_has_Data()/GET_FingerBuf_Data() 轮询取走、逐字节喂给帧解析。三个问题：
     1) finger_Head/finger_Tail 在 ISR 与主循环之间无保护共享
     2) 主循环被 while (PA1 == SET) 死等占住时，指纹数据直接积压/丢弃
     3) 200 字节 @9600 只够 0.2 秒

   现在：ISR 里按协议成帧 → 整帧投 finger_q → vTaskLock 解析（成帧逻辑在 lock.c）。
   优先级已在 Uart_2_init() 里设为 6 —— 只有 >= 5 才允许在中断里调 FromISR API。 */
void USART2_IRQHandler(void)
{
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;

	if(USART_GetITStatus(USART2,USART_IT_RXNE) == SET){
		uint8_t Data = (uint8_t)USART_ReceiveData(USART2);
		Lock_FingerRxByteFromISR(Data, &xHigherPriorityTaskWoken);
	}

	/* 溢出错误（ORE）：置位后 RXNE 不再被置起、链路"假死" ——
	 * 现象是指纹模块突然再也不回任何数据，而 MCU 一切正常。
	 * 清除办法是"先读 SR 再读 DR"，恰好由下面两句完成。 */
	if(USART_GetFlagStatus(USART2,USART_FLAG_ORE) != RESET){
		(void)USART_ReceiveData(USART2);
	}

	portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/* 发送一条完整的指令帧。
   command —— 指令字节表（含预先生好的校验和）；length —— 帧总字节数
   注：原代码里那句 `uint16_t cheaksum = 0;` 声明后从未使用，已删除 ——
       命令帧的校验和是预先算好写在指令表里的（见 lock.c 的 s_cmd_auto_identify）。 */
void ZW101_SendCommand(const uint8_t *command,uint16_t length){
	uint16_t i;
	for(i = 0;i<length;i++){
		Uart_2_SendByte(command[i]);
	}
}
