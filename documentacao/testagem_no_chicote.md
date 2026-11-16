# RTU — guia de testes no chicote (aspirante)

**Data alvo:** sessão no galpão com chicote  
**Projeto:** `firmware/comunicacao/rtu/`  
**Público:** aspirante / membro novo do time de embutidos  

> **Regra de ouro:** se precisar **compilar, atualizar ou flashar** qualquer código  
> (RTU, estação LoRa, FCU, RCU…), **não faça sozinha(o)**.  
> **Fale com a coordenadoria para atualização e flash rápido do código.**

Este guia assume o cenário típico de hoje:

- **RTU** montada no chicote com **módulo CAN**  
- SoftAP já no firmware flashado  
- **Estação LoRa** pode estar **desmontada** → testes LoRa ficam para depois (com coordenadoria)

---

## 0. O que a RTU faz (em 30 segundos)

```
FCU / RCU  --CAN 500k-->  RTU (ESP32)  --+--> SoftAP Wi‑Fi (celular)
                                         +--> SD (log LOG*.BIN)
                                         +--> LoRa E32 (estação)   ← se montada
```

A RTU **não mede** a maioria dos sensores. Ela **recebe** dados das outras ECUs, monta um pacote e mostra no celular / grava / manda no rádio.

---

## 1. Segurança e organização

| Faça | Não faça |
|---|---|
| GND comum entre ESP, CAN, SD, E32 | Ligar SD em **5 V** |
| Alimentação estável (USB + rail se E32 em VIN) | Forçar flash / `erase-flash` sem coordenadoria |
| Anotar Pass / Fail / Skip na tabela do final | Inventar pinagem “de memória” |
| Tirar foto do chicote se algo não bater | Mexer em FCU/RCU sem alguém do time |

---

## 2. Pinout da RTU (fonte de verdade = firmware)

> Use **`components/board/include/board_pins.h`**.  
> Docs antigos (`connections.md`) podem estar desatualizados — **confira o `.h`**.

Convenção: pino do **módulo** → GPIO do **ESP32**. UART LoRa já cruzada (TX↔RX).

### 2.1 Resumo rápido

| Bloco | Conexões (firmware atual) |
|---|---|
| **CAN** (TJA1050 / HVD230) | TXD ← **GPIO27**, RXD → **GPIO14**, VCC **3V3**, GND |
| **SD** SPI | MISO **19**, MOSI **22**, CLK **21**, CS **23**, VCC **3V3**, GND |
| **LoRa E32** | M0 **18**, M1 **5**, AUX **4**, ESP TX **17**→E32 RX, ESP RX **16**←E32 TX, VCC **VIN/5V**, GND |
| **LED debug** | GPIO **2** (CAN), GPIO **25** (LoRa/AUX) |
| **A7670** (opcional / stub) | RX ESP **34** ← modem TXD; TX doc **35** (**inválido** no WROOM — skip hoje) |

### 2.2 Checklist visual (antes de energizar)

- [ ] GND comum  
- [ ] CANH/CANL no chicote; terminação 120 Ω se for barramento curto de bancada  
- [ ] SD em **3V3**, CS = **GPIO23**  
- [ ] Se E32 estiver no chicote: M0/M1/AUX/UART conforme tabela acima  
- [ ] USB serial da RTU no PC (normalmente `/dev/ttyUSB0`) **ou** só SoftAP no celular  

---

## 3. SoftAP — como conectar (sem PC)

1. Ligue a RTU (chicote / USB).  
2. No celular, Wi‑Fi **`Mangue-RTU`**.  
3. Senha padrão: **`manguebaja`**.  
4. Abra o navegador: **http://192.168.4.1/**  

| Aba | O que ver |
|---|---|
| **Dados** | Velocidade, RPM, bateria, temps, roll/pitch, chips FCU/RCU/GPS |
| **Diag** | Health, FSM, LOC/DTC/QF, lista SD, **API token**, glossário |

### 3.1 Token de API (importante)

Comandos (START/KILL/FAROL/DBG) e download de SD pedem token.

