# smart-luggage — 智能行李箱

基于 STM32F103C8 + FreeRTOS V11.1.0 的智能行李箱控制系统。

[English](README.md) | **简体中文**

## 一、功能实现

| 功能     | 实现方式                                                                                       |
| ------ | ------------------------------------------------------------------------------------------ |
| GPS 定位 | ATGM336H 模块（GPS + 北斗双模）经 USART1 输入，9600 bps / 8N1。解析 NMEA 0183 的 GGA 语句，输出十进制度经纬度、定位质量与卫星数 |
| 指纹开锁   | ZW101 模块经 USART2 通信，9600 bps。PA1 触摸信号经 EXTI 触发中断；解析应答帧的确认码与比对结果，比对成功后驱动 PB12 继电器           |
| 云端通信   | ESP8266 经 USART3 通信，115200 bps。与 OneNET 建立 MQTT 连接，周期上报经纬度与锁状态，接收属性下发（LED / BUZ）           |
| 本地开锁   | PA0 物理开关，边沿检测。拨至低位时闭锁                                                                      |
| 防丢告警   | 蜂鸣器（TIM1_CH1N，PWM）。云端请求时持续鸣响；链路断开时间断鸣响（响 200 ms、停 1800 ms，周期 2 s）                          |
| 状态显示   | OLED（I2C）。显示定位状态、网络状态、锁状态与告警标志                                                             |
| 运行日志   | USART1 TX（PA9）输出，9600 bps。全系统日志统一经队列串行输出                                                   |

## 二、任务流程

### 2.1 启动流程

```
复位
 ├─ 系统时钟、NVIC 分组（Group_4）
 ├─ 外设初始化   Key_init / BUZ_init / Uart_Iint / Uart_2_init / Uart_3_init / OLED_Init
 ├─ 软件资源     Log_Init / UI_Init / Sys_Init / Gps_Init / Lock_Init
 ├─ Lock_GpioInit（EXTI 配置，须在 Uart_2_init 之后执行）
 ├─ 创建 6 个任务（gps / comm / lock / alarm / hb / log）
 └─ vTaskStartScheduler()
```

初始化顺序存在两条约束条件：

- 硬件初始化须在调度器启动前完成 —— 中断服务函数在调度器运行前即可能触发；
- 软件资源须早于对应中断使能 —— 否则中断到达时队列或信号量尚未创建。

### 2.2 GPS 输入链路

```
USART1 RXNE 中断
  └─ USART_ReceiveData 读取字节，硬件自动清除 RXNE
     └─ Gps_RxByteFromISR 逐字节写入行缓冲，遇 '\n' 补 '\0' 成行
        └─ xQueueSendFromISR 整行入队 gps_q（深度 4，每项 96 B）
           └─ vTaskGps 接收，超时 3000 ms
              ├─ ParseGGA 解析字段 2 / 4 / 6，度分格式转十进制度
              ├─ 加锁写入共享快照 s_fix
              └─ 定位状态跳变时更新 SYS_BIT_GPS_FIX
```

中断内仅执行字节接收与入队，不进行语句解析。3000 ms 超时用于判定模块离线。

### 2.3 指纹开锁链路

```
PA1 上升沿
  └─ EXTI1_IRQHandler 释放二值信号量
     └─ vTaskLock 获取信号量，延时 20 ms 后复检电平（消抖）
        └─ ZW101_SendCommand 下发自动识别指令

USART2 RXNE 中断
  └─ Lock_FingerRxByteFromISR 按 ZW101 协议成帧
     └─ xQueueSendFromISR 整帧入队 finger_q（深度 4，每项 64 B）
        └─ vTaskLock 校验帧长与校验和
           └─ 比对成功
              ├─ 驱动继电器开锁
              ├─ 刷新 OLED 显示
              ├─ 更新 SYS_BIT_UNLOCKED
              └─ 通过任务通知请求 comm_task 上报锁状态
```

### 2.4 云端通信链路

vTaskComm 为双状态循环。

