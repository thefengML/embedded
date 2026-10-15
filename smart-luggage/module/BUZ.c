/* BUZ.c —— 蜂鸣器驱动（TIM1_CH1N → PB13，PWM 方波发声）。 */

#include "stm32f10x.h"                  // Device header
#include "BUZ.h"

/* 初始化：PB13 复用推挽输出 + TIM1_CH1N PWM。
   按寄存器值算：72 MHz ÷ 18 = 4 MHz 计数，周期 1000 → 4 kHz 方波，占空比 50%。
   初始化完 TIM1 就开始计数，是否发声由 MOE（主输出使能）决定 —— 见 BUZ_Stop 的说明。 */
void BUZ_init(){
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB,ENABLE);
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_TIM1,ENABLE);
	
	GPIO_InitTypeDef GPIO_Struct;
	GPIO_Struct.GPIO_Mode = GPIO_Mode_AF_PP;
	GPIO_Struct.GPIO_Pin = GPIO_Pin_13;
	GPIO_Struct.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOB,&GPIO_Struct);
	
	TIM_TimeBaseInitTypeDef TIM_Struct;
	
	TIM_Struct.TIM_ClockDivision = TIM_CKD_DIV1;
	TIM_Struct.TIM_CounterMode = TIM_CounterMode_Up;
	TIM_Struct.TIM_Period = 1000-1;
	TIM_Struct.TIM_Prescaler = 18-1;
	TIM_Struct.TIM_RepetitionCounter = 0;
	TIM_TimeBaseInit(TIM1,&TIM_Struct);
	
	TIM_OCInitTypeDef OC_Struct;
	TIM_OCStructInit(&OC_Struct);
	OC_Struct.TIM_OCMode = TIM_OCMode_PWM1;
	OC_Struct.TIM_OCPolarity = TIM_OCPolarity_High;
	OC_Struct.TIM_OutputState = TIM_OutputState_Enable;
	OC_Struct.TIM_OutputNState = TIM_OutputNState_Enable;
	OC_Struct.TIM_Pulse = 500-1;
	TIM_OC1Init(TIM1,&OC_Struct);
	
	TIM_CtrlPWMOutputs(TIM1,ENABLE);
	TIM_Cmd(TIM1,ENABLE);
	
}

/* 静音。
    */
void BUZ_Stop(){
	TIM_CtrlPWMOutputs(TIM1,DISABLE);
	TIM_Cmd(TIM1,DISABLE);

}

/* 发声。顺序与 BUZ_Stop 相反：先跑计数、再开输出级。 */
void BUZ_Open(){
	TIM_Cmd(TIM1,ENABLE);
	TIM_CtrlPWMOutputs(TIM1,ENABLE);
}
