# AV3 – Horta Inteligente: Rede IoT com ESP-NOW + MQTT + Node-RED

```
 [Sensor 1] ))) broadcast )))  [Repetidor 1] ))) broadcast )))
 [Sensor 2] )))   ESP-NOW  )))  [Repetidor 2] )))   ESP-NOW  )))  [ESP32 A] ==UART2==> [ESP32 B] --Wi-Fi/MQTT--> Broker --> Node-RED
 [Sensor N] )))            )))  [Repetidor N] )))            )))  (receptor)  115200   (bridge)                  (dashboard + log CSV)
```

**Mesh por inundação (flooding), sem MAC configurado em lugar nenhum.** O sensor manda em
broadcast (`FF:FF:FF:FF:FF:FF`). Todo repetidor que ouve repassa a mensagem **uma vez** em broadcast,
com `hop + 1`. Cada placa lembra o par `(id, cont)` das últimas mensagens e ignora cópias,
e nada é repassado depois de `MAX_SALTOS` (4) saltos. O Gateway A também descarta cópias e manda
cada mensagem uma única vez ao B. O `id` de cada mensagem é o **MAC do sensor de origem**, e o
campo `rep` no JSON diz quem entregou a cópia que chegou primeiro ao Gateway A.

Dá para trocar qualquer ESP32 por outro sem mudar nada no código. Todas só precisam estar no
mesmo `CANAL_ESPNOW`.

Em broadcast o ESP-NOW não confirma o recebimento (não tem ACK). Por isso a piscada do LED só
indica que a mensagem saiu pelo rádio. A entrega de verdade é medida no fim da cadeia, com o PDR
calculado pelo Node-RED e pelo `calcular_pdr.py`.

| Pasta | O que é |
|---|---|
| `firmware/00_descobrir_mac` | Imprime o MAC da placa (opcional: só para etiquetar e identificar as placas) |
| `firmware/01_sensor` | Sensor Inteligente: lê v1/v2, envia em broadcast, LED de status, **teste de estresse no botão BOOT** |
| `firmware/02_repetidor` | Repetidor de flooding: ouve broadcast, descarta cópias, `hop++`, retransmite em broadcast |
| `firmware/03_gateway_A_receptor` | Ouve broadcast, descarta cópias → `Serial2.println(json)` |
| `firmware/04_gateway_B_mqtt` | Lê UART2, ignora linhas sem `{`, publica JSON (ArduinoJson) com LWT e reconexão |
| `node-red/flow_cps.json` | Flow pronto para importar (dashboard, gráficos, tabela com PDR, log CSV, status) |
| `node-red/gerar_flow.py` | Gera o flow já com o nome da equipe/broker: `python3 gerar_flow.py equipe3` |
| `ferramentas/calcular_pdr.py` | Calcula o PDR de cada rodada do teste a partir do CSV |

