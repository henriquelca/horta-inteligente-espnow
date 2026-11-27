/*
 * 02 - NÓ REPETIDOR  (ESP32 DOIT Devkit V1)
 * -----------------------------------------
 * Repetidor de MESH POR INUNDAÇÃO (flooding) em ESP-NOW broadcast.
 * Não precisa saber o MAC de ninguém:
 *   1. ouve tudo o que chega em broadcast no canal CANAL_ESPNOW;
 *   2. ignora mensagens que já viu (chave = id do sensor + cont);
 *   3. ignora mensagens que já deram MAX_SALTOS saltos;
 *   4. soma 1 no "hop" e retransmite em broadcast.
 * Os demais campos (id = MAC do sensor de origem, cont, v1, v2) seguem intactos.
 * Pode haver quantos repetidores quiser: cada um repassa cada mensagem só uma vez.
 *
 * LED (GPIO 2): piscada curta = retransmitiu; piscada longa = erro ao transmitir.
 * (Em broadcast não existe confirmação de recebimento.)
 */
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>

// ===================== CONFIGURAÇÃO DA EQUIPE =====================
#define CANAL_ESPNOW  1      // mesmo canal em TODOS os nós ESP-NOW
#define MAX_SALTOS    4      // mensagens com hop >= isso não são mais repassadas
#define PINO_LED      2
// ==================================================================

uint8_t MAC_BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

typedef struct __attribute__((packed)) {
  uint8_t  id[6];
  uint32_t cont;
  float    v1;
  float    v2;
  uint8_t  hop;
} Mensagem;

// ---------- memória de mensagens já vistas (evita loop infinito) ----------
// Guarda as últimas CACHE_N mensagens por CACHE_VALIDADE_MS. As cópias
// repetidas chegam em milissegundos, então 5 s é folga de sobra; e o contador
// do sensor só volta a 1 depois de muito mais tempo que isso.
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
// ---------------------------------------------------------------------------

QueueHandle_t fila;   // o callback só enfileira; a decisão e o envio ficam no loop()
volatile uint32_t recebidas = 0, descartadas = 0, txOk = 0, txErro = 0;
volatile int ultimoStatus = -1;
uint32_t repassadas = 0, duplicadas = 0, saltosEsgotados = 0;
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
  if (status == ESP_NOW_SEND_SUCCESS) { txOk++;   ultimoStatus = 0; }
  else                                { txErro++; ultimoStatus = 1; }
}

void setup() {
  Serial.begin(115200);
  pinMode(PINO_LED, OUTPUT);
  fila = xQueueCreate(32, sizeof(Mensagem));
  randomSeed(esp_random());

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
  memcpy(peer.peer_addr, MAC_BROADCAST, 6);
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("Erro ao adicionar o endereço de broadcast como peer");
  }

  uint8_t meu[6];
  esp_read_mac(meu, ESP_MAC_WIFI_STA);
  Serial.printf("\nREPETIDOR (flooding) pronto. Meu MAC: %02X:%02X:%02X:%02X:%02X:%02X (canal %d, max %d saltos)\n",
                meu[0], meu[1], meu[2], meu[3], meu[4], meu[5], CANAL_ESPNOW, MAX_SALTOS);
}

void loop() {
  Mensagem m;
  while (xQueueReceive(fila, &m, 0) == pdTRUE) {
    if (jaVisto(m))          { duplicadas++;      continue; }
    if (m.hop >= MAX_SALTOS) { saltosEsgotados++; continue; }

    m.hop++;  // mais um salto
    // Espera aleatória curta: se houver vários repetidores, eles não
    // retransmitem todos no mesmo instante (o que causaria colisão).
    delay(random(1, 8));
    esp_now_send(MAC_BROADCAST, (uint8_t *)&m, sizeof(m));
    repassadas++;

    if (m.cont % 50 == 0 || m.cont < 5) {
      Serial.printf("repassada %02X:%02X:%02X:%02X:%02X:%02X cont=%lu hop=%u | rx=%lu repass=%lu dup=%lu esgot=%lu desc=%lu tx ok=%lu erro=%lu\n",
                    m.id[0], m.id[1], m.id[2], m.id[3], m.id[4], m.id[5],
                    (unsigned long)m.cont, m.hop, (unsigned long)recebidas,
                    (unsigned long)repassadas, (unsigned long)duplicadas,
                    (unsigned long)saltosEsgotados, (unsigned long)descartadas,
                    (unsigned long)txOk, (unsigned long)txErro);
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
