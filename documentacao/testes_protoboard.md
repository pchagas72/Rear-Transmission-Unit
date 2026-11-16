# RTU — rotina de testes (protoboard)

Placa em protoboard com pinout **`connections.md`** (`board_pins.h`).
Este roteiro valida o **firmware** nos componentes: CAN, LoRa E32, SD e
(opcional / stub) A7670. **Não há GPS nesta placa.**

Projeto: `firmware/comunicacao/rtu/`  
Ativar IDF: `source ~/.espressif/tools/activate_idf_v6.1.sh`

### Pinout (`board_pins.h` / connections.md)

Convenção: pino do **módulo** ⇒ ESP32 (UART já cruzado TX↔RX).

| Bloco | Conexões |
|---|---|
| **LoRa E32** | M0→**15**, M1→**17**, AUX→**5**, RX→**16** (ESP TX), TX→**4** (ESP RX), VCC→VIN, GND |
| **CAN** SN65HVD230 | RX→**14** (TWAI RX), TX→**27** (TWAI TX), VCC→3V3, GND |
| **SD** SPI | MISO→**19**, MOSI→**22**, CLK→**21**, CS→**18**, VCC→3V3, GND |
| **A7670** (via LLC 1,8↔3,3) | TXD→**34** (ESP RX), RXD→**35** (ESP TX — **inválido** no WROOM), GND |
| **LED** | GPIO **2** (onboard) |

Mapa GPIO → função:

| GPIO | Uso |
|---|---|
| 2 | LED status |
| 4 | LoRa RX (ESP ← E32 TX) |
| 5 | LoRa AUX |
| 14 | CAN RX |
| 15 | LoRa M0 |
| 16 | LoRa TX (ESP → E32 RX) |
| 17 | LoRa M1 |
| 18 | SD CS |
| 19 | SD MISO |
| 21 | SD CLK |
| 22 | SD MOSI |
| 27 | CAN TX |
| 34 | GPRS RX (ESP ← A7670 TXD) |
| 35 | GPRS TX doc HW (ESP→RXD) — input-only, não funciona |

### LEDs de bancada — sem precisar do PC

Dois LEDs mostram **CAN** e **LoRa** (não o limp FCU/RCU — peers ausentes são normais na proto).

| LED | GPIO | Significado |
|---|---|---|
| DEBUG LED | **2** | CAN (TWAI up) |
| DEBUG LED SD | **25** | LoRa (E32 AUX ready) |

| Estado FSM | Padrão (cada LED) | Leitura |
|---|---|---|
| **BOOT** | Aceso fixo | Subindo |
| **SELF_CHECK** | Pisca lento (~400/400 ms) | Checando |
| **RUNNING** + link OK | 1 pulso curto / ~2 s | **Esse link está bom** |
| **RUNNING** + link FAIL | Estrobo ~100 ms | **Esse link falhou** |
| **ERROR** | LED2 estrobo; LED25 segue AUX | CAN caiu |

Regra: **heartbeat ~2 s = OK** nesse link; **estrobo = ruim**. Limp FCU/RCU **não** muda o LED.

---

## 0. Pré-requisitos

| Item | Detalhe |
|---|---|
| Alimentação | USB no DevKit + rail 5 V estável se E32 em VIN; A7670 (se ligado) com fonte própria ≥2 A |
| Serial | `idf.py -p /dev/ttyUSB0 monitor` (ou porta equivalente), 115200 |
| Ferramentas | segundo E32 ou estação rádio; módulo CAN (outro ESP/placa) ou analisador; cartão SD FAT32 |
| Firmware | `idf.py build flash monitor` a partir de `firmware/comunicacao/rtu` |

### Checklist visual rápido (antes de energizar)

- [ ] GND comum entre ESP, E32, SD, CAN transceiver, LLC
- [ ] SD em **3V3** (não 5 V)
- [ ] SN65HVD230 em **3V3**; CANH/CANL com terminação 120 Ω se barramento curto de bancada
- [ ] E32: M0=GPIO15, M1=GPIO17, AUX=GPIO5, RX=GPIO16, TX=GPIO4
- [ ] CAN: ESP TX=GPIO27 → transceiver TX; ESP RX=GPIO14 ← transceiver RX
- [ ] SD: MISO19 MOSI22 CLK21 **CS18**
- [ ] A7670 (se presente): TXD→LLC→GPIO34; RXD→GPIO35 no doc (**bloqueio ESP**); stub só

---

## 1. Boot e FSM

**Objetivo:** firmware sobe, tasks disparam, FSM sai de BOOT.

