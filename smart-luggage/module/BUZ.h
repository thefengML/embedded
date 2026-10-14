#ifndef _BUZ_H
#define _BUZ_H

/* BUZ.h —— 蜂鸣器（TIM1_CH1N → PB13，PWM）。

   BUZ_Stop() / BUZ_Open() 内部带 MOE（主输出使能）控制，用来修掉
   "停计数器却把输出电平冻结在高电平、蜂鸣器一直响"的隐患（S10），对外接口没变。

   ⚠️ 这两个函数【不要】在别处直接调用 —— 蜂鸣器是单份物理资源，现在有
      "云端请求"和"本地告警"两个来源要用它，由 code/alarm.c 统一仲裁。
      要让它响请用 Alarm_SetRemoteBuzzer()，本地告警用 Alarm_SetLinkAlarm()。 */

void BUZ_init(void);

void BUZ_Stop(void);
void BUZ_Open(void);

#endif
