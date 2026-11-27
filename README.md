# AV3 – Rede IoT com ESP-NOW + MQTT + Node-RED

```
 [Sensor 1] --ESP-NOW--> [Repetidor 1] --ESP-NOW--\
                                                   \
 [Sensor 2] --ESP-NOW--> [Repetidor 2] --ESP-NOW----> [ESP32 A] ==UART2==> [ESP32 B] --Wi-Fi/MQTT--> Broker --> Node-RED
                                                   /   (Gateway: receptor)  115200     (Gateway: bridge)          (dashboard + log CSV)
 [Sensor N] --ESP-NOW--> [Repetidor N] --ESP-NOW--/
```

| Pasta | O que é |
|---|---|
| `firmware/00_descobrir_mac` | Imprime o MAC da placa (grave em todas antes de tudo) |
| `firmware/01_sensor` | Sensor Inteligente: lê v1/v2, envia ao repetidor, LED de status, **teste de estresse no botão BOOT** |
| `firmware/02_repetidor` | Ponte transparente: recebe do sensor, `hop++`, envia ao gateway A |
| `firmware/03_gateway_A_receptor` | Recebe ESP-NOW de todos os repetidores → `Serial2.println(json)` |
| `firmware/04_gateway_B_mqtt` | Lê UART2, ignora linhas sem `{`, publica JSON (ArduinoJson) com LWT e reconexão |
| `node-red/flow_cps.json` | Flow pronto para importar (dashboard, gráficos, tabela com PDR, log CSV, status) |
| `node-red/gerar_flow.py` | Gera o flow já com o nome da equipe/broker: `python3 gerar_flow.py equipe3` |
| `ferramentas/calcular_pdr.py` | Calcula o PDR de cada rodada do teste a partir do CSV |

## 1. Preparar a IDE Arduino
- Placa: **ESP32 Arduino (Espressif)** → **DOIT ESP32 DEVKIT V1**. Funciona com core 2.x e 3.x (testado no 3.3.11).
- Bibliotecas: **PubSubClient** (Nick O'Leary), **ArduinoJson** 7.x (Benoit Blanchon), **DHT sensor library** + **Adafruit Unified Sensor** (só se usar DHT22).

## 2. Mapear os MACs
1. Grave `00_descobrir_mac` em cada placa e anote o MAC no Monitor Serial (115200). **Etiquete as placas.**
2. Preencha:
   - `01_sensor.ino` → `MAC_REPETIDOR` = MAC do **repetidor par** desse sensor
   - `02_repetidor.ino` → `MAC_GATEWAY` = MAC do **ESP32 A**
   - O ESP32 B não precisa de MAC.

## 3. Sensores
**Por enquanto o sensor está em modo simulado** (`#define TIPO_SENSOR SENSOR_SIMULADO`): v1 oscila em torno de
25 °C e v2 em torno de 60 %, com um pouco de ruído. Não precisa ligar nada além do USB/power bank — serve para validar
toda a parte de comunicação (ESP-NOW → UART → MQTT → Node-RED).

Quando tiverem o sensor: mude para `SENSOR_DHT22` (dados no GPIO 4; v1 = temperatura, v2 = umidade). Se for outro
sensor, troque só a função `lerSensores()` em `01_sensor.ino` e os rótulos dos gráficos/tabela no Node-RED.

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
3. Painel em `http://localhost:1880/ui`: indicador do gateway, tabela com cada sensor (valores, hop, RSSI, recebidas/esperadas, **PDR ao vivo**) e gráficos históricos de v1 e v2 (1 linha por sensor).
4. Log de auditoria: `~/cps_log_<equipe>.csv` (Windows: `C:\Users\<você>\`). Colunas: `ts_iso,ts_ms,id,cont,v1,v2,hop,rep,rssi`; o horário é o do servidor Node-RED na recepção.
- Requisitos atendidos: nó **JSON** antes dos gráficos ✔, timestamp do servidor ✔, tempo real por sensor ✔, histórico ✔, log em arquivo ✔, Online/Offline ✔.

## 6. Plano de testes
**Funcional:** ligue tudo e confira no painel que `cont` sobe de 1 em 1, `hop = 1`, e o PDR fica ~100%. Desligue o ESP32 B (ou o roteador) e veja o status ir para **OFFLINE** em ~20 s (LWT); religue e veja voltar.

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
