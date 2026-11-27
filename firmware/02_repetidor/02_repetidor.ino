/*
 * 02 - NÓ REPETIDOR  (ESP32 DOIT Devkit V1)
 * -----------------------------------------
 * Ponte transparente: recebe as mensagens do Sensor Inteligente via ESP-NOW,
 * incrementa o campo "hop" e as reencaminha ao Gateway (ESP32 "A") via ESP-NOW.
 * Os demais campos (id, cont, v1, v2) seguem intactos.
 *
 * LED (GPIO 2): piscada curta = encaminhou e o Gateway confirmou (ACK);
 *               piscada longa = Gateway não confirmou.
 */
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>

// ===================== CONFIGURAÇÃO DA EQUIPE =====================
// MAC do ESP32 "A" do Gateway (use o sketch 00_descobrir_mac)
uint8_t MAC_GATEWAY[6] = {0x24, 0x0A, 0xC4, 0x00, 0x00, 0x00};

#define CANAL_ESPNOW  1
#define PINO_LED      2
// ==================================================================

typedef struct __attribute__((packed)) {
  uint8_t  id[6];
  uint32_t cont;
  float    v1;
  float    v2;
  uint8_t  hop;
} Mensagem;

QueueHandle_t fila;   // o callback só enfileira; o envio é feito no loop()
volatile uint32_t recebidas = 0, descartadas = 0, ackOk = 0, ackFalha = 0;
volatile int ultimoStatus = -1;
uint32_t ledApagarEm = 0;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void aoReceber(const esp_now_recv_info_t *info, const uint8_t *dados, int len) {
#else
void aoReceber(const uint8_t *mac, const uint8_t *dados, int len) {
#endif
  if (len != sizeof(Mensagem)) { descartadas++; return; }
  Mensagem m;
  memcpy(&m, dados, sizeof(m));
  recebidas++;
  if (xQueueSend(fila, &m, 0) != pdTRUE) descartadas++;
}

#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 0)
void aoEnviar(const wifi_tx_info_t *info, esp_now_send_status_t status) {
#else
void aoEnviar(const uint8_t *mac, esp_now_send_status_t status) {
#endif
  if (status == ESP_NOW_SEND_SUCCESS) { ackOk++;    ultimoStatus = 0; }
  else                                { ackFalha++; ultimoStatus = 1; }
}

void setup() {
  Serial.begin(115200);
  pinMode(PINO_LED, OUTPUT);
  fila = xQueueCreate(32, sizeof(Mensagem));

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(CANAL_ESPNOW, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("Erro ao iniciar ESP-NOW. Reiniciando...");
    delay(2000);
    ESP.restart();
  }
  esp_now_register_recv_cb(aoReceber);
  esp_now_register_send_cb(aoEnviar);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, MAC_GATEWAY, 6);
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("Erro ao adicionar o Gateway como peer");
  }

  uint8_t meu[6];
  esp_read_mac(meu, ESP_MAC_WIFI_STA);
  Serial.printf("\nREPETIDOR pronto. Meu MAC: %02X:%02X:%02X:%02X:%02X:%02X (canal %d)\n",
                meu[0], meu[1], meu[2], meu[3], meu[4], meu[5], CANAL_ESPNOW);
}

void loop() {
  Mensagem m;
  while (xQueueReceive(fila, &m, 0) == pdTRUE) {
    m.hop++;  // mais um salto
    esp_now_send(MAC_GATEWAY, (uint8_t *)&m, sizeof(m));
    if (m.cont % 50 == 0 || m.cont < 5) {
      Serial.printf("encaminhada %02X:%02X:%02X:%02X:%02X:%02X cont=%lu hop=%u | rx=%lu desc=%lu ack ok=%lu falha=%lu\n",
                    m.id[0], m.id[1], m.id[2], m.id[3], m.id[4], m.id[5],
                    (unsigned long)m.cont, m.hop, (unsigned long)recebidas,
                    (unsigned long)descartadas, (unsigned long)ackOk, (unsigned long)ackFalha);
    }
  }

  uint32_t agora = millis();
  if (ultimoStatus >= 0) {
    digitalWrite(PINO_LED, HIGH);
    ledApagarEm = agora + (ultimoStatus == 0 ? 60 : 600);
    ultimoStatus = -1;
  }
  if (ledApagarEm && (int32_t)(agora - ledApagarEm) >= 0) {
    digitalWrite(PINO_LED, LOW);
    ledApagarEm = 0;
  }
  delay(1);
}
