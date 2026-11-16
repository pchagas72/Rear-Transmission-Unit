# Etapa protoboard — plano da sessão

Roteiro de critérios: `testes_protoboard.md`  
Projeto: `firmware/comunicacao/rtu/`  
IDF: `source ~/.espressif/tools/activate_idf_v6.1.sh`  
Porta esperada: `/dev/ttyUSB0` (115200)

---

## Pinagens — expectativa desta etapa

A fiação pode mudar conforme a equipe de hardware. O firmware acompanha; vamos
**testar variantes** na proto, não travar só no min_diff.

| Artefato | Papel |
|---|---|
| `components/board/include/board_pins.h` | **Fonte única** de GPIOs no código (toda mudança de pinagem passa aqui) |
| `connections.md` | **Pinagem ativa** (proposta HW / validação proto) |
| `pinout_min_diff.md` | Diff mínimo histórico (CS=23, A7670 TX=18) — referência |
| `pinout_ideal.md` | Alvo de placa definitiva |
| `relatorio_hw_connections.md` | Resultado dos testes para enviar a HW |
| `testes_protoboard.md` | Critérios + registro; anotar **qual pinagem** foi flashada |

### Fluxo quando HW mudar fios

1. Pedro (ou HW) descreve o mapa novo (módulo → GPIO) ou aponta o `.md` da variante.
2. Agente atualiza **só** `board_pins.h` (+ tabela no roteiro/registro se pedido).
3. Checagem rápida de restrições ESP32 antes do flash: não usar 6–11; 34–39 só input; strapping 0/2/5/12/15 com cuidado; UART TX/RX cruzados; SD com CS dedicado.
4. `build flash monitor` e reexecutar a Fase B (no mínimo #1 Boot/FSM + blocos afetados).
5. No registro: Pass/Fail **por pinagem** (ex.: `min_diff` vs `HW-v2`), não misturar resultados.

Se duas pinagens forem comparadas na mesma sessão, flashar uma, anotar, só então trocar fios + `board_pins.h` e repetir.

---

## Fase A — agora (Pedro)

1. Conferir conexões físicas vs pinout **ativo** (**connections.md** / `board_pins.h`).
2. Preencher no `testes_protoboard.md` o que couber **antes** do flash.
3. Ligar a protoboard no chicote / USB e avisar o agente para correr os testes.

---

## Fase B — quando Pedro pedir para correr testes (agente)

Ordem fixa (igual ao roteiro):

| # | Ação do agente | Como valida | Onde anota |
|---|---|---|---|
| 0 | `source` IDF → `idf.py build flash monitor` em `firmware/comunicacao/rtu` | build ok, porta sobe | nota de commit/hash no registro |
| 1 | Boot / FSM | logs (`TWAI`, E32 init, `tasks dispatched`, FSM → RUNNING) + LED (1 ou 2 pulsos; não estrobo) | registro #1 |
| 2 | SD | log `SD mounted` / `LOG….BIN`; após ~30 s, arquivo > 0 se cartão presente | registro #2 |
| 3 | LoRa TX | ausência de `AUX stuck`; se houver 2º E32, frames `AA BB CC DD` + 52 B | registro #3 |
| 4 | LoRa RX → CAN | frame binário `A5 5A` (`rtu_cmd_protocol.h`); log `op …`; frame no sniffer | registro #4 |
| 5 | CAN RX → pacote | injetar IDs legado; LoRa deixa de ser zero; link-down com silêncio | registro #5 |
| 6 | Integração | 2–3 min: LoRa + SD + um comando sem derrubar TX | registro #6 |
| 7 | A7670 | só se HW montado: stub no log, sem hang | registro #7 ou N/A |
| 8 | Reset ×5 | power-cycle / reset; 5/5 sobe até SELF_CHECK/RUNNING | registro #8 |

### Regras de operação (agente)

- Ativar IDF e trabalhar só em `firmware/comunicacao/rtu/` (não no symlink legado).
- Mudança de pinagem: só sob pedido (ou mapa HW explícito); editar `board_pins.h` e alinhar docs/registro; não espalhar `#define` de GPIO em outros arquivos.
- Outros patches de código: só com pedido explícito; se falhar, diagnosticar e propor.
- Preencher Pass/Fail + notas no **Registro de sessão** de `testes_protoboard.md` (data / operador / hash / **label da pinagem**).
- Em bancada sem FCU/RCU: **2 pulsos LED = limp esperado** → ainda Pass no #1 se RUNNING.
- A7670: não exigir MQTT; stub basta.
- Se faltar HW para um item (ex.: sem 2º E32, sem sniffer CAN), marcar **Skip** com motivo, não Fail.

### Comandos base (Fase B)

```bash
source ~/.espressif/tools/activate_idf_v6.1.sh
cd firmware/comunicacao/rtu
idf.py -p /dev/ttyUSB0 build flash monitor
```

---

## Estado da sessão

| Campo | Valor |
|---|---|
| Fase atual | **B** — SD Pass; LoRa Pass; CAN/cmd pendentes |
| Pinagem ativa no código | **connections.md** (`board_pins.h`; LoRa ESP TX=4 RX=16) |
| Variantes a testar | remap A7670 TX; revisar fiação SD |
| Proto no chicote | USB `/dev/ttyUSB0` |
| Flash / monitor | **Pass** (2026-11-02) |
| Registro | `testes_protoboard.md` + `relatorio_hw_connections.md` |
