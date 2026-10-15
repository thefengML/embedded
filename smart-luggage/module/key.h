#ifndef _KEY_H
#define _KEY_H

/* KEY.h —— 锁（继电器 PB12）+ PA0 物理开关。

   Clock_State 是锁状态的【权威来源】：云端下发（WIFI.c 的 ParseMQTTCommand）
   和指纹开锁（lock.c 的 HandleFrame）都会更新它；上报锁状态时直接读它，
   不在别处维护第二份 —— 少一份状态就少一类不一致 bug。 */

extern uint8_t Clock_State;     /* 0 = 关锁，1 = 开锁 */

void Key_init(void);
void Close_Clock(void);
void Open_Clock(void);

#endif
