/* WIFI.c —— ESP8266 / OneNET 通信链路（USART3 @115200）。

   从"盲等初始化 + 主循环轮询"改成"AT 状态机 + 通信任务 + 周期上报 + 断连感知"。

   裸机版的做法与问题：
       Wifi_init() 在 main() 里依次发 6 条 AT 指令，每条后面 delay_ms(3000)，
       合计盲等 13 秒；发完就假定成功，不看应答、失败不重试。
       · 那 13 秒里 GPS 解析、指纹、OLED 全部停摆
       · 连不上也不知道 —— 而任务书第 4 条（断连报警）恰恰依赖"知道连接状态"

   现在：
       · 连接拆成 7 个步骤，每步"发指令 → 等应答 → 超时重试"，全程在 comm_task 里
       · 等待不是死等：命中应答立刻返回，每 10 ms 让出 CPU，其它任务照常跑
       · 在线后每 5 秒上报一次位置（取代裸机版 main() 里那段的职责）
       · 每 30 秒发一条 AT 探活 —— 探活失败即判"断连"，自动重连 */

#include "WIFI.h"
#include "WIFI_Command.h"   /* 原写作 "WIFI_COMMAND.h"，大小写与实际文件名不符 —— Windows 能过，Linux 会编译失败，已修正 */
#include "secrets.h"        /* 只为上报主题 ONENET_TOPIC_POST；凭据本身在 WIFI_Command.c 里用 */
#include "FreeRTOS.h"
#include "task.h"
#include "gps.h"            /* Gps_GetFix()：上报时取一份定位快照 */
#include <stdio.h>
#include <string.h>
#include "key.h"    // 用于 Open_Clock / Close_Clock（原为 "KEY.h"，大小写不符）
#include "lock.h"   /* Lock_IsLocked() —— 上报锁状态时读它 */
#include "BUZ.h"    // （已不再直接调，见 alarm.h —— 蜂鸣器统一由告警模块仲裁）
#include "ui.h"     /* OLED 共享访问层：不再直接调 OLED_xxx()，一律走 UI_xxx() */
#include "log.h"    /* 任务里不再直接 printf，统一用 LOG() 投队列 */
#include "sys_state.h"  /* 状态中心：网络在线 / 告警等状态位 */
#include "alarm.h"      /* 蜂鸣器仲裁：云端 BUZ 命令的入口 + 断连告警 */

/* ---------------------------- 接收环形缓冲 ----------------------------
 * 这是典型的【单生产者-单消费者】结构，本身是安全的：
 *     · s_rx_head 只由 ISR 写
 *     · s_rx_tail 只由任务写
 *   两边各写各的索引、从不写对方的位置，所以不需要临界区。
 *
 * 但 s_rx_head **必须加 volatile**：任务侧在排空循环里反复读它，
 * 若编译器把它提升到寄存器，就会出现"ISR 明明写了、任务却看不见"的死循环。
 * 这正是原代码缺的一处（原来的 wifi_Head 没有 volatile）。
 * ------------------------------------------------------------------- */
#define WIFI_RX_BUF_SIZE   512U

static uint8_t           s_rx_buf[WIFI_RX_BUF_SIZE];
static volatile uint16_t s_rx_head = 0U;   /* 只由 ISR 写 → volatile 必需 */
static uint16_t          s_rx_tail = 0U;   /* 只由任务写               */

/* AT 应答匹配标志。只在 comm_task 上下文里读写，因此不需要加锁。 */
static uint8_t s_expect_hit = 0U;
static uint8_t s_error_hit  = 0U;

/* 行拼装缓冲。提到文件级（而不是 Wifi_PumpRx 的函数内 static），
 * 是为了让 Wifi_FlushRx() 能把它一起复位 —— 只清环形缓冲却留着半截行，
 * 下一次解析会从残片开始拼出错误的一行。 */
static char     s_line[256];
static uint16_t s_line_idx = 0U;

/* 前置声明 */
static void Wifi_PumpRx(const char *expect);
static void ParseMQTTCommand(char *payload);

/* 通信任务自己的句柄。vTaskComm 启动时保存，供别的任务发通知用。 */
static TaskHandle_t s_comm_task = NULL;