1. Aba **Diag** → campo **API token**.  
2. Digite `manguebaja` (padrão de fábrica).  
3. Toque **Salvar** (fica no celular).  
4. Volte em **Dados** e use os botões do rodapé.

Sem token → o botão responde **401** (não é bug do chicote).

### 3.2 LEDs de bancada (se existirem na proto)

| LED | GPIO | Leitura prática |
|---|---|---|
| DEBUG CAN | 2 | Heartbeat ~2 s = CAN ok; estrobo = ruim |
| DEBUG LoRa | 25 | Heartbeat ~2 s = AUX ok; estrobo = LoRa ruim |

Sem FCU/RCU no barramento, **limp** é **normal** — SoftAP ainda deve abrir.

---

## 4. Roteiro de testes de hoje

Preencha a coluna **Resultado** com **Pass / Fail / Skip** e uma nota curta.

### T1 — Boot + SoftAP

| Passo | O que fazer | Pass se… |
|---|---|---|
| 1 | Energizar RTU | SoftAP `Mangue-RTU` aparece |
| 2 | Abrir `http://192.168.4.1/` | Página Mangue RTU carrega |
| 3 | Olhar Diag → Health | Aparece `fw`, `boot`, heap, uptime |

**Resultado T1:** ___________  

---

### T2 — Auth (token)

| Passo | O que fazer | Pass se… |
|---|---|---|
| 1 | Sem token (ou token errado), apertar **DBG ON** | Status mostra **401** / unauthorized |
| 2 | Salvar token `manguebaja` | — |
| 3 | Apertar **DBG ON** / **DBG OFF** de novo | Resposta `ok:true` (não 401) |

Se tiver PC na mesma rede SoftAP (coordenadoria):

```bash
# esperado: 401
curl -i -X POST -H 'Content-Type: text/plain' --data led_on http://192.168.4.1/api/cmd

# esperado: 200 + {"ok":true,...}
curl -i -X POST -H 'Content-Type: text/plain' -H 'X-RTU-Token: manguebaja' \
  --data led_on http://192.168.4.1/api/cmd
```

**Resultado T2:** ___________  

---

### T3 — CAN no chicote (prioridade do dia)

**Cenário atual:** RTU + módulo CAN no chicote. Idealmente FCU e/ou RCU também energizadas no mesmo barramento.

| Passo | O que fazer | Pass se… |
|---|---|---|
| 1 | Conferir CANH/CANL + GND entre nós | Sem curto óbvio |
| 2 | SoftAP → **Dados** → chips **FCU** / **RCU** | Ficam verdes (`on`) se a ECU está falando |
| 3 | Observar campos (V, RPM, speed, temps…) | Valores mudam / não ficam todos “zerados eternos” |
| 4 | Com token: **DBG ON** | LED de debug na FCU/RCU (se implementado) reage; SoftAP mostra DBG |

**Notas esperadas:**

- Sem FCU/RCU → chips `off` / limp = **Skip**, não Fail.  
- Motor **SNA** = NTC inválido/desconectado na RCU = pode ser esperado.  
- GPS vem da **FCU**, não da RTU.

**Resultado T3:** ___________  

---

### T4 — Cartão SD

| Passo | O que fazer | Pass se… |
|---|---|---|
| 1 | Cartão **FAT32** (não exFAT) no slot | — |
| 2 | Reset / power-on com FSM em RUNNING | Diag → Health: chip **SD** `on` |
| 3 | Esperar ~30 s | Diag → SD logs lista `LOG*.BIN` |
| 4 | Com token, baixar um arquivo pelo link | Download inicia (ou 401 se token errado) |

Se o mount falhar: anote a mensagem (timeout / cartão / CS). **Não reflash** — fale com a coordenadoria.

**Resultado T4:** ___________  

---

### T5 — LoRa / estação (só se a estação estiver montada)

Hoje a estação pode estar **desmontada**. Nesse caso marque **Skip** e peça ajuda.

> **Fale com a coordenadoria para atualização e flash rápido do código** da  
> `firmware/comunicacao/estacao_lora/` e para montar o E32 da estação.

#### Como montar a estação (quando for o caso)

