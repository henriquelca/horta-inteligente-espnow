/*
 * 01 - NÓ SENSOR INTELIGENTE  (ESP32 DOIT Devkit V1)
 * ---------------------------------------------------
 * HORTA INTELIGENTE: lê luminosidade (v1, %) e altura da planta (v2, cm) e envia via ESP-NOW para o seu
 * Repetidor par. LED local indica o status de cada envio.
 *
 * MODOS
 *  - Normal: envia 1 mensagem a cada INTERVALO_NORMAL_MS (2 s).
 *  - Teste de estresse: aperte o botão BOOT (GPIO 0). O contador volta para 1 e
 *    o nó dispara TOTAL_TESTE (1000) mensagens com intervalo aleatório de
 *    100 a 200 ms. Ao terminar, o LED fica ACESO FIXO e o nó para de enviar.
 *    Aperte BOOT de novo para outra rodada (próxima distância), ou EN/RESET
 *    para voltar ao modo normal.
 *
 * LED (GPIO 2, o LED azul da placa)
 *  - piscada curta (60 ms)  -> Repetidor confirmou o recebimento (ACK ESP-NOW)
 *  - piscada longa (600 ms) -> falha no envio (Repetidor fora de alcance/desligado)
 *  - aceso fixo             -> rodada de teste concluída
 */
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>

// ===================== CONFIGURAÇÃO DA EQUIPE =====================
// MAC do REPETIDOR par deste sensor (use o sketch 00_descobrir_mac)
uint8_t MAC_REPETIDOR[6] = {0x24, 0x0A, 0xC4, 0x00, 0x00, 0x00};

#define CANAL_ESPNOW         1      // mesmo canal em TODOS os nós ESP-NOW
#define INTERVALO_NORMAL_MS  2000
#define TOTAL_TESTE          1000
#define TESTE_MIN_MS         100
#define TESTE_MAX_MS         200
#define PINO_LED             2
#define PINO_BOTAO           0      // botão BOOT da placa

// HORTA INTELIGENTE
//   v1 = luminosidade (% , 0 = escuro, 100 = luz máxima)  -> LDR no GPIO 34
//   v2 = altura da planta (cm) = ALTURA_SENSOR_CM - distância medida pelo ultrassom
//        (HC-SR04 apontado para baixo, sobre a planta: TRIG GPIO 5, ECHO GPIO 18)
//
//   SENSOR_REAL     -> lê o LDR e o HC-SR04
//   SENSOR_SIMULADO -> dados mockados (luz variando e planta "crescendo") para testar a rede
#define SENSOR_REAL      1
#define SENSOR_SIMULADO  2
#define TIPO_SENSOR      SENSOR_REAL   // por enquanto: dados mockados

#define PINO_LDR          34     // tem que ser pino do ADC1 (32-39): o ADC2 não funciona com o rádio ligado
#define LDR_INVERTIDO     0      // 1 se o valor CAI quando tem mais luz (comum em módulos LDR com saída AO)
#define PINO_TRIG         5
#define PINO_ECHO         18     // HC-SR04 de 5 V: use divisor de tensão no ECHO (ex.: 1k + 2k) -> 3,3 V
#define ALTURA_SENSOR_CM  50.0   // distância do sensor de ultrassom até a TERRA do vaso (meça com régua)
// ==================================================================



// Payload: id (MAC), contador, ValorLido1, ValorLido2, hop
// IGUAL em sensor, repetidor e gateway A.
typedef struct __attribute__((packed)) {
  uint8_t  id[6];
  uint32_t cont;
  float    v1;
  float    v2;
  uint8_t  hop;
} Mensagem;

Mensagem msg;
uint32_t contador = 0;

// Teste de estresse
bool     emTeste = false;
bool     testeConcluido = false;
uint32_t enviadosTeste = 0;
uint32_t proximoEnvioMs = 0;

// Estatísticas de enlace (ACK do repetidor)
volatile uint32_t ackOk = 0, ackFalha = 0;
volatile int      ultimoStatus = -1;   // -1 nada, 0 ok, 1 falha