/* 供其它任务请求"把当前锁状态上报到云端"。

   用【任务通知】而不是队列：语义是"状态变了，去报一下"，最新的值覆盖旧的就行，
   不必排队（连按两次开关只需要报最终状态）。

   为什么不直接在 lock_task 里发 AT 指令：USART3 和 AT 状态机归 comm_task 独占，
   两个任务抢同一个外设必然互相打断。跨任务只传"请求"，真正的串口动作统一由
   comm_task 做。 */
void Net_RequestStatePublish(void)
{
    if (s_comm_task != NULL)
    {
        (void)xTaskNotify(s_comm_task, 1U, eSetValueWithOverwrite);
    }
}

/* ----------------------------------------------------------------------------
 * USART3 初始化：PB10 = TX（复用推挽）/ PB11 = RX（浮空输入），
 * 115200 / 8N1 / 无流控，打开 RXNE 中断。
 * -------------------------------------------------------------------------- */
void Uart_3_Init(void) {
    // 1. 使能 GPIOB 和 USART3 时钟
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART3, ENABLE);

    // 2. 配置 GPIO 引脚
    GPIO_InitTypeDef GPIO_InitStruct;
    
    // PB10 -> TX (复用推挽输出)
    GPIO_InitStruct.GPIO_Pin = GPIO_Pin_10;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &GPIO_InitStruct);
    
    // PB11 -> RX (浮空输入，或带上拉)
    GPIO_InitStruct.GPIO_Pin = GPIO_Pin_11;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &GPIO_InitStruct);

    //配置 USART3 参数
    USART_InitTypeDef USART_InitStruct;
    USART_InitStruct.USART_BaudRate = 115200;          
    USART_InitStruct.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStruct.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_InitStruct.USART_Parity = USART_Parity_No;
    USART_InitStruct.USART_StopBits = USART_StopBits_1;
    USART_InitStruct.USART_WordLength = USART_WordLength_8b;
    USART_Init(USART3, &USART_InitStruct);

    //使能接收中断
    USART_ITConfig(USART3, USART_IT_RXNE, ENABLE);

    //配置 NVIC
    /* 抢占优先级 2 -> 6（必须 >= FreeRTOS 要求的 5，
     * 否则中断里不能调用 xxxFromISR()）。Group_4 下子优先级填 0。 */
    NVIC_InitTypeDef NVIC_InitStruct;
    NVIC_InitStruct.NVIC_IRQChannel = USART3_IRQn;
    NVIC_InitStruct.NVIC_IRQChannelCmd = ENABLE;
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 6;
    NVIC_InitStruct.NVIC_IRQChannelSubPriority = 0;
    NVIC_Init(&NVIC_InitStruct);

    // 6. 使能 USART3
    USART_Cmd(USART3, ENABLE);
}

/* USART3 接收中断 —— 只把字节塞进环形缓冲，不做解析。
   （有 ORE 溢出处理；USART1 那边同样有一处。） */
void USART3_IRQHandler(void) {
    uint8_t data;

    if (USART_GetITStatus(USART3, USART_IT_RXNE) == SET) {
        data = (uint8_t)USART_ReceiveData(USART3);
        // 存入 WiFi 环形缓冲区（满则丢弃这一字节）
        if ((uint16_t)((s_rx_head + 1U) % WIFI_RX_BUF_SIZE) != s_rx_tail) {
            s_rx_buf[s_rx_head] = data;
            s_rx_head++;
            if (s_rx_head >= WIFI_RX_BUF_SIZE) {
                s_rx_head = 0U;
            }
        }
    }

    /* 溢出错误（ORE）：置位后 RXNE 不再被置起、链路会"假死" ——
     * 现象是 ESP8266 突然再也不回应任何指令，而 MCU 一切正常。
     * 清除办法是"先读 SR 再读 DR"，恰好由下面两句完成。 */
    if (USART_GetFlagStatus(USART3, USART_FLAG_ORE) != RESET) {
        (void)USART_ReceiveData(USART3);
    }
}

/* 丢弃接收缓冲里尚未被读走的数据。

   发新指令之前必须清一下：上一条指令的应答（尤其末尾那个 "OK"）如果还留在缓冲里，
   会被下一条指令的等待误判成"应答到了"。这是 AT 状态机最容易踩的坑之一。 */
static void Wifi_FlushRx(void)
{
    s_rx_tail  = s_rx_head;   /* 丢掉环形缓冲里未被读走的字节 */
    s_line_idx = 0U;          /* 半截行也一起丢掉，免得接出错误的一行 */
}