**未连接状态** —— 按固定顺序执行 7 条 AT 指令（ATE0 → CWMODE → CWJAP → MQTTUSERCFG → MQTTCONN → MQTTSUB ×2），每条等待预期应答，单条超时重试一次；整轮失败后退避 10 s 重新开始。

**已连接状态** —— 每 5 s 上报一次经纬度（无有效定位时跳过本次上报）；每 30 s 发送一条 AT 指令，连续 2 次发送失败判定链路断开，返回未连接状态。

**下行处理** —— 收到 `+MQTTSUBRECV` 时解析 JSON：`LED` 字段控制继电器，`BUZ` 字段置位 `SYS_BIT_REMOTE_BUZ`。

**跨任务请求** —— lock_task 通过任务通知请求上报锁状态，串口操作由 comm_task 执行（USART3 由 comm_task 独占）。

### 2.5 告警链路

告警来源有两个，统一由 vTaskAlarm 进行仲裁：

| 来源   | 触发条件                                                                    | 鸣响方式 |
| ---- | ----------------------------------------------------------------------- | ---- |
| 云端请求 | 收到 `BUZ:true`，置位 `SYS_BIT_REMOTE_BUZ`                                   | 持续鸣响 |
| 链路断开 | comm_task 判定断连，置位 `SYS_BIT_ALARM`（当前由编译开关 `ALARM_ENABLE_LINK_ALARM` 屏蔽） | 间断鸣响 |

```
vTaskAlarm 阻塞于 Sys_WaitBits(SYS_BITS_BUZZER, 1000 ms)
  ├─ 任一位有效 → 进入鸣叫循环，直至两位均失效
  └─ 超时且均无效 → 保持静音，继续等待
```

### 2.6 任务协作

系统状态由事件组统一持有。各任务仅写入自己可知的位，其余任务只读：

| 事件位                  | 写入方                 | 读取方                           |
| -------------------- | ------------------- | ----------------------------- |
| `SYS_BIT_GPS_FIX`    | gps_task            | hb_task（显示）、comm_task（判定是否上报） |
| `SYS_BIT_NET_ONLINE` | comm_task           | hb_task（显示）                   |
| `SYS_BIT_UNLOCKED`   | lock_task、comm_task | hb_task（显示）、comm_task（上报）     |
| `SYS_BIT_ALARM`      | comm_task           | alarm_task（阻塞等待）              |
| `SYS_BIT_REMOTE_BUZ` | comm_task           | alarm_task（阻塞等待）              |

## 三、层次划分

```
应用层    code/           6 个任务（gps / comm / lock / alarm / hb / log）
                         日志串行化、OLED 互斥访问层、系统状态中心
                         GPS 语句解析、指纹帧解析、AT 状态机

驱动层    module/         串口收发与中断成帧、AT 指令集、ZW101 协议
                         蜂鸣器 PWM、继电器、按键、OLED 显示

内核层    FreeRTOS/       任务调度、队列、互斥量、二值信号量
                         事件组、任务通知

库层      lib/  start/    STM32F10x 标准外设库、CMSIS、启动文件
```

**层间调用关系**

- 应用层任务之间不直接调用，全部通过内核层提供的 IPC 对象协作；
- 应用层调用驱动层完成外设操作；
- 驱动层在中断上下文中通过 `...FromISR` API 与内核层交互；
- 内核层与库层不依赖应用层。

**代码归属**

| 目录              | 来源                                     |
| --------------- | -------------------------------------- |
| `code/`         | 本项目实现                                  |
| `module/`       | 本项目实现（`OLED.c`、`OLED_Data.c` 为第三方显示驱动） |
| `FreeRTOS/`     | 第三方 —— FreeRTOS 内核 V11.1.0             |
| `lib/`、`start/` | 第三方 —— ST 标准外设库、CMSIS 与启动文件            |

## 四、编译

Keil MDK 5 + ARM Compiler 6，严格 C99。打开 `xinglix_RTOS.uvprojx` 编译。

编译结果：0 error / 0 warning，Flash 47.0 KB，RAM 17.2 KB。

编译前须将 `code/secrets.h.example` 复制为 `code/secrets.h`，填入 WiFi 与 OneNET 凭据。

