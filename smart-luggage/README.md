# smart-luggage — Intelligent Luggage

An intelligent luggage control system based on STM32F103C8 + FreeRTOS V11.1.0.

**English** | [简体中文](README.zh-CN.md)

## 1. Implemented Features

| Feature | Implementation |
|---|---|
| GPS positioning | ATGM336H module (GPS + BeiDou) on USART1, 9600 bps / 8N1. Parses NMEA 0183 GGA sentences and outputs decimal-degree latitude/longitude, fix quality and satellite count |
| Fingerprint unlock | ZW101 module on USART2, 9600 bps. A touch signal on PA1 triggers an EXTI interrupt. Parses the confirmation code and match result of the response frame; on a successful match, drives the relay on PB12 |
| Cloud connectivity | ESP8266 on USART3, 115200 bps. Establishes an MQTT connection to OneNET, periodically reports position and lock state, and receives property downlinks (LED / BUZ) |
| Local locking | Physical switch on PA0, edge detection. Locked when switched low |
| Anti-loss alarm | Buzzer (TIM1_CH1N, PWM). Continuous tone on cloud request; intermittent tone on link loss (200 ms on, 1800 ms off, 2 s period) |
| Status display | OLED (I2C). Shows fix state, network state, lock state and alarm flag |
| Runtime log | USART1 TX (PA9), 9600 bps. All log output is serialized through a queue |

## 2. Task Flow

### 2.1 Startup

```
Reset
 ├─ System clock, NVIC priority group (Group_4)
 ├─ Peripheral init   Key_init / BUZ_init / Uart_Iint / Uart_2_init / Uart_3_init / OLED_Init
 ├─ Software objects  Log_Init / UI_Init / Sys_Init / Gps_Init / Lock_Init
 ├─ Lock_GpioInit (EXTI setup, must run after Uart_2_init)
 ├─ Create 6 tasks (gps / comm / lock / alarm / hb / log)
 └─ vTaskStartScheduler()
```

Two ordering constraints apply:

- Hardware initialization must complete before the scheduler starts — interrupt service routines may fire before the scheduler is running;
- Software objects must be created before the corresponding interrupt is enabled — otherwise the interrupt may arrive before the queue or semaphore exists.

### 2.2 GPS Input Path

```
USART1 RXNE interrupt
  └─ USART_ReceiveData reads one byte; the hardware clears RXNE
     └─ Gps_RxByteFromISR appends it to the line buffer; on '\n' appends '\0' to terminate the line
        └─ xQueueSendFromISR enqueues the whole line to gps_q (depth 4, 96 B per item)
           └─ vTaskGps receives, timeout 3000 ms
              ├─ ParseGGA extracts fields 2 / 4 / 6 and converts ddmm.mmmm to decimal degrees
              ├─ Writes to the shared snapshot s_fix under a mutex
              └─ Updates SYS_BIT_GPS_FIX when the fix state changes
```

The ISR only receives bytes and enqueues them; it does not parse sentences. The 3000 ms timeout is used to detect a missing module.

### 2.3 Fingerprint Unlock Path

```
PA1 rising edge
  └─ EXTI1_IRQHandler gives the binary semaphore
     └─ vTaskLock takes the semaphore, waits 20 ms, then re-checks the pin level (debounce)
        └─ ZW101_SendCommand sends the auto-identify command

USART2 RXNE interrupt
  └─ Lock_FingerRxByteFromISR assembles a frame per the ZW101 protocol
     └─ xQueueSendFromISR enqueues the whole frame to finger_q (depth 4, 64 B per item)
        └─ vTaskLock checks frame length and checksum
           └─ On a successful match
              ├─ Drives the relay to unlock
              ├─ Refreshes the OLED display
              ├─ Updates SYS_BIT_UNLOCKED
              └─ Requests comm_task to report the lock state via a task notification
```

### 2.4 Cloud Connectivity Path