/* 发送一条 AT 指令（自动补 \r\n）。
   发送前先清掉陈旧应答，避免上一条的 "OK" 被下一条误认（见 Wifi_FlushRx 说明）。 */
void WiFi_SendCmd(const char *cmd) { //发送AT命令
    /* 先清掉陈旧应答，避免上一条的 "OK" 被下一条误认（见 Wifi_FlushRx 说明） */
    Wifi_FlushRx();

    while (*cmd){
        USART_SendData(USART3, (uint8_t)(*cmd++));
        while (USART_GetFlagStatus(USART3, USART_FLAG_TXE) == RESET);
    }
    // 发送换行符 \r\n
    USART_SendData(USART3, '\r');
    while (USART_GetFlagStatus(USART3, USART_FLAG_TXE) == RESET);
    USART_SendData(USART3, '\n');
    while (USART_GetFlagStatus(USART3, USART_FLAG_TXE) == RESET);
}

/* 把接收缓冲里的数据全部取出、按行解析。
   expect —— 期望在应答里出现的子串；NULL = 只处理下行命令、不匹配应答

   必须在 comm_task 里调用（读写 s_rx_tail 和两个标志）。

   关键点：AT 应答和 MQTT 下行命令走的是同一条串口，所以"等应答"和"处理下行命令"
   是同一件事的两面 —— 这也正是它比原来"发完就 delay_ms(3000)"强的地方：
   等待期间不会把数据噎住。 */
static void Wifi_PumpRx(const char *expect)
{
    char c;

    while (s_rx_tail != s_rx_head)
    {
        c = (char)s_rx_buf[s_rx_tail];
        s_rx_tail++;
        if (s_rx_tail >= WIFI_RX_BUF_SIZE) { s_rx_tail = 0U; }

        if (c == '\n')
        {
            s_line[s_line_idx] = '\0';
            s_line_idx = 0U;

            if ((expect != NULL) && (strstr(s_line, expect) != NULL))
            {
                s_expect_hit = 1U;
            }
            if ((strstr(s_line, "ERROR") != NULL) || (strstr(s_line, "FAIL") != NULL))
            {
                s_error_hit = 1U;
            }

            /* MQTT 下行命令随时可能到，不受当前连接步骤影响 */
            if ((strstr(s_line, "+MQTTSUBRECV") != NULL) ||
                (strstr(s_line, "+MQTTPUBLISH") != NULL))
            {
                ParseMQTTCommand(s_line);
            }
        }
        else if (s_line_idx < (sizeof(s_line) - 1U))
        {
            if (c != '\r') { s_line[s_line_idx] = c; s_line_idx++; }   /* 顺手去掉 '\r' */
        }
        /* 行超长：多余字符直接丢，等下一个 '\n' 复位 s_line_idx */
    }
}

/* Net_WaitFor 的三种结果 —— 区分"超时"和"明确报错"很重要，
 * 因为上报和探活对这两种情况的容忍度不一样（见下面的用法）。 */
#define NET_WAIT_TIMEOUT   0U
#define NET_WAIT_OK        1U
#define NET_WAIT_ERROR     2U

/* 等一个期望应答（边等边解析）。
   expect     —— 期望出现的应答子串
   timeout_ms —— 单次等待上限
   返回 NET_WAIT_OK / NET_WAIT_ERROR / NET_WAIT_TIMEOUT

   和原来"发完指令 delay_ms(3000)"的区别：
     · 命中就立刻返回 —— 快的时候 100 ms 就过，不用傻等满 3 秒
     · 每 10 ms 让出一次 CPU，GPS 和其它任务照常跑
     · 等待期间顺带把 MQTT 下行命令处理掉，不会丢 */
static uint8_t Net_WaitFor(const char *expect, uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();

    s_expect_hit = 0U;
    s_error_hit  = 0U;

    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms))
    {
        Wifi_PumpRx(expect);

        if (s_expect_hit != 0U) { return NET_WAIT_OK;    }
        if (s_error_hit  != 0U) { return NET_WAIT_ERROR; }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return NET_WAIT_TIMEOUT;
}

/* ----------------------------------------------------------------------------
 * 连接步骤表
 *
 * name 字段是给日志用的【短名】—— 日志里绝不能打完整指令，
 * 否则串口一接就让人看到 WiFi 密码和 OneNET 密钥。
 * -------------------------------------------------------------------------- */
