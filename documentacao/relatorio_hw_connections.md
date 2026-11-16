# Relatório RTU — pinout `connections.md` (para Hardware)

**Firmware:** `firmware/comunicacao/rtu`  
**Pinagem flashada:** `connections.md` via `board_pins.h`  
**Data:** 2026-11-02  
**Operador:** Pedro / agente bancada  

## Diff relevante vs restrições ESP32-WROOM-32

| Item | connections.md | Problema | Ação pedida a HW |
|---|---|---|---|
| A7670 `RXD` (ESP→modem) | **GPIO35** | GPIO34–39 são *input-only*; ESP **não** gera UART TX | Remapear para GPIO de saída (**não** GPIO18 — SD CS). Ex.: 25/26/32/33 |
| SD `CS` | **GPIO18** (bancada) | — (fiação corrigida 2026-11-02) | OK: mount + `LOG0002.BIN` |
| A7670 `VCC` / `3.8EN` | `?` / None | Fonte e power-on abertos | Fechar rail ≥2 A e pino EN/PWR se a placa expuser |
| LoRa UART | Doc original TX/RX | Fiação física invertida vs doc antigo | Firmware bancada: **ESP TX=4 RX=16**; `connections.md` alinhado |

Demais pinos (LoRa M0/M1/AUX 15/17/5, CAN 14/27, SD 19/22/21, A7670 RX 34) coerentes com WROOM-32.

## Build / flash

| Item | Status |
|---|---|
| Build (`ninja` em `build/`) | **Pass** (2026-11-02) |
| Flash `/dev/ttyUSB0` | **Pass** (2026-11-02) |

## Tabela de funcionamento (protoboard + estação)

| # | Item | Status | Nota |
|---|---|---|---|
| 1 | Boot / FSM → `RUNNING` | **Pass** | Limp sem FCU/RCU = esperado |
| 2 | SD log (`LOG*.BIN`) | **Pass** | Mount OK @400 kHz; SDSC 1910MB; `/sdcard/LOG0002.BIN` (fiação corrigida) |
| 3 | LoRa TX telemetria | **Pass** | Revalidado 2026-11-02 18:25 só pela estação (RTU sem USB) |
| 4 | Estação LoRa RX + print | **Pass** | ~35 s: seq 48→54+, ok≥7 bad=0, CHAN=0x06, CRC ok; campos zero (sem FCU/RCU) |
| 5 | LoRa RX cmd → CAN | **Pendente** | Parser binário `A5 5A` ativo; sem sniffer/nó CAN |
| 6 | CAN RX → campos no pacote | **Pendente** | TWAI up @500k; sem peers → campos zero |
| 7 | Integração LoRa+CAN+SD | **Pendente** | Bloqueado por #2 / #5 / #6 |
| 8 | GPRS A7670 (RF / rede) | **Pass (HW)** | Confirmado por Pedro a céu aberto (teto Al do galpão bloqueia) |
| 9 | GPRS no firmware RTU | **Stub** | Sem AT/MQTT; `a7670_ready()=false`; GPIO35 inválido p/ ESP→modem |
| 10 | LEDs CAN / LoRa (GPIO2 / 25) | **Pass** | Heartbeat ≈ OK; estrobo = fail |

## Registro alinhado a `testes_protoboard.md`

| # | Teste | Pass/Fail/Skip | Nota |
|---|---|---|---|
| 1 | Boot / FSM | **Pass** | SELF_CHECK → RUNNING (limp) |
| 2 | SD log | **Pass** | `SD mounted` + `log file: /sdcard/LOG0002.BIN` |
| 3 | LoRa TX | **Pass** | Estação recebe pacotes |
| 4 | LoRa RX → CAN | **Skip** | Sem nó/sniffer CAN na bancada |
| 5 | CAN RX → pacote | **Skip** | Sem peers FCU/RCU |
| 6 | Integração | **Skip** | Depende de SD + CAN |
| 7 | A7670 stub | **Pass (stub)** | Warning GPIO35; sem hang. RF GPRS ok outdoor (HW) |
| 8 | Reset ×5 | **Pendente** | |

## Logs-chave observados (2026-11-02)

- `Rear Transmission Unit — pinout connections.md, sem GPS local`
- `TWAI running TX=27 RX=14 @500k`
- `Init M0=15 M1=17 AUX=5 UART TX=4 RX=16`
- `radio ADD=0000 SPED=0x1A CHAN=0x06 OPT=0x44`
- `SD SPI CS=18 MOSI=22 CLK=21 MISO=19`
- `SD mounted` / Type SDSC Size 1910MB / `log file: /sdcard/LOG0002.BIN`
- `A7670` stub + warning GPIO35
- `FSM: → RUNNING (limp)`

## Conclusão (para HW)

1. **SD OK** após correção de fiação — CS18 MOSI22 CLK21 MISO19, FAT32, `LOG*.BIN`.
2. **A7670 RXD ≠ GPIO35** — remapear saída (não usar 18 = SD CS); driver AT ainda não integrado.
3. **LoRa OK** (TX RTU ↔ RX estação) com UART ESP TX=4 RX=16 e CHAN=`0x06`.
4. GPRS RF validado outdoor; indoor galpão Al = skip esperado.
