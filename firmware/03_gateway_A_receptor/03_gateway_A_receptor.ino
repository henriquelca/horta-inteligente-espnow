/*
 * 03 - GATEWAY, ESP32 "A" (Receptor Local)
 * -----------------------------------------
 * Ponto de terminação da rede ESP-NOW (mesh por inundação em broadcast):
 * ouve tudo no canal, DESCARTA as cópias repetidas (a mesma mensagem pode
 * chegar direto do sensor e de novo por um ou mais repetidores) e repassa
 * cada mensagem UMA vez, uma por linha, ao ESP32 "B" pela UART2
 * (GPIO 16 = RX, GPIO 17 = TX, 115200 baud) com Serial2.println().
 * Não precisa saber o MAC de nenhuma placa.
 *
 * Cada mensagem vira uma linha JSON terminada em '\n':
 *   {"id":"24:0A:C4:00:01:10","cont":144,"v1":32.40,"v2":55.50,"hop":1,"rep":"..","rssi":-61}
 * "id"  = MAC do sensor que criou a mensagem (origem);
 * "rep" = MAC de quem entregou a cópia que chegou primeiro (o próprio sensor se hop=0);
 * "rssi" = sinal desse último salto (dBm).
 *
 * Por que dois ESP32? O "A" fica só no ESP-NOW (canal fixo) e o "B" fica só no
 * Wi-Fi. Assim o canal do roteador não interfere na rede ESP-NOW.
 *
 * LIGAÇÃO A <-> B:  A.GPIO17(TX) -> B.GPIO16(RX)
 *                   A.GPIO16(RX) <- B.GPIO17(TX)
 *                   A.GND        -- B.GND
 */
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>

#define CANAL_ESPNOW  1
#define PINO_LED      2
#define RX2           16
#define TX2           17
#define BAUD_UART     115200

// 1 = aceita mensagem que veio direto do sensor (hop 0).
// 0 = só aceita o que passou por pelo menos 1 repetidor. Use 0 no TESTE DE
//     ALCANCE do repetidor, para o gateway não "furar" o caminho ouvindo o
//     sensor diretamente quando ele estiver perto.
#define ACEITAR_DIRETO  1

typedef struct __attribute__((packed)) {
  uint8_t  id[6];
  uint32_t cont;
  float    v1;
  float    v2;
  uint8_t  hop;
} Mensagem;

typedef struct {
  Mensagem m;
  uint8_t  rep[6];
  int      rssi;
} Recebida;

// ---------- memória de mensagens já vistas (remove duplicadas) ----------
#define CACHE_N            64
#define CACHE_VALIDADE_MS  5000
struct Visto { uint8_t id[6]; uint32_t cont; uint32_t ms; bool usado; };
Visto cache[CACHE_N];
int cacheProx = 0;

bool jaVisto(const Mensagem &m) {
  uint32_t agora = millis();
  for (int i = 0; i < CACHE_N; i++) {
    if (cache[i].usado && agora - cache[i].ms < CACHE_VALIDADE_MS &&
        cache[i].cont == m.cont && memcmp(cache[i].id, m.id, 6) == 0) return true;
  }
  memcpy(cache[cacheProx].id, m.id, 6);
  cache[cacheProx].cont = m.cont;
  cache[cacheProx].ms = agora;
  cache[cacheProx].usado = true;
  cacheProx = (cacheProx + 1) % CACHE_N;
  return false;
}
// -------------------------------------------------------------------------

QueueHandle_t fila;
volatile uint32_t total = 0, descartadas = 0;
uint32_t duplicadas = 0, diretasIgnoradas = 0;
uint32_t ledApagarEm = 0;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void aoReceber(const esp_now_recv_info_t *info, const uint8_t *dados, int len) {
  const uint8_t *mac = info->src_addr;
  int rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
#else
void aoReceber(const uint8_t *mac, const uint8_t *dados, int len) {
  int rssi = 0;
#endif
  if (len != sizeof(Mensagem)) { descartadas++; return; }
  Recebida r;
  memcpy(&r.m, dados, sizeof(Mensagem));
  memcpy(r.rep, mac, 6);
  r.rssi = rssi;
  total++;
  if (xQueueSend(fila, &r, 0) != pdTRUE) descartadas++;
}

// float -> texto JSON válido (NaN vira null)
void numJson(char *dst, size_t n, float v) {
  if (isnan(v) || isinf(v)) snprintf(dst, n, "null");
  else snprintf(dst, n, "%.2f", v);
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(BAUD_UART, SERIAL_8N1, RX2, TX2);
  pinMode(PINO_LED, OUTPUT);
  fila = xQueueCreate(64, sizeof(Recebida));

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(CANAL_ESPNOW, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("Erro ao iniciar ESP-NOW. Reiniciando...");
    delay(2000);
    ESP.restart();
  }
  esp_now_register_recv_cb(aoReceber);

  uint8_t meu[6];
  esp_read_mac(meu, ESP_MAC_WIFI_STA);
  Serial.printf("\nGATEWAY A pronto. Meu MAC: %02X:%02X:%02X:%02X:%02X:%02X (canal %d)\n",
                meu[0], meu[1], meu[2], meu[3], meu[4], meu[5], CANAL_ESPNOW);
  Serial.printf("Mesh por broadcast: aceita direto do sensor = %s\n", ACEITAR_DIRETO ? "sim" : "nao");
}

void loop() {
  Recebida r;
  while (xQueueReceive(fila, &r, 0) == pdTRUE) {
    if (!ACEITAR_DIRETO && r.m.hop == 0) { diretasIgnoradas++; continue; }
    if (jaVisto(r.m)) { duplicadas++; continue; }   // cópia de algo já repassado ao B

    char v1[16], v2[16], linha[200];
    numJson(v1, sizeof(v1), r.m.v1);
    numJson(v2, sizeof(v2), r.m.v2);
    snprintf(linha, sizeof(linha),
             "{\"id\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"cont\":%lu,\"v1\":%s,\"v2\":%s,"
             "\"hop\":%u,\"rep\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"rssi\":%d}",
             r.m.id[0], r.m.id[1], r.m.id[2], r.m.id[3], r.m.id[4], r.m.id[5],
             (unsigned long)r.m.cont, v1, v2, r.m.hop,
             r.rep[0], r.rep[1], r.rep[2], r.rep[3], r.rep[4], r.rep[5], r.rssi);

    Serial2.println(linha);   // para o ESP32 "B" (o '\n' delimita a mensagem)
    Serial.println(linha);    // cópia para depuração no Monitor Serial

    digitalWrite(PINO_LED, HIGH);
    ledApagarEm = millis() + 40;
  }
  if (ledApagarEm && (int32_t)(millis() - ledApagarEm) >= 0) {
    digitalWrite(PINO_LED, LOW);
    ledApagarEm = 0;
  }
  delay(1);
}