vTaskComm is a two-state loop.

**Disconnected state** — Executes 7 AT commands in fixed order (ATE0 → CWMODE → CWJAP → MQTTUSERCFG → MQTTCONN → MQTTSUB ×2). Each waits for its expected response and retries once on timeout; if a full round fails, it backs off 10 s and restarts.

**Connected state** — Reports position every 5 s (skipped when no valid fix is available); sends an AT command every 30 s, and treats 2 consecutive send failures as link loss, returning to the disconnected state.

**Downlink handling** — On `+MQTTSUBRECV`, parses the JSON payload: the `LED` field controls the relay, the `BUZ` field sets `SYS_BIT_REMOTE_BUZ`.

**Cross-task request** — lock_task requests a lock-state report via a task notification; the actual UART operation is performed by comm_task, which owns USART3 exclusively.

### 2.5 Alarm Path

Two alarm sources, arbitrated by vTaskAlarm:

| Source | Trigger | Tone |
|---|---|---|
| Cloud request | `BUZ:true` received; sets `SYS_BIT_REMOTE_BUZ` | Continuous |
| Link loss | comm_task detects a broken link; sets `SYS_BIT_ALARM` (gated by the `ALARM_ENABLE_LINK_ALARM` compile switch, off by default) | Intermittent |

```
vTaskAlarm blocks on Sys_WaitBits(SYS_BITS_BUZZER, 1000 ms)
  ├─ Any bit set → enters the tone loop until both bits clear
  └─ Timed out with no bit set → stays silent, keeps waiting
```

### 2.6 Task Cooperation

System state is held in a single event group. Each task writes only the bits it can determine; other tasks read them:

| Event bit | Written by | Read by |
|---|---|---|
| `SYS_BIT_GPS_FIX` | gps_task | hb_task (display), comm_task (report decision) |
| `SYS_BIT_NET_ONLINE` | comm_task | hb_task (display) |
| `SYS_BIT_UNLOCKED` | lock_task, comm_task | hb_task (display), comm_task (report) |
| `SYS_BIT_ALARM` | comm_task | alarm_task (blocking wait) |
| `SYS_BIT_REMOTE_BUZ` | comm_task | alarm_task (blocking wait) |

## 3. Layering

```
Application   code/          6 tasks (gps / comm / lock / alarm / hb / log)
                             Log serialization, OLED mutex layer, system state center
                             GPS sentence parsing, fingerprint frame parsing, AT state machine

Driver        module/        UART I/O and frame assembly in ISRs, AT command set,
                             ZW101 protocol, buzzer PWM, relay, key, OLED display

Kernel        FreeRTOS/      Task scheduling, queues, mutexes, binary semaphore,
                             event group, task notification

Library       lib/  start/   STM32F10x standard peripheral library, CMSIS, startup file
```

**Call relationships**

- Application tasks do not call each other directly; they cooperate exclusively through the IPC objects provided by the kernel;
- The application layer calls the driver layer for peripheral operations;
- The driver layer interacts with the kernel through the `...FromISR` APIs in interrupt context;
- The kernel and library layers do not depend on the application layer.

**Code provenance**

| Directory | Origin |
|---|---|
| `code/` | Implemented in this project |
| `module/` | Implemented in this project (`OLED.c`, `OLED_Data.c` are a third-party display driver) |
| `FreeRTOS/` | Third-party — FreeRTOS kernel V11.1.0 |
| `lib/`, `start/` | Third-party — ST standard peripheral library, CMSIS and startup file |

## 4. Build

Keil MDK 5 with ARM Compiler 6, strict C99. Open `xinglix_RTOS.uvprojx` and build.

Result: 0 errors, 0 warnings, 47.0 KB flash, 17.2 KB RAM.

Before building, copy `code/secrets.h.example` to `code/secrets.h` and fill in your Wi-Fi and OneNET credentials. The real credentials file is not part of this repository.
