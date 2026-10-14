/* WIFI_Command.c —— AT 指令字面量的集中定义。

   凭据外置：这些字符串原来直接写着明文凭据（WiFi 名/密码、OneNET 设备名/密钥），
   一旦把工程推到 GitHub 就等于公开。现在改用 secrets.h 里的宏做【编译期字符串拼接】
   （C 的相邻字面量自动连接），本文件里再也看不到任何凭据，仓库可以安全公开；
   仍是编译期常量，没有运行时开销、不占额外 RAM；换设备/换 WiFi 只改 secrets.h。 */

#include "WIFI_Command.h"   /* 原为 "WIFI_COMMAND.h"，大小写修正 */
#include "secrets.h"        /* 凭据外置：SSID / 密码 / productKey / token / host 都在这里 */

const char AT[]      = "AT";
const char ATE0[]    = "ATE0";
const char Station[] = "AT+CWMODE=1";

const char CWJAP[]   = "AT+CWJAP=\"" WIFI_SSID "\",\"" WIFI_PASSWORD "\"";
const char CIFSR[]   = "AT+CIFSR";
const char CWLLAP[]  = "AT+CWLAP";

const char MQTTUSERCFG[] =
    "AT+MQTTUSERCFG=0,1,\"" ONENET_DEVICE_NAME "\",\"" ONENET_PRODUCT_ID
    "\",\"" ONENET_AUTH_TOKEN "\",0,0,\"\"";

const char MQTTCONN[] =
    "AT+MQTTCONN=0,\"" ONENET_MQTT_HOST "\"," ONENET_MQTT_PORT ",1";

const char MQTTSUB[]     = "AT+MQTTSUB=0,\"" ONENET_TOPIC_POST_REPLY "\",0";
const char MQTTSUB_CMD[] = "AT+MQTTSUB=0,\"" ONENET_TOPIC_SET "\",0";
