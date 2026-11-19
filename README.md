# RTU — Rear Transmission Unit

Firmware ESP32 (ESP-IDF 6.1) do hub de telemetria: **CAN → SoftAP + LoRa + SD**.

```
FCU / RCU ──CAN──► RTU ─┬─ SoftAP  Mangue-RTU  → http://192.168.4.1/
                        ├─ LoRa E32            → estação / pit
                        └─ SD LOG*.BIN
```

Pacote canônico: `firmware/shared/telemetry_packets.h` (**52 B**).  
Pinos: `components/board/include/board_pins.h`. Docs extras: [`documentacao/`](documentacao/).

---

## Funcionalidades

| Bloco | O que faz |
|-------|-----------|
| CAN @ 500 kbit/s | Agrega FCU/RCU; envia cmds (kill, start, farol, DBG, box) |
| SoftAP | UI Dados + Diag (LOC/DTC/QF), health, SD download/erase |
| LoRa E32 | TX telem ~1 Hz; RX cmd binário → CAN; burst em ACK |
| SD | Log contínuo `LOG####.BIN` |
| FSM | Boot → Self-check → Running (+ limp se ECU/LoRa falhar) |
| SNA | CVT/NTC inválidos → `-1` (UI mostra **SNA**) |

**Fora do v1:** GPS local (vem da FCU); A7670 4G (stub — GPIO35 inválido p/ TX).

---

## Pinout

Fonte: `board_pins.h` (ESP32-WROOM-32).

### LoRa E32 (UART2) — VCC = **VIN 5 V**

| Sinal | GPIO |
|-------|------|
| ESP TX → E32 RX | 17 |
| ESP RX ← E32 TX | 16 |
| M0 / M1 / AUX | 18 / 5 / 4 |
| GND | GND |

Perfil rádio: ADD=`0000` · SPED=`0x1A` · CHAN=`0x06` · OPT=`0x44`

### CAN (TWAI) — VCC = **3V3**

| Sinal | GPIO |
|-------|------|
| TX → transceiver | 27 |
| RX ← transceiver | 14 |

### SD SPI — VCC = **3V3**

| Sinal | GPIO |
|-------|------|
| MISO / MOSI / CLK / CS | 19 / 22 / 21 / 23 |

### LEDs / A7670 (stub)

| Uso | GPIO |
|-----|------|
| LED CAN (D1) | 2 |
| LED LoRa/AUX (D2) | 25 |
| A7670 RX ← modem | 34 |
| A7670 TX → modem | 35 *(input-only — HW a remapear)* |

<!-- print: foto / diagrama da proto ou PCB -->
![Pinout / montagem](documentacao/prints/pinout.png)

---

## Build e flash

```bash
source ~/.espressif/tools/activate_idf_v6.1.sh   # ou export.sh do IDF 6.1
cd firmware/comunicacao/rtu
idf.py set-target esp32    # só na 1ª vez
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Após mudar `partitions.csv`: `idf.py erase-flash` e flash de novo.

---

## SoftAP — uso

1. Ligar a RTU (USB e/ou VIN).
2. Wi‑Fi **`Mangue-RTU`** (senha/token padrão NVS — tipicamente `manguebaja`; ver Diag → cfg).
3. Abrir **http://192.168.4.1/**

| Aba | Conteúdo |
|-----|----------|
| **Dados** | Velocidade, RPM, V, temps (SNA se sensor off), GPS, chips FCU/RCU |
| **Diag** | Health, tabela LOC/DTC/QF, SD, glossário |
| Dock | START / KILL / FAROL / DBG ON·OFF |

APIs: `/api/json` · `/api/diag` · `/api/health` · `/api/sd` · `/api/cmd` (auth token).

<!-- print: SoftAP Dados -->
![SoftAP — Dados](documentacao/prints/softap_dados.png)

<!-- print: SoftAP Diag -->
![SoftAP — Diag](documentacao/prints/softap_diag.png)

---

## Comandos e pacote

Comandos ASCII (SoftAP / LoRa via estação): `kill`, `start`, `light_on|off`, `led_on|off`, `box`, `box_off`, `coming_ack`.

Frame no ar/USB (estação): `AA BB CC DD` + 52 B + CRC-8/ATM.  
Flags de link/IMU/MLX/GPS/ACK em `telemetry_packets.h`.

<!-- print: pit ou seq no SoftAP ao vivo -->
![Telemetria ao vivo](documentacao/prints/live.png)

---

## Checklist rápido

- [ ] E32 em **5 V**, antena, GND comum  
- [ ] CAN/SD em **3V3**  
- [ ] SoftAP sobe; `/api/json` com `age_ms` baixo  
- [ ] Diag: canais não ficam `confirmed` sem motivo  
- [ ] Estação recebe LoRa (`CHAN=0x06`)  

Roteiros: `documentacao/testes_protoboard.md`, `documentacao/testagem_no_chicote.md`.
