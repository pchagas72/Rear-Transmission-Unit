# Guia da RTU para aspirantes

Este texto explica **o que esta placa faz** e **como o código está organizado**, sem assumir que você já conhece ESP-IDF ou FreeRTOS. Leia com calma e use os arquivos citados como mapa.

> **RTU** = *Rear Transmission Unit* — unidade de transmissão traseira.  
> Ela **não** mede a maioria dos sensores do carro. Ela **recebe** dados das outras ECUs (FCU/RCU) pelo barramento **CAN**, junta tudo num pacote e **repassa** por Wi‑Fi (SoftAP), LoRa e cartão SD.

Documentação técnica e roteiros de teste: pasta [`documentacao/`](documentacao/)  
(ex.: `architecture.md`, `connections.md`, `relatorio_hw_connections.md`, `testagem_no_chicote.md`).

---

## 1. Ideia em uma frase

```
FCU/RCU  --CAN-->  RTU (ESP32)  --+--> SoftAP (celular no pit)
                                  +--> LoRa (estação / rádio)
                                  +--> SD (log .BIN)
                                  +--> (futuro) 4G A7670
```

A RTU é um **hub**: porta de entrada CAN + saídas de telemetria/comando.

---

## 2. O que você precisa saber antes (mini glossário)

| Termo | Significado simples |
|---|---|
| **ESP32** | Microcontrolador com Wi‑Fi (e Bluetooth). Aqui roda o firmware. |
| **FreeRTOS** | Sistema que deixa várias “tarefas” rodarem em paralelo (como threads). |
| **Task** | Função em loop infinito com um trabalho específico (ex.: só LoRa TX). |
| **CAN** | Rede do carro: mensagens curtas com ID (ex.: `0x304` = RPM). |
| **TWAI** | Nome do periférico CAN dentro do ESP32. |
| **UART** | Comunicação serial ponto a ponto (LoRa E32, modem A7670). |
| **SPI** | Barramento usado no cartão SD. |
| **SoftAP** | A RTU cria um Wi‑Fi próprio (`Mangue-RTU`); você conecta e abre `http://192.168.4.1/`. |
| **LoRa E32** | Rádio de longo alcance (módulo Ebyte). |
| **Pacote** | Struct fixa de **52 bytes** (`telemetry_packet_t`) enviada no ar / gravada no SD. |
| **FSM** | Máquina de estados: Boot → Self-check → Running → Error. |
| **Limp** | Modo degradado: continua rodando, mas avisa que algo falhou (ex.: LoRa ou ECU sumiu). |
| **NVS** | Memória não volátil pequena (conta boots, último reset). |

---

## 3. Pasta do projeto (onde olhar)

Você está na raiz da placa: `firmware/comunicacao/rtu/`.

```
rtu/
├── main/                      ← “cérebro” da aplicação
│   ├── app_main.c             ← sobe Wi‑Fi, inicia drivers, cria as tasks
│   ├── system_fsm.c           ← estados Boot/Self/Running/Error
│   ├── telemetry_agg.c        ← car_state → pacote de telemetria
│   ├── can… (via component)   ← ver components/can_mgmt
│   ├── http_telem.c           ← página SoftAP + APIs JSON
│   ├── wifi_ap.c              ← cria a rede Wi‑Fi
│   ├── rtu_diag.c             ← diagnóstico LOC/DTC/QF por canal
│   ├── rtu_health.c           ← health (heap, boot, LoRa ok/fail…)
│   ├── cmd_parser.c           ← opcodes de comando → frames CAN
│   ├── telem_store.c          ← último pacote para o SoftAP ler
│   └── status_led.c           ← LEDs de saúde CAN / LoRa
├── components/
│   ├── board/                 ← pinos (board_pins.h) e IDs CAN (can_ids.h)
│   ├── can_mgmt/              ← driver TWAI + estado agregado do carro
│   ├── ebyte_e32/             ← driver do módulo LoRa
│   ├── sd_log/                ← monta SD e grava LOG####.BIN
│   └── a7670_mqtt/            ← stub do 4G (ainda não integrado de verdade)
├── partitions.csv             ← quanto da flash 4 MB vai para o app
├── README.md                  ← este arquivo (guia do aspirante)
└── documentacao/              ← demais .md + pdfs/
    ├── architecture.md
    ├── testagem_no_chicote.md
    └── pdfs/
```

Pacote compartilhado com outras placas/estação:  
`firmware/shared/telemetry_packets.h` (não aumente o tamanho sem alinhar todo mundo).

---

## 4. Fluxo ao ligar a placa (`app_main`)

Arquivo: `main/app_main.c`, função `app_main()`.

Ordem aproximada:

1. Zera runtime (`rt`), LED debug começa **ligado**.
2. `telem_store_init()` — reserva espaço do “último pacote”.
3. `rtu_diag_init()` — zera canais de diagnóstico.
4. `wifi_ap_start()` — sobe SoftAP + NVS.
5. `rtu_health_init()` — incrementa contador de boot na NVS.
6. `http_telem_start(...)` — servidor HTTP na porta 80.
7. `status_led_init()`, `fsm_init()`, `can_init()`, `e32_init()`.
8. Cria filas FreeRTOS (telemetria, SD, trigger LoRa, mutex de ACK).
9. Cria as **tasks** (ver seção 5).

