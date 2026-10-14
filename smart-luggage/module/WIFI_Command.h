#ifndef _WIFI_COMMAND_H
#define _WIFI_COMMAND_H

/* WIFI_Command.h —— 集中声明各条 AT 指令字符串（定义在 WIFI_Command.c）。

   这些串里没有任何凭据：WiFi 名/密码、OneNET 设备名/密钥都由 secrets.h 的宏在
   编译期拼进来，本头文件可以随工程公开。 */

extern const char AT[];
extern const char ATE0[];
extern const char Station[];

extern const char CWJAP[];
extern const char CIFSR[];
extern const char CWLLAP[];
extern const char MQTTUSERCFG[];
extern const char MQTTCONN[];
extern const char MQTTSUB[];
extern const char MQTTSUB_CMD[];

#endif