1. Flash + monitor.
2. Procurar no log:
   - `Rear Transmission Unit — pinout min_diff, sem GPS local`
   - `TWAI running TX=27 RX=14 @500k`
   - `Init M0=15 ... UART TX=16 RX=4`
   - `tasks dispatched`
   - `FSM` → `SELF_CHECK` e depois `RUNNING` (se CAN+AUX ok)

| Resultado | Interpretação (também no LED) |
|---|---|
| LED aceso fixo curto → pisca lento | BOOT → SELF_CHECK |
| 1 pulso ~2 s | RUNNING ok |
| 2 pulsos ~2 s | RUNNING limp (sem peers CAN é normal na bancada só) |
| Pisca rápido | ERROR — CAN/AUX |
| Trava em pisca lento | SELF_CHECK — CAN não sobe ou AUX baixo |

**Pass:** LED em heartbeat (1 ou 2 pulsos), não estroboscópio.

---

## 2. Cartão SD

**Objetivo:** montagem SPI + arquivo `LOGnnnn.BIN`.

1. Cartão FAT32 no slot (não exFAT).
2. Reset / power-on.
3. Log esperado:
   - `SD SPI CS=23 MOSI=22 CLK=21 MISO=19`
   - `SD mounted` / `log file: /sdcard/LOG....BIN`
4. Deixar rodar ~30 s em Running.
5. Desligar, ler o cartão no PC: arquivo novo com tamanho > 0 (pacotes `SD_Storage_Packet`).

| Falha | Ação |
|---|---|
| `cartao ausente` | CS18/MISO/MOSI/CLK, 3V3, contato do slot |
| monta mas não grava | formatar FAT32; ver probe no log |
| SD falha mas FSM Running | esperado (SD opcional); telemetria LoRa segue |

**Pass:** arquivo de log criado e crescendo.

---

## 3. LoRa E32 — TX de telemetria

**Objetivo:** pacote 52 B + marker `AA BB CC DD` saindo no ar / UART.

1. Segundo E32 (ou estação) no mesmo canal/addr transparent mode, 9600.
2. RTU em Running (mesmo sem CAN: pacote “zerado” ainda deve TX a cada ~2,5 s).
3. No receptor: frames com prefixo `AA BB CC DD` e 52 bytes; CRC-8 no último byte.

Opcional no log RTU: ausência de `AUX stuck — skip TX`.

| Falha | Ação |
|---|---|
| `AUX stuck` | AUX GPIO5, modo M0/M1=0 (transparent), alimentação E32 |
| Nada no ar | cruzar TX/RX (ESP16→E32 RX, ESP4←E32 TX), GND, VIN |
| Lixo / baud | ambos 9600 8N1 |

**Pass:** receptor recebe pacotes periódicos com marker correto.

---

## 4. LoRa E32 — RX de comandos → CAN

**Objetivo:** frame binário da estação vira CAN + ACK na telemetria.
Spec: `firmware/shared/rtu_cmd_protocol.h` (sync `A5 5A`, CRC-8/ATM).

1. Barramento CAN com pelo menos um nó escutando (outra placa / sniffer) a 500 kbit/s.
2. Pelo E32, enviar frame (sem `\n`):

`A5 5A | opcode | plen | payload… | crc8(opcode||plen||payload)`

| Opcode | Hex | plen | CAN ID | data |
|---|---|---|---|---|
| KILL | `0x01` | 0 | `0x701` | `01` |
| START | `0x02` | 0 | `0x701` | `00` |
| LIGHT_ON | `0x03` | 0 | `0x700` | `01` |
| LIGHT_OFF | `0x04` | 0 | `0x700` | `00` |
| LED_ON / OFF | `0x05` / `0x06` | 0 | `0x702` | `01` / `00` |
| BOX / BOX_OFF | `0x07` / `0x08` | 0 | `0x703` | `01` / `00` |
| COMING_ACK | `0x09` | 0 | `0x704` | `00` |
| LIGHT_BLINK | `0x0A` | 4 | `0x700` | blink DLC5 (`n`,`ms` LE no payload) |

Frames prontos (CRC-8/ATM poly 0x07) — ou `rtu_cmd_pack_*()` na estação:

| Cmd | Bytes (hex) |
|---|---|
| KILL | `A5 5A 01 00 15` |
| START | `A5 5A 02 00 2A` |
| LIGHT_ON | `A5 5A 03 00 3F` |
| LIGHT_OFF | `A5 5A 04 00 54` |
| LED_ON | `A5 5A 05 00 41` |
| LED_OFF | `A5 5A 06 00 7E` |
| BOX | `A5 5A 07 00 6B` |
| BOX_OFF | `A5 5A 08 00 A8` |
| COMING_ACK | `A5 5A 09 00 BD` |
| LIGHT_BLINK n=10 ms=500 | `A5 5A 0A 04 0A 00 F4 01 49` |

