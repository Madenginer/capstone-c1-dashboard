/*
 * ============================================================================
 * PROYEK CAPSTONE DESIGN - KELOMPOK C1 (DEPARTEMEN TEKNIK ELEKTRO FTEIC ITS)
 * Sistem Monitoring dan Pengendalian Sirkulasi Udara Miniatur Ruang Server IoT
 * ============================================================================
 * Mikrokontroler : ESP32 DevKit V1 (30-Pin / 38-Pin)
 * Sensor         : DHT22 / AM2302 (GPIO 4)
 * Aktuator       : Modul Relay 1-Channel Active-HIGH (GPIO 18) -> 2x Kipas 12V DC
 * Cloud Database : Supabase PostgreSQL REST API (HTTPS POST)
 * Alert System   : Telegram Bot API (HTTPS JSON POST)
 * ============================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <DHT.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ============================================================================
// 1. KONFIGURASI JARINGAN WI-FI & TELEGRAM BOT
// ============================================================================
const char* WIFI_SSID     = "Kost Putra Pertama 4g";
const char* WIFI_PASSWORD = "s3m0g414nc4rs3l4lu";

// Konfigurasi Supabase Cloud
const char* SUPABASE_URL  = "https://wpcfddrvzlhpmqjfiydp.supabase.co/rest/v1/sensor_logs";
const char* SUPABASE_KEY  = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6IndwY2ZkZHJ2emxocG1xamZpeWRwIiwicm9sZSI6ImFub24iLCJpYXQiOjE3OTM1MDk5NDEsImV4cCI6MjEwOTA4NTk0MX0.IesYOLYRz-jqgorDpcYLHAXX4iwwwUkgu7asvWFzSnM";

// Konfigurasi Telegram Bot (@capstone_c1_bot)
const String TELEGRAM_BOT_TOKEN = "9121748639:AAF16kFyw5_ZN-bDugo800EIjSvUicH89Oo";
const String TELEGRAM_CHAT_ID   = "5899050816";

// ============================================================================
// 2. DEFINISI PIN & AMBANG BATAS RESMI PROPOSAL CAPSTONE C1
// ============================================================================
#define DHTPIN          4       // Pin Data Sensor DHT22 terhubung ke GPIO 4
#define DHTTYPE         DHT22   // Tipe sensor: DHT22 (AM2302)
#define PIN_RELAY_FANS  18      // Pin IN Modul Relay terhubung ke GPIO 18
#define PIN_LED_STATUS  2       // LED onboard ESP32 (indikator status)

// Logika Modul Relay: Active-HIGH
#define RELAY_ON        HIGH    // Sinyal HIGH (3.3V) mengaktifkan relay & memutar kipas
#define RELAY_OFF       LOW     // Sinyal LOW (0.0V) mematikan relay & kipas

// Parameter Ambang Batas Termal Standar Resmi Proposal Capstone C1
const float TEMP_FAN_ON_THRESHOLD      = 28.0;    // Kipas Push-Pull ON saat T >= 28.0 °C
const float TEMP_FAN_OFF_THRESHOLD     = 27.5;    // Kipas Push-Pull OFF saat T < 27.5 °C (Histeresis 0.5 °C)
const float TEMP_CRITICAL_ALERT        = 30.0;    // Alert Telegram Darurat saat T >= 30.0 °C
const unsigned long TELEMETRY_INTERVAL = 5000;    // Siklus telemetri data: 5.000 ms (5 detik)
const unsigned long TELEGRAM_COOLDOWN  = 300000;  // Cooldown anti-spam Telegram: 5 menit (300.000 ms)

// ============================================================================
// 3. VARIABEL GLOBAL
// ============================================================================
DHT dht(DHTPIN, DHTTYPE);
WiFiClientSecure secureClient;

float currentTemp       = 0.0;
float currentHumidity   = 0.0;
bool  relayFanActive    = false;
bool  firstAlertSent    = false;

unsigned long lastCycleMillis = 0;
unsigned long lastAlertMillis = 0;
unsigned long lastWiFiRetry   = 0;

void connectToWiFi();
void sendToSupabaseCloud(float temp, float hum, bool fanState);
bool sendTelegramEmergencyAlert(float temp, float hum, bool fanState);

// ============================================================================
// 4. SETUP
// ============================================================================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Nonaktifkan detektor brownout hardware
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n=======================================================");
  Serial.println("  SISTEM MONITORING & KENDALI RUANG SERVER - KELOMPOK C1");
  Serial.println("         (FIRMWARE FINAL STANDAR RESMI PROPOSAL)        ");
  Serial.println("=======================================================");

  pinMode(PIN_RELAY_FANS, OUTPUT);
  pinMode(PIN_LED_STATUS, OUTPUT);
  digitalWrite(PIN_RELAY_FANS, RELAY_OFF);
  digitalWrite(PIN_LED_STATUS, LOW);

  dht.begin();
  secureClient.setInsecure(); // Bypass SSL cert verification once globally
  Serial.println("[INIT] Sensor DHT22 pada GPIO 4 siap.");
  Serial.println("[INIT] Modul Relay Kipas Active-HIGH pada GPIO 18 siap.");

  connectToWiFi();
}

// ============================================================================
// 5. LOOP UTAMA (ARSITEKTUR NON-BLOCKING MILLIS)
// ============================================================================
void loop() {
  unsigned long currentMillis = millis();

  // Reconnect Wi-Fi otomatis di latar belakang jika koneksi terputus
  if (WiFi.status() != WL_CONNECTED) {
    if (currentMillis - lastWiFiRetry >= 10000) {
      lastWiFiRetry = currentMillis;
      Serial.println("[WIFI] Mencoba menyambung kembali ke Wi-Fi...");
      WiFi.reconnect();
    }
  }

  // Siklus telemetri terjadwal tiap 5.000 ms
  if (currentMillis - lastCycleMillis >= TELEMETRY_INTERVAL) {
    lastCycleMillis = currentMillis;

    // 1. Akuisisi Data Suhu & Kelembapan dari DHT22
    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (isnan(t) || isnan(h)) {
      Serial.println("[WARN] Gagal membaca data DHT22! Periksa kabel GPIO 4.");
      return;
    }

    currentTemp = t;
    currentHumidity = h;

    Serial.println("-------------------------------------------------------");
    Serial.printf("[TELEMETRI] Suhu: %.1f C | Kelembapan: %.1f %%RH | Wi-Fi: %s\n",
                  currentTemp, currentHumidity, (WiFi.status() == WL_CONNECTED) ? "TERHUBUNG" : "TERPUTUS");

    // 2. Logika Kontrol Sirkulasi Kipas (Histeresis Termal Standar: 28.0 C / 27.5 C)
    if (currentTemp >= TEMP_FAN_ON_THRESHOLD) {
      if (!relayFanActive) {
        Serial.printf(">>> [KONTROL] Suhu (%.1f C) >= %.1f C -> KIPAS PUSH-PULL DIAKTIFKAN (RELAY ON)! <<<\n",
                      currentTemp, TEMP_FAN_ON_THRESHOLD);
      }
      relayFanActive = true;
      digitalWrite(PIN_RELAY_FANS, RELAY_ON);
      digitalWrite(PIN_LED_STATUS, HIGH);
    } else if (currentTemp < TEMP_FAN_OFF_THRESHOLD) {
      if (relayFanActive) {
        Serial.printf(">>> [KONTROL] Suhu (%.1f C) < %.1f C -> Kipas Push-Pull Dimatikan (RELAY OFF)! <<<\n",
                      currentTemp, TEMP_FAN_OFF_THRESHOLD);
      }
      relayFanActive = false;
      digitalWrite(PIN_RELAY_FANS, RELAY_OFF);
      digitalWrite(PIN_LED_STATUS, LOW);
    }

    Serial.printf("[STATUS] Kipas Push-Pull: %s | Pin 18: %s | LED ESP32: %s\n",
                  relayFanActive ? "AKTIF (MENYALA)" : "STANDBY (MATI)",
                  (digitalRead(PIN_RELAY_FANS) == HIGH) ? "HIGH (3.3V)" : "LOW (0.0V)",
                  digitalRead(PIN_LED_STATUS) ? "ON" : "OFF");

    // 3. Kirim Data Telemetri ke Supabase Cloud Tiap 5 Detik
    if (String(SUPABASE_URL).indexOf("xxxx") == -1) {
      sendToSupabaseCloud(currentTemp, currentHumidity, relayFanActive);
    }

    // 4. Logika Notifikasi Darurat Telegram (Standar Proposal: T >= 30.0 C)
    if (currentTemp >= TEMP_CRITICAL_ALERT) {
      if (!firstAlertSent || (currentMillis - lastAlertMillis >= TELEGRAM_COOLDOWN)) {
        Serial.println("[ALERT] Suhu melampaui ambang kritis 30.0 C! Mengirim notifikasi darurat Telegram...");
        if (sendTelegramEmergencyAlert(currentTemp, currentHumidity, relayFanActive)) {
          lastAlertMillis = currentMillis;
          firstAlertSent = true;
        }
      } else {
        unsigned long sisaCooldown = (TELEGRAM_COOLDOWN - (currentMillis - lastAlertMillis)) / 1000;
        Serial.printf("[ALERT] Suhu kritis, notifikasi dalam masa cooldown (%lu detik tersisa).\n", sisaCooldown);
      }
    }
  }
}

// ============================================================================
// 6. FUNGSI KONEKSI WI-FI
// ============================================================================
void connectToWiFi() {
  Serial.printf("[WIFI] Menghubungkan ke SSID: %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int retryCount = 0;
  while (WiFi.status() != WL_CONNECTED && retryCount < 25) {
    delay(500);
    Serial.print(".");
    digitalWrite(PIN_LED_STATUS, !digitalRead(PIN_LED_STATUS));
    retryCount++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(PIN_LED_STATUS, HIGH);
    Serial.println("\n[WIFI] Terhubung Berhasil!");
    Serial.print("[WIFI] Alamat IP ESP32: ");
    Serial.println(WiFi.localIP());
  } else {
    digitalWrite(PIN_LED_STATUS, LOW);
    Serial.println("\n[WIFI] Belum terhubung di awal. Sistem menjalankan kontrol lokal & auto-reconnect di latar belakang.");
  }
}

// ============================================================================
// 7. FUNGSI TRANSMISI DATA KE SUPABASE CLOUD
// ============================================================================
void sendToSupabaseCloud(float temp, float hum, bool fanState) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient https;
  https.setTimeout(6000);
  if (https.begin(secureClient, SUPABASE_URL)) {
    https.addHeader("Content-Type", "application/json");
    https.addHeader("apikey", SUPABASE_KEY);
    https.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
    https.addHeader("Prefer", "return=minimal");

    String jsonPayload = "{";
    jsonPayload += "\"temperature\":" + String(temp, 2) + ",";
    jsonPayload += "\"humidity\":" + String(hum, 2) + ",";
    jsonPayload += "\"fan_status\":" + String(fanState ? "true" : "false");
    jsonPayload += "}";

    int httpResponseCode = https.POST(jsonPayload);
    if (httpResponseCode >= 200 && httpResponseCode < 300) {
      Serial.printf("[SUPABASE] Sukses kirim data (HTTP %d)\n", httpResponseCode);
    } else {
      Serial.printf("[SUPABASE] Gagal kirim! Kode HTTP: %d\n", httpResponseCode);
    }
    https.end();
    secureClient.stop();
  }
}

// ============================================================================
// 8. FUNGSI PENGIRIMAN TELEGRAM BOT (NATIVE JSON POST)
// ============================================================================
bool sendTelegramEmergencyAlert(float temp, float hum, bool fanState) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient https;
  https.setTimeout(8000);

  String url = "https://api.telegram.org/bot" + TELEGRAM_BOT_TOKEN + "/sendMessage";
  
  String textContent = "⚠️ PERINGATAN SUHU KRITIS RUANG SERVER (C1) ⚠️\\n\\n";
  textContent += "🌡 Suhu Fisik: " + String(temp, 1) + " C (Batas Kritis: 30.0 C)\\n";
  textContent += "💧 Kelembapan: " + String(hum, 1) + " %RH\\n";
  textContent += "🌀 Status Kipas Push-Pull: " + String(fanState ? "AKTIF (ON)" : "MATI (OFF)") + "\\n\\n";
  textContent += "📍 Lokasi: Hardware Capstone C1 (Departemen Teknik Elektro FTEIC ITS)\\n";
  textContent += "Sistem kendali darurat telah mengaktifkan ventilasi sirkulasi udara.";

  String jsonPayload = "{\"chat_id\":\"" + TELEGRAM_CHAT_ID + "\",\"text\":\"" + textContent + "\"}";

  bool success = false;
  if (https.begin(secureClient, url)) {
    https.addHeader("Content-Type", "application/json");
    int httpCode = https.POST(jsonPayload);
    if (httpCode == 200) {
      Serial.printf("[TELEGRAM] Notifikasi darurat berhasil masuk ke HP Anas (HTTP %d)!\n", httpCode);
      success = true;
    } else {
      Serial.printf("[TELEGRAM] Respon server HTTP: %d\n", httpCode);
    }
    https.end();
    secureClient.stop();
  }
  return success;
}
