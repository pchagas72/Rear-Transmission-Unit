# RTU — arquitetura (firmware definitivo)

Path de build (ASCII, IDF 6.1): `firmware/comunicacao/rtu/`  
(symlink legado: `firmware/comunicação/Rear Transmission Unit` → este diretório).

Pinagem de trabalho: **`connections.md`** (`board_pins.h`).
Lacunas ESP32 (A7670 RXD=GPIO35 input-only): ver `relatorio_hw_connections.md`. SD CS=18.
Pacote LoRa/SD: `firmware/shared/telemetry_packets.h`.
**Sem GPS nesta placa** — posição/velocidade GPS, se existirem no pacote, vêm
via CAN da FCU. UART1 é do A7670.

---

## 1. Papel da placa

Hub de comunicação traseira (não aquisita sensores locais de veículo):

1. Consome frames CAN (RCU/FCU — IDs legado por enquanto).
2. Agrega estado num `telemetry_packet_t` (52 B).
3. Publica por LoRa (E32) e, depois, GPRS/MQTT (A7670 — stub).
4. Persiste no SD o mesmo payload + timestamp.
5. Injeta comandos binários LoRa (`rtu_cmd_protocol.h`; depois MQTT) na CAN.

Referência: `firmware/comunicação/RTU/` (protoboard). Este projeto é a
reescrita limpa.

---

## 2. Arquitetura de arquivos (aplicada)

```
Rear Transmission Unit/
├── architecture.md
├── connections.md / pinout_min_diff.md / pinout_ideal.md
├── CMakeLists.txt                 ← project(rtu)
├── sdkconfig.defaults
├── main/
│   ├── app_main.c                 ← filas + tasks
│   ├── system_fsm.c/.h            ← Boot → Self-check → Running → Error
│   ├── telemetry_agg.c/.h         ← car_state → telemetry_packet_t
│   ├── cmd_parser.c/.h            ← opcode binário → CAN + ACK flags
│   ├── data_types.h               ← filas / runtime (não o wire format)
│   └── CMakeLists.txt
└── components/
    ├── board/include/
    │   ├── board_pins.h           ← GPIOs = min_diff (fonte única)
    │   └── can_ids.h
    ├── can_mgmt/                  ← TWAI + car_state_t
    ├── ebyte_e32/                 ← UART2 + M0/M1/AUX
    ├── sd_log/                    ← SPI SD + probe FAT
    └── a7670_mqtt/                ← stub (UART1 pinos reservados)
```

Shared: `../../../shared` (`telemetry_packets.h`, `headlight_cmd.h`, …).

---

## 3. Runtime: FreeRTOS + FSM

| Camada | Papel |
|---|---|
| FSM | Política: Boot → Self-check → Running (limp) → Error |
| Tasks | I/O paralelo e deadlines |

### Tasks

| Task | Core | Prio | Notas |
|---|---|---|---|
| `can_svc` | 0 | 6 | ~50 Hz; TWAI; FSM tick; preenche telemetria/SD |
| `lora_tx` | 0 | 4 | quiet time pós-RX; seq+CRC |
| `lora_rx` | 1 | 4 | frame `A5 5A` → `cmd_parser` → CAN |
| `sd_log` | 1 | 2 | mount no task; fila pode dropar |
| `gprs_svc` | 1 | 2 | stub até AT/MQTT |

Prioridade de link: **LoRa > SD > GPRS**. Falha de GPRS ≠ ERROR.
Falha de SD ⇒ sem log (limp de armazenamento), não derruba Running.
FCU/RCU link-down ⇒ **limp** (flags no pacote), continua LoRa.

### Sem GPS local

- Não há task UART de GPS.
- `FLAG_GPS_OK` só se frames `CAN_GPS_ID` / `CAN_GPS_SPEED_ID` forem recentes (FCU).
- Não há `can_send_gps` na RTU.

---

## 4. Pinout (`board_pins.h` = connections.md)

| Função | GPIO |
|---|---|
| LoRa TX/RX (ESP) | 16 / 4 |
| LoRa M0 / M1 / AUX | 15 / 17 / 5 |
| A7670 TX/RX (ESP) | **35** / 34 (TX=35 inválido no WROOM) |
| SD MISO/MOSI/CLK/CS | 19 / 22 / 21 / **18** |
| CAN RX/TX | 14 / 27 |
| LED | 2 |

---

## 5. Decisões fechadas

- [x] Pinout ativo = **connections.md** (validação com HW)
- [x] Runtime = **FreeRTOS + FSM**
- [x] Wire format = **`telemetry_packets.h` (52 B)** + cmds `rtu_cmd_protocol.h`
- [x] **Sem GPS nesta placa**
- [x] Estrutura de arquivos aplicada (A7670 stub)

## 6. Ainda aberto

1. VCC / EN do A7670 no HW
2. Remap A7670 RXD (não GPIO35; **não** GPIO18 — ocupado pelo SD CS)
3. Migrar `can_ids` para numeração nova (`ids_can.md`) em lockstep com FCU/RCU
4. MQTT: mesmo binário 52 B ou JSON?
5. Comandos MQTT = mesmo frame binário do LoRa (`rtu_cmd_protocol.h`)?
6. Self-check: exigir SD? (hoje **não**)
