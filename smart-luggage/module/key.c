/* key.c —— 锁（继电器 PB12）+ PA0 物理开关的初始化与开关动作。
*/

#include "stm32f10x.h"                  // Device header
#include "key.h"   /* 原为 "KEY.h"，大小写与实际文件名不符，Linux 上会编译失败 */

uint8_t Clock_State;


/* 初始化：PB12 推挽输出（继电器，模拟锁）+ PA0 上拉输入（物理开关）。 */
void Key_init(){
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB,ENABLE);
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA,ENABLE);
	
	GPIO_InitTypeDef GPIO_Struct;
	GPIO_Struct.GPIO_Mode = GPIO_Mode_Out_PP;
	GPIO_Struct.GPIO_Pin = GPIO_Pin_12;         /* PB12 = 继电器（模拟锁）*/
	GPIO_Struct.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOB,&GPIO_Struct);
	GPIO_Struct.GPIO_Mode = GPIO_Mode_IPU;
	GPIO_Struct.GPIO_Pin = GPIO_Pin_0;          /* PA0 = 物理开关（上拉输入）*/
	GPIO_Struct.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOA,&GPIO_Struct);
}

/* 关锁：置状态位 + 拉低 PB12。 */
void Close_Clock(){
	Clock_State = 0;
	GPIO_ResetBits(GPIOB,GPIO_Pin_12);

}

/* 开锁：置状态位 + 拉高 PB12。 */
void Open_Clock(){
	Clock_State = 1;
	GPIO_SetBits(GPIOB,GPIO_Pin_12);
}
