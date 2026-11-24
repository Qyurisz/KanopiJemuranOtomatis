#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <ESP32Servo.h>

// --- Konfigurasi WiFi & ThingsBoard ---
const char* ssid = "JAM 7 COFFE";
const char* password = "jamtujuh";
const char* mqtt_server = "thingsboard.cloud";
const char* tb_token = "NhWsjmBXL44ZGczSQMGU";

// --- Konfigurasi Pin ---
#define DHTPIN 4
#define DHTTYPE DHT11
#define RAINPIN 34      // Pin DO (Digital Output) dari Raindrop Sensor
#define LDRPIN 35       // Pin AO (Analog Output) dari Sensor LDR
#define SERVOPIN 18     // Pin PWM untuk Servo

// --- Konfigurasi Sudut Servo ---
const int SUDUT_BUKA = 90;
const int SUDUT_TUTUP = 0;

// --- Inisialisasi Objek ---
WiFiClient espClient;
PubSubClient client(espClient);
DHT dht(DHTPIN, DHTTYPE);
Servo atapServo;

unsigned long lastPublish = 0;
unsigned long lastReconnectAttempt = 0;
const unsigned long publishInterval = 3000;    // Kirim data tiap 3 detik
const unsigned long reconnectInterval = 5000;  // Coba konek ulang tiap 5 detik

bool atapTertutup = false; // Melacak status posisi atap saat ini

// --- Fungsi Callback untuk Menerima Perintah RPC dari ThingsBoard ---
void callback(char* topic, byte* payload, unsigned int length) {
  String pesan = "";
  for (unsigned int i = 0; i < length; i++) {
    pesan += (char)payload[i];
  }
  Serial.println("[RPC MASUK] Topic: " + String(topic));
  Serial.println("[RPC PAYLOAD] " + pesan);

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, pesan);
  if (error) {
    Serial.println("[RPC] Gagal parsing JSON");
    return;
  }

  const char* method = doc["method"];
  const char* params = doc["params"];

  if (method != nullptr && strcmp(method, "setAtap") == 0) {
    if (params != nullptr && strcmp(params, "TUTUP") == 0) {
      atapServo.write(SUDUT_TUTUP);
      atapTertutup = true;
      Serial.println("[AKSI RPC] Atap DITUTUP");
    } 
    else if (params != nullptr && strcmp(params, "BUKA") == 0) {
      atapServo.write(SUDUT_BUKA);
      atapTertutup = false;
      Serial.println("[AKSI RPC] Atap DIBUKA");
    }
  }
}

bool reconnectCloud() {
  Serial.print("Menghubungkan ke ThingsBoard... ");
  if (client.connect("ESP32ClientJemuran", tb_token, "")) {
    Serial.println("Berhasil!");
    client.subscribe("v1/devices/me/rpc/request/+");
    Serial.println("Subscribed ke RPC ThingsBoard.");
    return true;
  } else {
    Serial.print("Gagal, status rc=");
    Serial.println(client.state());
    return false;
  }
}

void setup() {
  Serial.begin(115200);

  // Inisialisasi Sensor & Aktuator
  dht.begin();
  pinMode(RAINPIN, INPUT);
  atapServo.attach(SERVOPIN);
  atapServo.write(SUDUT_BUKA);
  atapTertutup = false;

  // Koneksi WiFi
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);
  
  Serial.print("Menghubungkan WiFi");
  unsigned long startAttempt = millis();
  // Batasi waktu tunggu awal maksimal 10 detik agar tidak stuck jika WiFi mati
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  // Setup MQTT ThingsBoard
  client.setServer(mqtt_server, 1883);
  client.setBufferSize(512);
  client.setCallback(callback);
}

void loop() {
  unsigned long now = millis();
  int statusHujan = (digitalRead(RAINPIN) == LOW) ? 1 : 0;

  // --- 1. LOGIKA FAILSAFE LOKAL ---
  // Dijalankan terlebih dahulu sebelum mencoba koneksi ulang
  bool isOffline = (WiFi.status() != WL_CONNECTED || !client.connected());
  if (isOffline && statusHujan == 1 && !atapTertutup) {
    atapServo.write(SUDUT_TUTUP);
    atapTertutup = true;
    Serial.println("[FAILSAFE LOKAL] Koneksi terputus & Hujan terdeteksi! Atap DITUTUP otomatis.");
  }

  // --- 2. MANAJEMEN KONEKSI NON-BLOCKING ---
  if (WiFi.status() == WL_CONNECTED) {
    if (!client.connected()) {
      if (now - lastReconnectAttempt >= reconnectInterval) {
        lastReconnectAttempt = now;
        if (reconnectCloud()) {
          lastReconnectAttempt = 0;
        }
      }
    } else {
      client.loop();
    }
  }

  // --- 3. PUBLISH TELEMETRY ---
  if (client.connected() && (now - lastPublish >= publishInterval)) {
    lastPublish = now;

    float suhu = dht.readTemperature();
    float kelembapan = dht.readHumidity();
    if (isnan(suhu) || isnan(kelembapan)) {
      Serial.println("[DHT] Pembacaan gagal, menggunakan nilai 0");
      suhu = 0;
      kelembapan = 0;
    }

    int nilaiCahaya = analogRead(LDRPIN);

    JsonDocument doc;
    doc["suhu"] = suhu;
    doc["kelembapan"] = kelembapan;
    doc["hujan"] = statusHujan;
    doc["cahaya"] = nilaiCahaya;
    doc["atap_tertutup"] = atapTertutup; // Kirim status atap ke dashboard

    char payload[256];
    serializeJson(doc, payload, sizeof(payload));

    client.publish("v1/devices/me/telemetry", payload);
    Serial.print("[TELEMETRY] ");
    Serial.println(payload);
  }
}