Se a fila/mutex falhar na alocação, a placa reinicia sozinha.

---

## 5. As tasks (o “dia a dia” do firmware)

Cada task é um `while (1)` com um foco. Prioridade maior = “furar a fila” quando o núcleo está ocupado.

| Task | Core | Prio | Arquivo / onde | O que faz |
|---|---|---|---|---|
| `can_svc` | 0 | 6 | `app_main.c` | A cada **~20 ms**: lê CAN, atualiza diagnóstico e FSM, monta pacote, alimenta SoftAP e (se Running) SD; reenvia LED debug |
| `lora_tx` | 0 | 4 | `app_main.c` | A cada **~1 s**: pega o pacote e manda no E32 (respeitando quiet-time após RX) |
| `lora_rx` | 1 | 5 | `app_main.c` | Lê bytes UART do E32; monta frame `A5 5A…`; vira comando CAN |
| `sd_log` | 1 | 2 | `app_main.c` + `sd_log` | Consome fila e grava no cartão |
| `gprs_svc` | 1 | 2 | `app_main.c` + `a7670_mqtt` | **Stub** — ainda não publica MQTT de verdade |

### 5.1 `can_svc` passo a passo

Função `can_svc_task()`:

1. `can_update_state(&car)` — drena frames TWAI e preenche `car_state_t` (RPM, velocidade, temps…).
2. `rtu_diag_tick(...)` — para cada canal (FCU, RCU, SPEED, …): atualiza miss/hit, DTC, QF.
3. `fsm_tick(...)` — decide se estamos em Self-check / Running / Error / limp.
4. `status_led_tick(...)` — pisca LEDs conforme CAN e AUX do LoRa.
5. `telemetry_fill(&pkt, &car, ...)` — copia estado → pacote padrão.
6. `telem_store_update(&pkt)` — SoftAP sempre vê dado fresco (mesmo com LoRa em limp).
7. Se FSM ok → manda cópia para a fila do SD.
8. De tempos em tempos, rebroadcast do comando LED `0x702` no CAN.

### 5.2 LoRa TX / RX

- **TX:** `lora_tx_task` — período `LORA_TX_PERIOD_MS` (1000). Antes de transmitir, espera o rádio “quieto” depois de um comando recebido (AUX). Conta ok/fail em `rtu_health_note_lora`.
- **RX:** `lora_rx_task` — máquina de estados byte a byte (`ST_SYNC0` …). Protocolo em `firmware/shared/rtu_cmd_protocol.h`. Comando válido → `cmd_parse_opcode` → `cmd_dispatch` → `can_post` / `can_post_u8`.

Prioridade: **RX > TX**, para comando da estação não perder para telemetria.

---

## 6. Camadas importantes (funções que você vai achar)

### 6.1 CAN — `components/can_mgmt/`

| Função | Papel |
|---|---|
| `can_init()` | Instala TWAI 500 kbit/s e sobe o driver |
| `can_update_state(car)` | Recebe frames e atualiza `car_state_t` |
| `can_refresh_link_status(...)` | Marca link FCU/RCU; zera/SNA sensores velhos; NTC inválido → `-1` |
| `can_enqueue_tx` / `can_post` / `can_post_u8` | Enfileira/envia comandos (farol, kill, LED…) |
| `can_get_bus_stats` | TEC/REC, estado do barramento, contagem RX |
| `can_is_running` | TWAI em `RUNNING`? |

IDs em `components/board/include/can_ids.h` (ex.: RPM `0x304`, LED debug `0x702`).

### 6.2 Agregação — `telemetry_agg.c`

| Função | Papel |
|---|---|
| `telemetry_fill(pkt, car, led_debug)` | Monta o pacote 52 B |
| `pack_status_flags(...)` | Bits de link/IMU/GPS/LED no campo `flags` |

CRC: `telemetry_crc8` (header compartilhado).

### 6.3 FSM — `system_fsm.c`

| Função | Papel |
|---|---|
| `fsm_init` | Começa em BOOT |
| `fsm_tick(can_ok, e32_ok, sd_ok, fcu_down, rcu_down)` | Transições |
| `fsm_state` / `fsm_is_running` / `fsm_limp` | Consultas |

Regras atuais (resumo):

- CAN obrigatório para ir a Running.
- E32 (LoRa) **opcional**: se cair, entra **limp**, SoftAP continua.
- FCU/RCU “confirmed LOC” também geram limp.

### 6.4 Diagnóstico — `rtu_diag.c`

| Função | Papel |
|---|---|
| `rtu_diag_tick` | Matura falhas (miss) / cura (hit) |
| `rtu_diag_fcu_down_confirmed` / `…_rcu_…` | LOC confirmado? |
| `rtu_diag_json` | JSON do SoftAP `/api/diag` |

Siglas na UI Diag: DTC, QF, LOC, SNA, TEC, REC… (glossário na própria página).

### 6.5 SoftAP — `wifi_ap.c` + `http_telem.c`