// LED não bloqueante
uint32_t ledApagarEm = 0;

// ----------------------- leitura do sensor -----------------------
float ultimoV1 = NAN, ultimoV2 = NAN;
uint32_t ultimaLeituraMs = 0;

#if TIPO_SENSOR == SENSOR_REAL
float lerLuzPercentual() {
  uint32_t soma = 0;
  for (int i = 0; i < 8; i++) soma += analogRead(PINO_LDR);   // média de 8 leituras
  float pct = 100.0 * (soma / 8.0) / 4095.0;
  return LDR_INVERTIDO ? 100.0 - pct : pct;
}

float lerDistanciaCm() {
  float leituras[3];
  int n = 0;
  for (int i = 0; i < 3; i++) {
    digitalWrite(PINO_TRIG, LOW);  delayMicroseconds(2);
    digitalWrite(PINO_TRIG, HIGH); delayMicroseconds(10);
    digitalWrite(PINO_TRIG, LOW);
    unsigned long us = pulseIn(PINO_ECHO, HIGH, 30000);   // até ~5 m
    if (us > 0) leituras[n++] = us * 0.0343 / 2.0;
    delay(20);
  }
  if (n == 0) return NAN;
  // mediana simples para descartar eco espúrio
  for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++)
    if (leituras[j] < leituras[i]) { float t = leituras[i]; leituras[i] = leituras[j]; leituras[j] = t; }
  return leituras[n / 2];
}
#endif

void lerSensores(float &v1, float &v2) {
#if TIPO_SENSOR == SENSOR_REAL
  // Lê no máximo a cada 1 s; no teste de estresse reaproveita a última leitura.
  if (ultimaLeituraMs == 0 || millis() - ultimaLeituraMs >= 1000) {
    ultimaLeituraMs = millis();
    ultimoV1 = lerLuzPercentual();
    float d = lerDistanciaCm();
    if (isnan(d)) Serial.println("[AVISO] Ultrassom sem eco (confira ligação/alcance)");
    else          ultimoV2 = max(0.0f, (float)(ALTURA_SENSOR_CM - d));
  }
#else
  // Luz: "dia" comprimido em 10 min, com ruído. Altura: começa em 8 cm e cresce 0,2 cm/min.
  float t = millis() / 1000.0;
  ultimoV1 = constrain(50.0 + 40.0 * sin(2 * PI * t / 600.0) + random(-20, 21) / 10.0, 0.0, 100.0);
  ultimoV2 = 8.0 + 0.2 * (t / 60.0) + random(-3, 4) / 10.0;
#endif
  v1 = ultimoV1;
  v2 = ultimoV2;
}

