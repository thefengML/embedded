# embedded

Projects related to embedded systems.

## Contents

### [smart-luggage](smart-luggage/) — 智能行李箱

An intelligent luggage system built on **STM32F103C8 + FreeRTOS V11.1.0**.

- **GPS positioning** — ATGM336H (GPS + BeiDou) on USART1
- **Fingerprint unlock** — ZW101 on USART2, touch wake-up via EXTI
- **Cloud connectivity** — ESP8266 + OneNET MQTT on USART3
- **Anti-loss alarm** — buzzer arbitration driven by an event group

6 tasks, 5 IPC mechanisms, no busy-waiting anywhere. Footprint: 47.0 KB flash / 17.2 KB RAM.

→ [Project README](smart-luggage/README.md) · [Schematic](smart-luggage/原理图/SCH_Schematic1.pdf)

## Note on credentials

No credentials are stored in this repository. Before building, copy
`smart-luggage/code/secrets.h.example` to `smart-luggage/code/secrets.h`
and fill in your own Wi-Fi and OneNET settings.