3. Log RTU: `op KILL` / `op COMING_ACK` etc. (tag `CMD`).
4. Sniffer CAN: frame correspondente.
5. Próximos pacotes LoRa devem carregar flags/soc de ACK por alguns envios (`pending_ack_ttl`).

**Pass:** comando visto no log **e** no barramento CAN. Strings ASCII **não** são mais aceitas.

---

## 5. CAN — RX de telemetria (agregação)

**Objetivo:** `can_svc` atualiza estado a partir de frames (legado).

Com um gerador/outra ECU, injetar (exemplos):

| ID | Conteúdo mínimo | Campo no pacote LoRa |
|---|---|---|
| `0x304` | rpm u16 LE | `rpm` |
| `0x300` | speed u16 LE | `speed` |
| `0x502` | tensão ×100 u16 LE | `voltage` |
| `0x400` / `0x401` | eng/cvt i16 LE | temps |
| `0x205`–`0x207` | IMU | roll/pitch/acc/dps |
| `0x102` / `0x600` | debug FCU/RCU | states + flags |

1. Enviar frames a ~10–50 Hz.
2. Observar LoRa: campos deixam de ser zero; `FLAG_*_LINK_DOWN` limpa quando o peer está vivo.
3. Sem frames por >1,5 s: limp (`FLAG_FCU_LINK_DOWN` / `FLAG_RCU_LINK_DOWN`).

**Pass:** pacote LoRa reflete os IDs injetados; link-down reage ao silêncio.

---

## 6. Integração LoRa ↔ CAN ↔ SD

**Objetivo:** caminho completo do hub.

1. CAN com dados fictícios + LoRa TX + SD montado.
2. Rodar 2–3 minutos.
3. Verificar:
   - LoRa contínuo (~2,5 s ou trigger pós-comando)
   - SD crescendo
   - Um `light_on` via LoRa aparece no CAN e não derruba o TX de telemetria (quiet time pós-RX)

**Pass:** os três caminhos estáveis juntos; sem `ERROR` na FSM.

---

## 7. A7670 (somente se hardware estiver na proto)

**Estado do firmware:** stub — `a7670_init` só loga pinos; **não** há AT/MQTT.

Checklist HW/firmware atual:

1. Log: `stub — UART1 TX=18 RX=34`
2. Não exigir registro na rede para Running.
3. Quando o driver existir: teste separado (AT `ATE0`, `AT+CPIN?`, MQTT) — fora deste roteiro.

**Pass (hoje):** boot sem hang com ou sem modem alimentado.

---

## 8. Strapping / reset (regressão rápida)

**Objetivo:** GPIO5 (AUX) e GPIO15 (M0) não impedem boot.

1. Power-cycle 5×.
2. Sempre deve entrar em monitor e chegar a SELF_CHECK/RUNNING.
3. Se ocasionalmente cai em download mode / não sobe: revisar nível de M0/AUX no instante do reset (pulls).

**Pass:** 5/5 boots limpos.

---

## Ordem sugerida na bancada

```
1 Boot/FSM  →  2 SD  →  3 LoRa TX  →  5 CAN RX
         ↘  4 LoRa RX→CAN  ↗
                    →  6 Integração
                    →  8 Reset
         (7 A7670 só se montado)
```

---

## Registro de sessão (copiar)

| # | Teste | Pass/Fail | Nota / commit flash |
|---|---|---|---|
| 1 | Boot / FSM | **Pass** | RUNNING (limp) — pinout connections.md |
| 2 | SD log | **Pass** | `SD mounted` SDSC 1910MB; `/sdcard/LOG0002.BIN` (fiação corrigida) |
| 3 | LoRa TX | **Pass** | 2026-11-02 18:25 estação: seq 48–54+ ok/bad 7/0 (RTU sem USB) |
| 4 | LoRa RX → CAN | **Skip** | Sem sniffer/nó CAN |
| 5 | CAN RX → pacote | **Skip** | Sem FCU/RCU |
| 6 | Integração | **Skip** | Bloqueado por SD/CAN |
| 7 | A7670 stub | **Pass** | Stub + warn GPIO35; RF GPRS OK outdoor (Pedro) |
| 8 | Reset ×5 | | |

Data: **2026-11-02**  Operador: **Pedro / agente**  Firmware: `comunicacao/rtu` @ `9a9224a-dirty` (SD init @400 kHz)