Projeto: `firmware/comunicacao/estacao_lora/`  
SoftAP da estação: Wi‑Fi **`Mangue-Estacao`** / senha **`manguebaja`** → `http://192.168.4.1/`  
(é **outra** placa — não confunda com `Mangue-RTU`)

Pinout estação (`estacao_lora/.../board_pins.h`):

| Bloco | Conexões |
|---|---|
| **LoRa E32** | M0 **32**, M1 **33**, AUX **13**, ESP TX **25**→E32 RX, ESP RX **26**←E32 TX, VCC VIN, GND |
| **CAN** (opcional na estação) | TX **27**, RX **14**, 3V3, GND |

Canal LoRa alinhado com a RTU: **CHAN = `0x06`**, 9600, modo transparente.

| Passo | O que fazer | Pass se… |
|---|---|---|
| 1 | Estação flashada + E32 no ar | SoftAP `Mangue-Estacao` sobe **ou** serial mostra RX |
| 2 | RTU em RUNNING | Estação recebe pacotes periódicos (seq sobe) |
| 3 | (Avançado) comando binário LoRa `A5 5A …` | RTU loga `CMD` e aparece frame no CAN |

**Não invente frames CRC na mão sem checagem.** Protocolo: `firmware/shared/rtu_cmd_protocol.h`.  
Se precisar enviar comandos LoRa: **Fale com a coordenadoria para atualização e flash rápido do código.**

**Resultado T5:** ___________  

---

### T6 — Integração (meta do dia)

Só faz sentido se T1–T3 (e de preferência T4) passaram.

| Check | OK? |
|---|---|
| SoftAP ao vivo com FCU/RCU | [ ] |
| Auth ok (sem 401 com token certo) | [ ] |
| SD gravando (se cartão presente) | [ ] |
| LoRa ok (se estação montada) | [ ] |

**Resultado T6:** ___________  

---

## 5. O que **não** testar sozinha(o) hoje

| Item | Por quê | Ação |
|---|---|---|
| Flash / erase da RTU | Risco de brick / partição | **Fale com a coordenadoria para atualização e flash rápido do código** |
| Flash da estação LoRa | Estação desmontada / pinout distinto | Idem |
| A7670 4G / MQTT | Firmware ainda **stub** + GPIO35 inválido | Skip + avisar HW/coordenadoria |
| OTA | Ainda não há partição `two_ota` | Skip |
| Mudar token/senha NVS em produção | Pode trancar o SoftAP do time | Só com coordenadoria |

---

## 6. Sinais no SoftAP — leitura rápida

| O que aparece | Significado |
|---|---|
| `LIVE …ms` | Pacote fresco no SoftAP |
| `SEM SINAL` | Sem update recente (CAN parado / RTU reiniciando) |
| Chip FCU/RCU `off` | Sem frames dessa ECU (fiação, ECU off, baud) |
| Motor **SNA** | Temp. motor inválida (NTC) |
| `401` nos botões | Token errado / não salvo |
| Health: `E32` off / limp | LoRa down — SoftAP ainda pode funcionar |
| Health: `SD` off | Cartão / SPI / CS23 |

---

## 7. Tabela de registro (preencha e entregue)

| # | Teste | Pass/Fail/Skip | Nota (1 linha) | Quem |
|---|---|---|---|---|
| T1 | Boot + SoftAP | | | |
| T2 | Auth token | | | |
| T3 | CAN FCU/RCU | | | |
| T4 | SD log / download | | | |
| T5 | LoRa / estação | | | |
| T6 | Integração | | | |

**Observações extras (foto / fio / ECU ligada):**

```
…


```

---

## 8. Contato / escalonamento

1. Problema de **fiação / chicote** → time de hardware + foto.  
2. SoftAP não sobe / página quebrada / token não aceita → **coordenadoria** (possível reflash).  
3. Estação desmontada / LoRa → **Fale com a coordenadoria para atualização e flash rápido do código.**  
4. Dúvida de significado de sigla → aba **Diag → Siglas** na própria UI.

Boa sessão no galpão — foque em **SoftAP + auth + CAN**. LoRa e flash ficam com a coordenadoria quando a estação voltar a montar.