// ----------------------- callback de envio -----------------------
// A assinatura mudou na versão 3.3 do core ESP32 do Arduino.
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 0)
void aoEnviar(const wifi_tx_info_t *info, esp_now_send_status_t status) {
#else
void aoEnviar(const uint8_t *mac, esp_now_send_status_t status) {
#endif
  if (status == ESP_NOW_SEND_SUCCESS) { ackOk++;    ultimoStatus = 0; }
  else                                { ackFalha++; ultimoStatus = 1; }
}

void enviar() {
  contador++;
  msg.cont = contador;
  float v1, v2;
  lerSensores(v1, v2);
  msg.v1 = v1;
  msg.v2 = v2;
  msg.hop = 0;
  esp_err_t r = esp_now_send(MAC_REPETIDOR, (uint8_t *)&msg, sizeof(msg));
  if (r != ESP_OK) {
    ackFalha++;
    ultimoStatus = 1;
  }
  if (!emTeste || contador % 50 == 0) {
    Serial.printf("cont=%lu v1=%.2f v2=%.2f  (ack ok=%lu falha=%lu)\n",
                  (unsigned long)msg.cont, msg.v1, msg.v2,
                  (unsigned long)ackOk, (unsigned long)ackFalha);
  }
}

void iniciarTeste() {
  emTeste = true;
  testeConcluido = false;
  contador = 0;          // a rodada sempre vai de 1 a TOTAL_TESTE
  enviadosTeste = 0;
  ackOk = ackFalha = 0;
  proximoEnvioMs = millis() + 1000;
  digitalWrite(PINO_LED, LOW);
  Serial.printf("\n=== TESTE DE ESTRESSE: %d mensagens, intervalo %d-%d ms ===\n",
                TOTAL_TESTE, TESTE_MIN_MS, TESTE_MAX_MS);
}

void setup() {
  Serial.begin(115200);
  pinMode(PINO_LED, OUTPUT);
  pinMode(PINO_BOTAO, INPUT_PULLUP);
  randomSeed(esp_random());

#if TIPO_SENSOR == SENSOR_REAL
  pinMode(PINO_TRIG, OUTPUT);
  pinMode(PINO_ECHO, INPUT);
  analogReadResolution(12);
#endif

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(CANAL_ESPNOW, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("Erro ao iniciar ESP-NOW. Reiniciando...");
    delay(2000);
    ESP.restart();
  }
  esp_now_register_send_cb(aoEnviar);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, MAC_REPETIDOR, 6);
  peer.channel = 0;          // 0 = canal atual
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("Erro ao adicionar o Repetidor como peer");
  }

  esp_read_mac(msg.id, ESP_MAC_WIFI_STA);
  Serial.printf("\nSENSOR pronto. Meu MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                msg.id[0], msg.id[1], msg.id[2], msg.id[3], msg.id[4], msg.id[5]);
  Serial.printf("Enviando para o repetidor %02X:%02X:%02X:%02X:%02X:%02X no canal %d\n",
                MAC_REPETIDOR[0], MAC_REPETIDOR[1], MAC_REPETIDOR[2],
                MAC_REPETIDOR[3], MAC_REPETIDOR[4], MAC_REPETIDOR[5], CANAL_ESPNOW);
  Serial.println("Aperte BOOT para iniciar o teste de estresse.");
}

void loop() {
  uint32_t agora = millis();

  // Botão BOOT inicia uma rodada de teste (com debounce simples)
  static uint32_t ultimoBotao = 0;
  if (digitalRead(PINO_BOTAO) == LOW && agora - ultimoBotao > 1000) {
    ultimoBotao = agora;
    iniciarTeste();
  }

  if (emTeste) {
    if ((int32_t)(agora - proximoEnvioMs) >= 0) {
      enviar();
      enviadosTeste++;
      proximoEnvioMs = agora + random(TESTE_MIN_MS, TESTE_MAX_MS + 1);
      if (enviadosTeste >= TOTAL_TESTE) {
        emTeste = false;
        testeConcluido = true;
        delay(300);  // espera os últimos ACKs
        Serial.printf("=== TESTE CONCLUÍDO: %lu enviadas | ACK do repetidor: ok=%lu falha=%lu ===\n",
                      (unsigned long)enviadosTeste, (unsigned long)ackOk, (unsigned long)ackFalha);
        Serial.println("O PDR fim-a-fim é medido no log do Node-RED (ferramentas/calcular_pdr.py).");
        digitalWrite(PINO_LED, HIGH);
      }
    }
  } else if (!testeConcluido) {
    if ((int32_t)(agora - proximoEnvioMs) >= 0) {
      proximoEnvioMs = agora + INTERVALO_NORMAL_MS;
      enviar();
    }
  }

  // Sinalização do status de envio
  if (ultimoStatus >= 0 && !testeConcluido) {
    digitalWrite(PINO_LED, HIGH);
    ledApagarEm = agora + (ultimoStatus == 0 ? 60 : 600);
    ultimoStatus = -1;
  }
  if (ledApagarEm && (int32_t)(agora - ledApagarEm) >= 0 && !testeConcluido) {
    digitalWrite(PINO_LED, LOW);
    ledApagarEm = 0;
  }
}
