/*
 * 00 - Descobrir o MAC de cada ESP32
 * -----------------------------------
 * Grave este sketch em CADA placa (sensor, repetidor, gateway A) e anote o MAC
 * impresso no Monitor Serial (115200). Etiquete as placas com fita crepe!
 * O MAC também sai no formato de array C, pronto para colar nos outros sketches.
 */
#include <WiFi.h>
#include <esp_mac.h>

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  delay(500);
}

void loop() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  Serial.printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X   ->   {0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X}\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  delay(2000);
}