typedef struct {
    const char *cmd;         /* 要发的 AT 指令 */
    const char *expect;      /* 期望出现的应答子串 */
    uint32_t    timeout_ms;  /* 单次等待上限 */
    uint8_t     retry_max;   /* 超时后最多再试几次（0 = 不重试） */
    const char *name;        /* 日志短名（不含凭据） */
} NetStep_t;

static const NetStep_t s_net_steps[] = {
    /* 先握个手并关回显：ATE0 能让后续应答更干净（模块不会把自己的指令回显回来） */
    { ATE0,         "OK",           1500U,  1U, "ATE0"      },
    /* 设为 Station 模式。裸机版把这步注释掉了，靠模块的持久化设置碰运气 ——
     * 一旦模块被恢复出厂设置就连不上，所以这里补上（幂等操作，无害）。 */
    { Station,      "OK",           1500U,  1U, "CWMODE"    },
    /* 连路由器：等 "WIFI GOT IP" 而不是 "OK"，
     * 因为密码错/找不到 AP 时模块也会回 ERROR，用更明确的串更稳 */
    { CWJAP,        "WIFI GOT IP", 25000U,  1U, "JOIN-AP"   },
    { MQTTUSERCFG,  "OK",           5000U,  1U, "MQTTCFG"   },
    { MQTTCONN,     "OK",           8000U,  1U, "MQTTCONN"  },
    { MQTTSUB,      "OK",           5000U,  1U, "SUB-REPLY" },
    { MQTTSUB_CMD,  "OK",           5000U,  1U, "SUB-SET"   },
};
#define NET_STEP_COUNT   (sizeof(s_net_steps) / sizeof(s_net_steps[0]))

/* 上报与探活周期 */
#define NET_REPORT_PERIOD_MS   5000U    /* 位置上报：5 秒（与裸机版一致） */
#define NET_PROBE_PERIOD_MS    30000U   /* AT 探活：30 秒一次 */
#define NET_PROBE_FAIL_LIMIT   2U       /* 连续 2 次探活失败即判断连 */
#define NET_PROBE_TIMEOUT_MS   2000U

/* 解析一条 MQTT 下行命令（payload 是 "+MQTTSUBRECV" 那整行）。
   支持云端下发的 LED（锁）和 BUZ（蜂鸣）两个属性。 */
void ParseMQTTCommand(char *payload)
{
    // 查找 JSON 起始位置
    char *json = strchr(payload, '{');
    if (!json) return;

    /* 锁状态位要跟着一起更新：LOCK 和 BLOCK 两处都会改锁 ——
     * 云端下发是其中之一，另一处是指纹开锁（lock.c）。它们都改完就同步一次
     * SYS_BIT_UNLOCKED，别处（OLED、上报）只读不猜。 */
    if (strstr(json, "\"LED\":true") || strstr(json, "\"LED\":1"))
    {
        Open_Clock();   // 继电器控制函数
		UI_ShowChinese(72,36,"开");
		UI_Update();
        Sys_UpdateBit(SYS_BIT_UNLOCKED, Lock_IsLocked());
        LOG("[cmd] LED ON\r\n");
    }
    else if (strstr(json, "\"LED\":false") || strstr(json, "\"LED\":0"))
    {
        Close_Clock();
		UI_ShowChinese(72,36,"关");
		UI_Update();
        Sys_UpdateBit(SYS_BIT_UNLOCKED, Lock_IsLocked());
        LOG("[cmd] LED OFF\r\n");
    }

    /* 蜂鸣器不再在这里直接开关，交给告警模块统一仲裁：
     * 云端"找箱子"的请求和固件自己的"断连告警"都可能要用蜂鸣器，
     * 各自直接写寄存器会互相覆盖（后写的那句把前一句的效果盖掉）。
     * 这里只表达"云端请求响 / 不响"，什么时候真正响由 alarm_task 裁决。 */
    if (strstr(json, "\"BUZ\":true") || strstr(json, "\"BUZ\":1"))
    {
        Alarm_SetRemoteBuzzer(1U);
    }
    else if (strstr(json, "\"BUZ\":false") || strstr(json, "\"BUZ\":0"))
    {
        Alarm_SetRemoteBuzzer(0U);
    }
}

