# RTU — pinagem ideal (ESP32-WROOM-32)

Objetivo: mapa limpo para a placa definitiva — sem strapping em controles,
input-only só onde cabe, UARTs/SPI agrupados, enable do modem no ESP.

Não tenta preservar a proposta atual; ver `pinout_min_diff.md` para o diff mínimo.

## Regras aplicadas

- Não usar GPIO6–11 (SPI flash do módulo)
- Não usar GPIO1/3 (UART0 USB/console)
- Evitar strapping em saídas críticas: GPIO0, 2, 5, 12, 15
- GPIO34–39 só como entrada (UART RX / CAN RX)
- Um CS dedicado no SD
- PWR/EN do A7670 controlável pelo ESP

## Conexões

Convenção: pino do **módulo** ⇒ ESP32.

### LoRa (Ebyte E32) — UART2

| Módulo | ESP32 | Notas |
|---|---|---|
| RX | GPIO25 | ESP TX → RX do módulo |
| TX | GPIO26 | ESP RX ← TX do módulo |
| M0 | GPIO32 | Fora de strapping |
| M1 | GPIO33 | Fora de strapping |
| AUX | GPIO13 | Input |
| VCC | rail 5 V (ou VIN se a fonte aguentar TX) | Preferir regulador da placa |
| GND | GND | |

### GPRS (A7670SA) — UART1 + LLC 1,8↔3,3 V

| Módulo | Caminho | ESP32 | Notas |
|---|---|---|---|
| 1 (VCC) | — | fonte 3,8–4,2 V / ≥2 A | **Não** usar 3V3 do ESP |
| 2 (GND) | — | GND | |
| 3 (TXD) | LLC LV→HV | **GPIO34** | ESP RX (input-only) |
| 4 (RXD) | LLC HV→LV | **GPIO18** | ESP TX |
| 5 (3.8EN / PWRKEY*) | — | **GPIO4** | Power-on controlado |
| 6 (NET) | — | NC ou LED | Status só |
| 7 (DTR) | — | NC | Opcional depois |

\*Confirmar no esquemático do módulo se o pino 5 é EN da LDO 3,8 V ou PWRKEY;
o GPIO4 serve para o controle de power que a placa de fato expuser.

### SD (SPI 3V3) — SPI2/HSPI-like

| Módulo | ESP32 | Notas |
|---|---|---|
| MISO | GPIO19 | |
| MOSI | GPIO23 | |
| CLK | GPIO21 | |
| CS | GPIO22 | Dedicado |
| VCC | 3V3 | |
| GND | GND | |

### CAN (SN65HVD230) — TWAI

| Transceptor | ESP32 | Função no ESP |
|---|---|---|
| RX | **GPIO35** | TWAI RX (input-only bem empregado) |
| TX | GPIO27 | TWAI TX |
| VCC | 3V3 | |
| GND | GND | |

### LLC (WCMCU-0108)

- VA ⇒ 1,8 V (A7670)
- VB ⇒ 3,3 V (ESP)

## Mapa compacto ESP32

| GPIO | Uso |
|---|---|---|
| 4 | A7670 EN / PWR |
| 13 | LoRa AUX |
| 18 | GPRS TX (ESP → RXD) |
| 19 | SD MISO |
| 21 | SD CLK |
| 22 | SD CS |
| 23 | SD MOSI |
| 25 | LoRa TX (ESP → E32 RX) |
| 26 | LoRa RX (ESP ← E32 TX) |
| 27 | CAN TX |
| 32 | LoRa M0 |
| 33 | LoRa M1 |
| 34 | GPRS RX (ESP ← TXD) |
| 35 | CAN RX |

## Livres (úteis depois)

GPIO0, 2, 5, 12, 14, 15, 16, 17 — preferir não amarrar strapping (0/2/5/12/15)
sem pull explícito. GPIO14/16/17 são bons para LED de status, spare UART, etc.

## Por que muda tanto vs a proposta atual

| Tema | Proposta HW | Ideal |
|---|---|---|
| A7670 RXD | GPIO35 (inválido) | GPIO18 |
| CAN RX | GPIO14 | GPIO35 (aproveita input-only) |
| LoRa M0/M1/AUX | 15 / 17 / 5 (2 strapping) | 32 / 33 / 13 |
| LoRa UART | 16 / 4 | 25 / 26 (par clássico UART2) |
| SD CS | ausente | GPIO22 |
| SD MOSI | 22 | 23 (CS em 22) |
| A7670 EN | None | GPIO4 |
