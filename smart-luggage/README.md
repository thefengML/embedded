# smart-luggage — 智能行李箱

An intelligent luggage system based on **STM32F103C8 + FreeRTOS V11.1.0**,
developed as a graduation project.

**English** | [简体中文](README.zh-CN.md)

## Features

| Feature | Hardware | Interface |
|---|---|---|
| GPS positioning | ATGM336H (GPS + BeiDou) | USART1 @ 9600 |
| Fingerprint unlock | ZW101 | USART2 @ 9600 + PA1 touch (EXTI) |
| Cloud connectivity | ESP8266 + OneNET MQTT | USART3 @ 115200 |
| Physical lock | Relay | PB12 (GPIO) |
| Anti-loss alarm | Buzzer | TIM1_CH1N (PWM) |
| Status display | OLED | I2C |

## Architecture

Six FreeRTOS tasks:

| Task | Priority | Responsibility | Blocks on |
|---|---|---|---|
| `log` | 1 | Serializes all log output | queue (forever) |
| `hb` | 2 | OLED refresh + status log | `vTaskDelay(1000)` |
| `lock` | 3 | Fingerprint frame parsing / unlock | queue (100 ms) + semaphore |
| `alarm` | 3 | Buzzer arbitration | event group (1000 ms timeout) |
| `gps` | 4 | NMEA parsing | queue (3000 ms) |
| `comm` | 5 | AT state machine, reporting, keep-alive | task notification |

Priorities are ordered by **deadline** ("how soon does this break?"), not by
importance — the cloud heartbeat timeout is the hardest deadline, so `comm` is
highest.

Five IPC mechanisms are in real use: queues (3), mutexes (2), a binary
semaphore, an event group and a task notification. Every ISR uses the
`...FromISR` API, and no task polls — all of them block on something.

## Why RTOS

The bare-metal version had two provable defects, both fixed here:

1. **Busy-waiting on the fingerprint sensor** — `while (PA1 == SET) {}` froze the
   entire main loop while a finger rested on the sensor. The GPS receive buffer
   (200 bytes at 9600 bps) overflows in about 0.2 s, so **remote unlock was
   unavailable exactly while someone was unlocking locally**. This is not a
   theoretical risk — it is a structural consequence.

2. **13 seconds of blind waiting during network setup** — six consecutive
   `delay_ms(3000)` calls with no response checking and no retry. If the ESP8266
   was absent or the router was off, the system still waited the full 13 s.

## Directory layout

```
code/        application layer — tasks, IPC usage, state center
module/      device drivers — UART, Wi-Fi/AT, fingerprint, buzzer, key
FreeRTOS/    FreeRTOS kernel V11.1.0
lib/         STM32F10x standard peripheral library
start/       CMSIS + startup file
原理图/       schematic (PDF)
```

## Build

Keil MDK 5 with ARM Compiler 6, strict C99.

Open `xinglix_RTOS.uvprojx` and build. Result: **0 errors, 0 warnings**,
47.0 KB flash / 17.2 KB RAM.

**Before building**, create `code/secrets.h` by copying
`code/secrets.h.example` and fill in your own credentials — the real file is
intentionally excluded from this repository.