/* 上报一次位置。
   lon / lat —— 经纬度字符串（十进制度）
   返回 1 = 发出且未收到明确错误；0 = 收到 ERROR/FAIL

   【为什么不把 json/cmd 放栈上】两个 buffer 合计 416 字节，comm_task 栈只有
   1792 字节，放栈上太奢侈。改成 static 后它们的开销落到 .bss。代价是函数不可重入 ——
   但只有 comm_task 会调用它，安全。 */
uint8_t MQTT_Publish(const char *lon, const char *lat) {
    static char json[160];
    static char cmd[256];
    uint8_t     rc;

    /* 格式串里的 `\,` 是历史遗留的非标准转义序列（C 里根本没有这个转义）。
     * 实测 AC5 / AC6 生成的字符串字节完全一致、OneNET 也能正常收到，所以
     * 【本次不动它】—— 改它会改变发给云端的内容，必须先上机确认。
     * 只把 sprintf 换成 snprintf 加个上界，输出内容不变。 */
    (void)snprintf(json, sizeof(json),
        "{\\\"id\\\":\\\"123\\\"\\,\\\"params\\\":{\\\"LONG\\\":{\\\"value\\\":\\\"%s\\\"}\\,\\\"LAT\\\":{\\\"value\\\":\\\"%s\\\"}}}",
        lon, lat);

    (void)snprintf(cmd, sizeof(cmd),
        "AT+MQTTPUB=0,\"" ONENET_TOPIC_POST "\",\"%s\",0,0", json);

    WiFi_SendCmd(cmd);
    LOG("[mqtt] pub lon=%s lat=%s\r\n", lon, lat);

    /* 不再盲等 500 ms。等 AT 的 OK：
     *  · 收到 OK      → 成功
     *  · 收到 ERROR   → 明确失败（模块回了错误）
     *  · 超时         → 也当作"已发出"。有些 AT 固件的 MQTTPUB 不额外回 OK，
     *                   不能因为"没回 OK"就误判成断连。
     * 链路是否真的活着，交给下面 30 秒一次的 AT 探活去判（那个一定会回 OK）。 */
    rc = Net_WaitFor("OK", 2000U);
    return (rc == NET_WAIT_ERROR) ? 0U : 1U;
}

/* 把当前锁状态上报到云端（由 Net_RequestStatePublish() 触发）。
   报文格式与原裸机版完全一致（含历史遗留的 `\,`）：
     {"id":"2","params":{"LED":{"value":true/false}}} */
static void PublishLockState(void)
{
    static char cmd[192];
    uint8_t     locked = Lock_IsLocked();

    (void)snprintf(cmd, sizeof(cmd),
        "AT+MQTTPUB=0,\"" ONENET_TOPIC_POST "\","
        "\"{\\\"id\\\":\\\"2\\\"\\,\\\"params\\\":{\\\"LED\\\":{\\\"value\\\":%s}}}\",0,0",
        (locked != 0U) ? "true" : "false");

    WiFi_SendCmd(cmd);
    LOG("[net] pub lock=%s\r\n", (locked != 0U) ? "ON" : "OFF");
    (void)Net_WaitFor("OK", 2000U);
}

/* 上报"告警状态"到云端。
   active —— 1 = 告警中，0 = 告警已解除
   返回 1 = 已发出且未收到明确错误；0 = 模块回了 ERROR/FAIL

   【能报的只有"解除"，报不了"发生"】—— 因为本模块唯一会主动置起的告警就是
   "链路断了"，而链路断的时候这条报文根本发不出去。

   所以固件侧的实际用法是：断连 → 本地蜂鸣（给现场的人）；
   重连成功 → 报一条 active=0（告诉云端"刚才失联过，现在回来了"）。
   "箱子已经掉线"这件事由【客户端】自己判 —— 它发现设备超过 N 秒没有位置上报，
   就该自己去报警。这与"判定放在客户端"的整体设计一致，也避免了固件去猜
   "云端认为我掉线了没有"。

   ⚠️ 需要 OneNET 后台配合：物模型里要有布尔型属性 ALARM，否则这条会被云端拒绝
      （现象是日志里出现 ERROR）。没加也不影响别的功能 —— 上报失败只记日志，
      不参与断连判定，位置上报和锁状态上报照常。 */
