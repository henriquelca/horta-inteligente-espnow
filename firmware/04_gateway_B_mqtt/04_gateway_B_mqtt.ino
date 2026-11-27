/*
 * 04 - GATEWAY, ESP32 "B" (Bridge Internet: Wi-Fi + MQTT)
 * --------------------------------------------------------
 * Lê as linhas JSON vindas do ESP32 "A" pela UART2 (GPIO 16 RX / 17 TX,
 * 115200) e publica cada uma imediatamente em:
 *     cps/<EQUIPE>/sensor/<id_do_sensor>
 *
 * - Ignora linhas que não começam com '{' (lixo de boot do ESP32 "A").
 * - Valida e reserializa o JSON com a biblioteca ArduinoJson (v7).
 * - Last Will and Testament em cps/<EQUIPE>/gateway/status:
 *     ao conectar publica "online" (retido); se cair, o broker publica "offline".
 * - Reconecta Wi-Fi e MQTT automaticamente.
 *
 * LED (GPIO 2):  aceso fixo     = Wi-Fi e MQTT conectados
 *                pisca rápido   = sem Wi-Fi
 *                pisca devagar  = Wi-Fi ok, sem MQTT
 *
 * Bibliotecas (Gerenciador de Bibliotecas da IDE):
 *   - PubSubClient (Nick O'Leary)
 *   - ArduinoJson  (Benoit Blanchon) versão 7.x
 */
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_mac.h>

// ===================== CONFIGURAÇÃO DA EQUIPE =====================
#define EQUIPE        "equipeX"            // ex.: "equipe3"
#define WIFI_SSID     "NOME_DA_REDE"
#define WIFI_SENHA    "SENHA_DA_REDE"
#define MQTT_SERVIDOR "broker.hivemq.com"  // ou o IP do seu Mosquitto
#define MQTT_PORTA    1883
#define MQTT_USUARIO  ""                   // vazio = sem autenticação
#define MQTT_SENHA    ""
// ==================================================================

#define PINO_LED   2
#define RX2        16
#define TX2        17
#define BAUD_UART  115200

const char *TOPICO_STATUS = "cps/" EQUIPE "/gateway/status";

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
String clientId;

char linha[512];
size_t tamLinha = 0;
uint32_t publicadas = 0, ignoradas = 0, perdidasSemConexao = 0;

// ---------------------------- Wi-Fi ----------------------------
uint32_t ultimaTentativaWifi = 0;
void cuidarWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (ultimaTentativaWifi && millis() - ultimaTentativaWifi < 10000) return;
  ultimaTentativaWifi = millis();
  Serial.printf("[WiFi] conectando a %s...\n", WIFI_SSID);
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_SENHA);
}

// ---------------------------- MQTT -----------------------------
uint32_t ultimaTentativaMqtt = 0;
void cuidarMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  if (ultimaTentativaMqtt && millis() - ultimaTentativaMqtt < 3000) return;
  ultimaTentativaMqtt = millis();

  Serial.printf("[MQTT] conectando a %s:%d como %s...\n", MQTT_SERVIDOR, MQTT_PORTA, clientId.c_str());
  const char *usuario = strlen(MQTT_USUARIO) ? MQTT_USUARIO : nullptr;
  const char *senha   = strlen(MQTT_SENHA)   ? MQTT_SENHA   : nullptr;
  // connect(id, user, pass, willTopic, willQos, willRetain, willMessage)
  if (mqtt.connect(clientId.c_str(), usuario, senha, TOPICO_STATUS, 1, true, "offline")) {
    mqtt.publish(TOPICO_STATUS, "online", true);
    Serial.println("[MQTT] conectado; status = online");
  } else {
    Serial.printf("[MQTT] falhou, rc=%d (nova tentativa em 3 s)\n", mqtt.state());
  }
}

// ----------------------------- LED -----------------------------
void cuidarLed() {
  bool wifiOk = WiFi.status() == WL_CONNECTED;
  if (wifiOk && mqtt.connected()) { digitalWrite(PINO_LED, HIGH); return; }
  uint32_t periodo = wifiOk ? 500 : 100;
  digitalWrite(PINO_LED, (millis() / periodo) % 2);
}

// ------------------- processamento de uma linha -------------------
void processarLinha(char *s) {
  while (*s == ' ' || *s == '\r' || *s == '\t') s++;   // tira espaços do início
  size_t n = strlen(s);
  while (n && (s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = '\0';
  if (n == 0) return;
  if (s[0] != '{') { ignoradas++; return; }            // mensagens de boot etc.

  JsonDocument doc;
  DeserializationError erro = deserializeJson(doc, s);
  if (erro || !doc["id"].is<const char *>()) {
    ignoradas++;
    Serial.printf("[UART] JSON inválido (%s): %s\n", erro.c_str(), s);
    return;
  }

  String topico = String("cps/") + EQUIPE + "/sensor/" + doc["id"].as<const char *>();
  char payload[320];
  serializeJson(doc, payload, sizeof(payload));

  if (!mqtt.connected()) { perdidasSemConexao++; return; }
  if (mqtt.publish(topico.c_str(), payload)) {
    publicadas++;
    if (publicadas % 50 == 1) Serial.printf("[MQTT] #%lu %s %s\n", (unsigned long)publicadas, topico.c_str(), payload);
  } else {
    perdidasSemConexao++;
  }
}

void lerUart() {
  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\n') {
      linha[tamLinha] = '\0';
      processarLinha(linha);
      tamLinha = 0;
    } else if (tamLinha < sizeof(linha) - 1) {
      linha[tamLinha++] = c;
    } else {
      tamLinha = 0;   // linha grande demais: descarta
      ignoradas++;
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial2.setRxBufferSize(4096);   // segura as mensagens enquanto reconecta
  Serial2.begin(BAUD_UART, SERIAL_8N1, RX2, TX2);
  pinMode(PINO_LED, OUTPUT);

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char id[32];
  snprintf(id, sizeof(id), "cps-%s-gw-%02X%02X%02X", EQUIPE, mac[3], mac[4], mac[5]);
  clientId = id;

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  mqtt.setServer(MQTT_SERVIDOR, MQTT_PORTA);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(5);

  Serial.printf("\nGATEWAY B. Status em %s, dados em cps/%s/sensor/<id>\n", TOPICO_STATUS, EQUIPE);
}

void loop() {
  cuidarWifi();
  cuidarMqtt();
  mqtt.loop();
  lerUart();
  cuidarLed();

  static uint32_t ultimoResumo = 0;
  if (millis() - ultimoResumo > 30000) {
    ultimoResumo = millis();
    Serial.printf("[resumo] publicadas=%lu ignoradas=%lu perdidas_sem_conexao=%lu wifi=%d mqtt=%d\n",
                  (unsigned long)publicadas, (unsigned long)ignoradas, (unsigned long)perdidasSemConexao,
                  WiFi.status() == WL_CONNECTED, mqtt.connected());
  }
}
