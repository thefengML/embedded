#ifndef _WIFI_H
#define _WIFI_H
#include "stm32f10x.h"

/* WIFI.h —— ESP8266 / OneNET 通信链路（USART3）。

   对外接口从"盲等初始化 + 主循环轮询"改成"状态机 + 任务"：

     已删除 Wifi_init()
         —— 原来是发一串 AT 指令、每条后面 delay_ms，合计盲等 13 秒，而且不看应答、
            失败不重试。裸机版 main() 里那 13 秒会把 GPS 解析、指纹、OLED 全部冻住。

     已删除 Wifi_ProcessCommandLine() / IsWifi_Has_Data() / GET_Wifi_Data()
         —— 环形缓冲和行解析都收进 WIFI.c 内部（static），外部不再需要驱动。
            原来这些函数暴露出去，等于把"什么时候读缓冲"的责任推给调用方。

     新增 vTaskComm()
         —— 通信任务：连接状态机 → 每 5 秒上报位置 → 每 30 秒 AT 探活。
            在 main.c 里创建，实现在 WIFI.c（整条链路属通信模块）。

     凭据已外置到 code/secrets.h（模板：code/secrets.h.example） */

void Uart_3_Init(void);
void WiFi_SendCmd(const char *cmd);

/* 通信任务：连接 → 周期上报 → 探活（断连自动重连）。
 * 优先级 5（最高业务优先级），因为云端心跳超时的代价最直接。 */
void vTaskComm(void *pvParameters);

/* 上报一次位置到 OneNET。
 * 返回 1 = 已发出且未收到明确错误；0 = 模块回了 ERROR/FAIL。 */
uint8_t MQTT_Publish(const char *lon, const char *lat);

/* 上报告警状态到 OneNET。
 * 实际只用得上 active=0（断连恢复时补报）—— 见 WIFI.c 里的说明。
 * 返回 1 = 已发出且未收到明确错误；0 = 模块回了 ERROR/FAIL。
 * ⚠️ 需要 OneNET 后台有布尔型属性 ALARM，否则云端会拒绝；拒绝不影响其它功能。 */
uint8_t MQTT_PublishAlarm(uint8_t active);

/* 供其它任务请求"把当前锁状态上报到云端"（指纹开锁、物理开关会调它）。
 * 用任务通知实现：只传"请求"，真正的串口动作由 comm_task 做 ——
 * USART3 和 AT 状态机归它独占，别的任务不能直接碰。 */
void Net_RequestStatePublish(void);

#endif