uint8_t MQTT_PublishAlarm(uint8_t active)
{
    static char cmd[192];
    uint8_t     rc;

    (void)snprintf(cmd, sizeof(cmd),
        "AT+MQTTPUB=0,\"" ONENET_TOPIC_POST "\","
        "\"{\\\"id\\\":\\\"3\\\"\\,\\\"params\\\":{\\\"ALARM\\\":{\\\"value\\\":%s}}}\",0,0",
        (active != 0U) ? "true" : "false");

    WiFi_SendCmd(cmd);
    LOG("[net] pub alarm=%s\r\n", (active != 0U) ? "ON" : "OFF");

    rc = Net_WaitFor("OK", 2000U);
    return (rc == NET_WAIT_ERROR) ? 0U : 1U;
}

/* 通信任务：连接状态机 → 周期上报 → 周期探活。

   裸机版把 Wifi_init() 放在 main() 里用 delay_ms 硬等 13 秒 —— 那 13 秒内
   GPS 解析、指纹、OLED 全部停摆。搬进任务后等待由 vTaskDelay 让出 CPU，
   其它任务照常运行。这就是"为什么要上 RTOS"最直接的证据。

   三个状态量：
     online = 0 → 跑连接状态机（step / retry 是它的游标）
     online = 1 → 每 5 秒上报位置，每 30 秒 AT 探活 */