| Função / rota | Papel |
|---|---|
| `wifi_ap_start` | Rede `Mangue-RTU` |
| `http_telem_start` | Registra handlers HTTP |
| `GET /` | Página Dados \| Diag |
| `GET /api/json` | Telemetria ao vivo |
| `GET /api/diag` | Canais LOC/QF |
| `GET /api/health` | Heap, boot, reset, SD, LoRa counters |
| `GET /api/sd` | Lista `LOG*.BIN` |
| `GET /api/sd/get?name=…` | Download do log |
| `POST /api/cmd` | Comandos ASCII (`kill`, `start`, `led_on`…) |

Comandos do dock passam por `http_ascii_cmd` → `cmd_parse_opcode` → CAN.

### 6.6 SD — `components/sd_log/`

| Função | Papel |
|---|---|
| `sd_log_init` | SPI + mount FAT em `/sdcard` |
| `sd_log_data` | Escreve pacotes; flush periódico |
| `sd_log_list_json` / `sd_log_open_read` | SoftAP listar/baixar com nome seguro (`LOG####.BIN`) |

### 6.7 LoRa driver — `components/ebyte_e32/`

| Função | Papel |
|---|---|
| `e32_init` | GPIOs M0/M1/AUX + UART + tentativa de config |
| `e32_aux_ready` | AUX alto = rádio pronto |
| `e32_send_struct` | Envia o pacote em modo transparente |

### 6.8 Health — `rtu_health.c`

| Função | Papel |
|---|---|
| `rtu_health_init` | Lê/grava boot count na NVS |
| `rtu_health_note_lora` | Conta TX ok/fail |
| `rtu_health_json` | Resposta de `/api/health` |

Versão de firmware: `rtu_fw.h` (`RTU_FW_VER`, `RTU_FW_SCHEMA`) — **não** muda o tamanho do pacote LoRa.

---

## 7. SoftAP na prática (como testar sem medo)

1. Ligue a RTU.
2. No celular/PC, conecte no Wi‑Fi **Mangue-RTU** (senha padrão do projeto — ver log/`wifi_ap.h`).
3. Abra **http://192.168.4.1/**
4. Aba **Dados**: velocidade, RPM, tensões, temps, roll/pitch em graus, chips de link.
5. Aba **Diag**: health, DTC/QF, lista de logs SD, glossário de siglas.
6. Botões do rodapé mandam comandos CAN (START/KILL/FAROL/DBG).

Se Motor mostrar **SNA**, o NTC está inválido/desconectado (valor negativo ou timeout) — isso é esperado.

---

## 8. O que a RTU *não* faz (ainda / de propósito)

| Item | Situação |
|---|---|
| GPS próprio | Não tem. GPS no pacote vem da **FCU** via CAN. |
| A7670 4G | Hardware parcialmente ok; firmware **stub**; pino TX ESP→modem precisa remap (GPIO35 é input-only). |
| OTA | Partição atual é **uma** app grande (~4 MB chip quase todo na factory). OTA pediria tabela `two_ota`. |
| Medir Hall/CVT/NTC localmente | Isso é da **RCU** (e IMU/RPM da **FCU**). |

---

## 9. Como estudar o código (roteiro sugerido)

1. Abra `can_ids.h` — entenda quem manda o quê.
2. Leia `can_update_state` — switch dos IDs.
3. Leia `telemetry_fill` — como vira o pacote 52 B.
4. Leia o loop de `can_svc_task` — coração do sistema.
5. Abra a página SoftAP (`INDEX_HTML` em `http_telem.c`) e as rotas `/api/*`.
6. Só depois entre em LoRa TX/RX e `cmd_parser`.

---

## 10. Build / flash (bem resumido)

Na pasta desta placa, com ESP-IDF 6.1 ativado:

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Se mudar `partitions.csv`, normalmente precisa **apagar a flash** antes (`erase-flash`), porque a tabela de partições mudou.

---

## 11. Checklist mental antes de “mexer em produção”

- [ ] Alterei o pacote 52 B? → alinhar estação + parser Python + FCU/RCU se necessário.
- [ ] Alterei ID CAN? → alinhar firmware das aquisições.
- [ ] SoftAP cresceu muito? → olhar `% free` no fim do `idf.py build`.
- [ ] Comando novo no dock? → `http_ascii_cmd` + `cmd_parser` + ID em `can_ids.h`.

---

## 12. Onde pedir ajuda

- Dúvida de **pino/fiação**: `documentacao/connections.md` + `board_pins.h`.
- Dúvida de **arquitetura**: `documentacao/architecture.md`.
- Dúvida de **bancada/testes**: `documentacao/testes_protoboard.md` / `documentacao/relatorio_hw_connections.md`.
- Dúvida de **chicote (galpão)**: `documentacao/testagem_no_chicote.md` (PDF em `documentacao/pdfs/`).
- Dúvida de **siglas na UI**: aba Diag → seção Siglas.

Bem-vindo(a) ao time de embutidos — a RTU parece grande, mas o fluxo é sempre o mesmo: **CAN → pacote → SoftAP / LoRa / SD**.