## 1. Preparar a IDE Arduino
- Placa: **ESP32 Arduino (Espressif)** → **DOIT ESP32 DEVKIT V1**. Funciona com core 2.x e 3.x (testado no 3.3.11).
- Bibliotecas: **PubSubClient** (Nick O'Leary), **ArduinoJson** 7.x (Benoit Blanchon), **DHT sensor library** + **Adafruit Unified Sensor** (só se usar DHT22).

## 2. MACs: não precisa configurar
Não precisa preencher MAC em nenhum firmware: é só gravar o firmware certo em cada placa.
Cada placa imprime o próprio MAC no Monitor Serial (115200) ao ligar. Anotar esse MAC e
etiquetar as placas é opcional, mas ajuda a reconhecer o `id` (sensor) e o `rep` (quem entregou)
no painel.

**Teste de alcance do repetidor:** com o sensor perto, o Gateway A ouve o sensor direto e o
repetidor fica de fora do caminho. Para obrigar as mensagens a passarem pelo repetidor, grave o
Gateway A com `#define ACEITAR_DIRETO 0`. Assim ele só aceita mensagens com `hop >= 1`.

## 3. Sensores (Horta Inteligente)
| Campo | Grandeza | Sensor | Ligação no ESP32 |
|---|---|---|---|
| `v1` | Luminosidade recebida pela planta (%) | LDR (módulo ou LDR + resistor 10k) | saída analógica → **GPIO 34**, VCC 3,3 V |
| `v2` | Altura da planta (cm) = `ALTURA_SENSOR_CM` − distância | Ultrassom HC-SR04, preso acima do vaso apontando para baixo | TRIG → **GPIO 5**, ECHO → **GPIO 18** (com divisor 1k/2k se for HC-SR04 de 5 V), VCC 5 V (VIN) |

- **Por enquanto está em modo simulado** (`#define TIPO_SENSOR SENSOR_SIMULADO`): a luz sobe e desce num "dia" de 10 min
  e a planta "cresce" 0,2 cm/min a partir de 8 cm. Serve para validar toda a comunicação sem ligar sensor nenhum.
- Para usar os sensores de verdade: `SENSOR_REAL`, e meça com régua a distância do ultrassom até a terra do vaso → `ALTURA_SENSOR_CM`.
- Se a luz aparecer invertida (mais luz = número menor), mude `LDR_INVERTIDO` para 1.
- O LDR tem que ficar num pino do ADC1 (32 a 39): o ADC2 não funciona enquanto o rádio (ESP-NOW) está ligado.

## 4. Gateway (2 ESP32)
Ligação: **A.GPIO17 (TX) → B.GPIO16 (RX)**, **A.GPIO16 (RX) ← B.GPIO17 (TX)**, **GND ↔ GND**.
Em `04_gateway_B_mqtt.ino` configure `EQUIPE`, `WIFI_SSID`, `WIFI_SENHA` e o broker (padrão `broker.hivemq.com:1883`).
- Tópicos: `cps/<equipe>/sensor/<MAC do sensor>` e `cps/<equipe>/gateway/status` (`online`/`offline`, retido, LWT).
- O A envia também `hop`, `rep` (repetidor que entregou) e `rssi` além de `id, cont, v1, v2`.
- O LED do B: aceso = tudo ok; pisca rápido = sem Wi-Fi; pisca devagar = sem MQTT.
- Por que o A não usa Wi-Fi? Assim a rede ESP-NOW fica sempre no canal 1, independentemente do canal do roteador.

## 5. Node-RED
1. Precisa do `node-red-dashboard` (Menu → Manage palette → Install).
2. `python3 node-red/gerar_flow.py equipe3` (ou edite `equipeX` nos 2 nós *mqtt in*) → Menu → Import → `flow_cps.json` → Deploy.
3. Painel em `http://localhost:1880/ui` (aba **Horta Inteligente**): indicador do gateway; tabela por planta (luz, altura, **quanto cresceu** desde a primeira leitura, hop, RSSI, recebidas/esperadas, **PDR ao vivo**); gráfico de **luminosidade (%)** e gráfico de **altura da planta (cm)**, com 1 linha por sensor.
4. Log de auditoria: `~/cps_log_<equipe>.csv` (Windows: `C:\Users\<você>\`). Colunas: `ts_iso,ts_ms,id,cont,v1,v2,hop,rep,rssi`; o horário é o do servidor Node-RED na recepção.
- Requisitos atendidos: nó **JSON** antes dos gráficos ✔, timestamp do servidor ✔, tempo real por sensor ✔, histórico ✔, log em arquivo ✔, Online/Offline ✔.

## 6. Plano de testes
**Funcional:** ligue tudo e confira no painel que `cont` sobe, os gráficos de luz e altura se mexem de 1 em 1, `hop = 1`, e o PDR fica ~100%. Desligue o ESP32 B (ou o roteador) e veja o status ir para **OFFLINE** em ~20 s (LWT); religue e veja voltar.

**Estresse e alcance** (power banks nos sensores e repetidores):
1. Posicione o par na distância (10 m, 20 m, 30 m … ≥100 m em campo aberto). Marque o ponto no **Google Earth** (alfinete com o nome "10 m", "20 m"…; exporte o KML/print para o relatório).
2. Aperte **BOOT** no sensor → dispara 1000 mensagens (contador 1→1000, intervalo aleatório 100–200 ms, ~2,5 min). LED aceso fixo = acabou.
3. Mude a distância e aperte BOOT de novo. **Anote a ordem das distâncias.**
4. No fim: `python3 ferramentas/calcular_pdr.py ~/cps_log_equipe3.csv --total 1000 --csv resultados.csv`
   Cada rodada é separada automaticamente quando o contador volta a 1.

Modelo para o relatório:

| Rodada | Distância | Local (Google Earth) | Enviadas | Recebidas | PDR (%) | RSSI médio |
|---|---|---|---|---|---|---|
| 1 | 10 m | | 1000 | | | |
| 2 | 20 m | | 1000 | | | |
| … | 100 m | | 1000 | | | |

Dicas: ESP-NOW alcança bem mais em linha de visada com as placas elevadas (~1 m do chão, antenas paralelas). Varie a distância Sensor↔Repetidor mantendo o Repetidor perto do gateway (ou o contrário) e diga no relatório qual enlace foi testado.