void vTaskComm(void *pvParameters)
{
    uint8_t    step     = 0U;
    uint8_t    retry    = 0U;
    uint8_t    online   = 0U;
    uint8_t    probe_fail = 0U;
    uint8_t    link_recovered = 0U;   /* 断连后刚恢复：上线时补报一条"告警解除" */
    uint16_t   i;
    TickType_t last_pub   = 0U;
    TickType_t last_probe = 0U;
    GPS_Data_t fix;
    char       lon[16];
    char       lat[16];

    (void)pvParameters;

    /* 记下自己的句柄 —— 别的任务要靠它发任务通知来请求上报 */
    s_comm_task = xTaskGetCurrentTaskHandle();

    for (;;)
    {
        if (online == 0U)
        {
            /* ==================== 连接状态机 ==================== */
            const NetStep_t *st = &s_net_steps[step];

            if (retry == 0U)
            {
                /* 只打短名 —— 完整指令里有 WiFi 密码和 OneNET 密钥，不能进日志 */
                LOG("[net] %s: send\r\n", st->name);
                WiFi_SendCmd(st->cmd);
            }

            if (Net_WaitFor(st->expect, st->timeout_ms) == NET_WAIT_OK)
            {
                retry = 0U;
                step++;
                if (step >= NET_STEP_COUNT)
                {
                    online     = 1U;
                    probe_fail = 0U;
                    last_pub   = xTaskGetTickCount();
                    last_probe = xTaskGetTickCount();

                    /* 把"在线"这个事实写进状态中心 —— 之后 OLED、
                     * 以及任何想知道"网络通不通"的地方都只看这一位。 */
                    Sys_UpdateBit(SYS_BIT_NET_ONLINE, 1U);

                    /* 如果刚才是断连告警状态，说明这是【失联后恢复】，
                     * 要补报一条"告警解除"告知云端。注意必须在清位【之前】
                     * 读出来 —— 清完再读就永远是 0 了。 */
                    if ((Sys_GetBits() & SYS_BIT_ALARM) != 0U)
                    {
                        link_recovered = 1U;
                    }
                    Alarm_SetLinkAlarm(0U);     /* 解除本地告警（蜂鸣停止） */

                    LOG("[net] ONLINE\r\n");
                }
            }
            else
            {
                retry++;
                if (retry > st->retry_max)
                {
                    /* 这一步反复失败 → 整体从头来。先退避 10 秒再重试，
                     * 避免"连不上就一直高频重连"把 ESP8266 和路由器都拖垮。
                     * （裸机版没有这一层，连不上就是默默失联。） */
                    LOG("[net] %s failed, back off 10s then restart\r\n", st->name);

                    for (i = 0U; i < 100U; i++)
                    {
                        vTaskDelay(pdMS_TO_TICKS(100));
                        Wifi_PumpRx(NULL);   /* 退避期间也别把下行命令丢了 */
                    }
                    step  = 0U;
                    retry = 0U;
                }
                else
                {
                    LOG("[net] %s timeout, retry %u\r\n", st->name, (unsigned)retry);
                }
            }
        }
        else
        {
            /* ==================== 在线：上报 + 探活 ==================== */
            Wifi_PumpRx(NULL);      /* 只处理下行命令，不匹配应答 */

            /* ---- ⓪ 断连恢复补报 ----
             * MQTT 刚连上就立刻发可能不稳，所以放到进入在线循环后的第一轮。 */
            if (link_recovered != 0U)
            {
                link_recovered = 0U;
                (void)MQTT_PublishAlarm(0U);
            }

            /* ---- ① 别处请求的立即上报（指纹开锁 / 物理开关）----
             * 用非阻塞的 xTaskNotifyWait（超时 0）：有就处理，没有就往下走，
             * 不会拖慢后面的上报和探活节奏。 */
            {
                uint32_t notify_val;

                if (xTaskNotifyWait(0U, 0U, &notify_val, 0U) == pdTRUE)
                {
                    PublishLockState();
                }
            }

            /* ---- ② 每 5 秒上报一次位置 ----
             * 这段职责原来在裸机版 main() 的主循环里，现在归 comm_task：
             * 上报本身就走 USART3 + AT 指令，和通信是同一条链路，
             * 放这儿既省一个任务、也省一份栈（原来计划里的 report_task 因此取消）。 */
            if ((xTaskGetTickCount() - last_pub) >= pdMS_TO_TICKS(NET_REPORT_PERIOD_MS))
            {
                last_pub = xTaskGetTickCount();

                if (Gps_GetFix(&fix) != 0U)
                {
                    (void)snprintf(lon, sizeof(lon), "%.6f", fix.lon_dec);
                    (void)snprintf(lat, sizeof(lat), "%.6f", fix.lat_dec);

                    if (MQTT_Publish(lon, lat) == 0U)
                    {
                        LOG("[net] publish got ERROR\r\n");
                    }
                }
                else
                {
                    /* 没有有效定位就跳过这次上报。
                     * 【刻意不报 0,0 或上次的旧坐标】—— 云端会把无效坐标
                     * 当真实位置画到地图上，那比"不报"更糟。 */
                    LOG("[net] no GPS fix, skip publish\r\n");
                }
            }

            /* ---- ③ 每 30 秒 AT 探活 ----
             * 这是"断连可感知"的落点：任务是死是活、AT 层还通不通，
             * 一条 AT 就能问清楚（模块正常时必回 OK）。
             * 探活要看得比上报严格 —— 必须明确收到 OK 才算过。 */
            if ((xTaskGetTickCount() - last_probe) >= pdMS_TO_TICKS(NET_PROBE_PERIOD_MS))
            {
                last_probe = xTaskGetTickCount();

                WiFi_SendCmd(AT);   /* 发一条最简指令：模块活着就必回 OK */

                if (Net_WaitFor("OK", NET_PROBE_TIMEOUT_MS) == NET_WAIT_OK)
                {
                    probe_fail = 0U;
                }
                else
                {
                    probe_fail++;
                    LOG("[net] probe fail (%u/%u)\r\n",
                        (unsigned)probe_fail, (unsigned)NET_PROBE_FAIL_LIMIT);

                    if (probe_fail >= NET_PROBE_FAIL_LIMIT)
                    {
                        /* AT 层都不通了 = 链路已断。回到未上线状态重连，
                         * 这样"断连"是可感知、可自愈的，而不是默默失联。 */
                        online     = 0U;
                        probe_fail = 0U;
                        step       = 0U;
                        retry      = 0U;

                        /* 断连告警 —— 任务书第 4 条"断连 → 蜂鸣器"在固件侧的
                         * 落点就在这里。
                         *
                         * 为什么这件事只能固件侧判：手机无从区分"箱子掉线"
                         * 和"箱子关机/没电"。只有箱子自己发现"我联系不上云端了"
                         * 才靠得住。
                         *
                         * 注意：只有【曾经在线过、后来断了】才算告警 ——
                         * 上电就连不上（比如路由器没开）不发告警，那种情况下
                         * 蜂鸣器乱响反而是误报。防丢的语义是"它本来跟着我、
                         * 现在跟丢了"。 */
                        Sys_UpdateBit(SYS_BIT_NET_ONLINE, 0U);
                        Alarm_SetLinkAlarm(1U);

                        LOG("[net] link lost -> reconnect\r\n");
                        continue;
                    }
                }
            }

            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}
