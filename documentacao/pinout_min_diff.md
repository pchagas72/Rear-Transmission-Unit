# RTU — pinagem mínima (diff sobre `connections.md`)

Objetivo: corrigir só o que impede o firmware de funcionar no ESP32-WROOM-32.
Tudo o mais permanece igual à proposta de hardware.

## Diff vs `connections.md`

| Item | Antes | Depois | Motivo |
|---|---|---|---|
| A7670 `RXD` (ESP → modem) | GPIO35 | **GPIO18** | GPIO34–39 são *input-only*; ESP precisa **sair** UART TX |
| SD `CS` | (ausente) | **GPIO23** | SPI-SD exige Chip Select |

Sem outras mudanças de GPIO.

## Conexões

Convenção: pino do **módulo** ⇒ ESP32. Em UART, o cruzamento TX↔RX já está implícito
(ex.: `RX` do E32 recebe o TX do ESP no GPIO indicado).

### LoRa (Ebyte E32) — inalterado

| Módulo | ESP32 | Notas |
|---|---|---|
| M0 | GPIO15 | Strapping (MTDO) — ok após boot; nível no reset importa |
| M1 | GPIO17 | Livre no WROOM-32 |
| AUX | GPIO5 | Strapping — ok como input após boot |
| RX | GPIO16 | ESP UART TX → RX do módulo |
| TX | GPIO4 | ESP UART RX ← TX do módulo |
| VCC | ESP_VIN | Tipicamente 5 V no DevKit |
| GND | ESP_GND | |

### GPRS (A7670SA)

| Módulo | Caminho | ESP32 | Notas |
|---|---|---|---|
| 1 (VCC) | — | ? | Continua aberto (fonte dedicada ~2 A) |
| 2 (GND) | — | ESP_GND | |
| 3 (TXD) | LLC TX_A1→TX_B1 | **GPIO34** | ESP UART RX (input-only ok) |
| 4 (RXD) | LLC RX_A2→RX_B2 | **GPIO18** | ESP UART TX (**era 35**) |
| 5 (3.8EN) | — | None | Igual à proposta |
| 6 (NET) | — | None | |
| 7 (DTR) | — | None | |

### SD (SPI 3V3)

| Módulo | ESP32 | Notas |
|---|---|---|
| MISO | GPIO19 | Inalterado |
| MOSI | GPIO22 | Inalterado |
| CLK | GPIO21 | Inalterado |
| **CS** | **GPIO23** | **Novo** |
| VCC | ESP_3V3 | |
| GND | ESP_GND | |

### CAN (SN65HVD230) — inalterado

| Transceptor | ESP32 | Função no ESP |
|---|---|---|
| RX | GPIO14 | TWAI RX |
| TX | GPIO27 | TWAI TX |
| VCC | ESP_3V3 | |
| GND | ESP_GND | |

### LLC (WCMCU-0108) — inalterado

- VA ⇒ 1,8 V (lado A7670)
- VB ⇒ 3,3 V (lado ESP)

## Mapa compacto ESP32

| GPIO | Uso |
|---|---|
| 4 | LoRa RX (ESP ← E32 TX) |
| 5 | LoRa AUX |
| 14 | CAN RX |
| 15 | LoRa M0 |
| 16 | LoRa TX (ESP → E32 RX) |
| 17 | LoRa M1 |
| **18** | **GPRS TX (ESP → A7670 RXD)** |
| 19 | SD MISO |
| 21 | SD CLK |
| 22 | SD MOSI |
| **23** | **SD CS** |
| 27 | CAN TX |
| 34 | GPRS RX (ESP ← A7670 TXD) |

## Riscos aceitos de propósito

- GPIO5 e GPIO15 continuam strapping — funcionam, mas o hardware deve garantir
  níveis seguros no reset (ou só acionar M0 depois do boot).
- A7670 sem `3.8EN` no ESP — bring-up depende da fonte/HW.
- VCC do A7670 ainda indefinido.
